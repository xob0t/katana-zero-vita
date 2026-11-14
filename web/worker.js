// The builder, off the main thread. Pyodide runs builder.py and the repository's own scripts/ on the user's APK,
// which is read in place (WORKERFS) and never leaves the browser.
//
// The page calls the functions in `api` by name: it posts {id, fn, args} and gets back {id, result} or
// {id, failure}. While a build runs, {event: 'line', text} and {event: 'step', fraction, text} arrive in between.
import { loadPyodide } from 'https://cdn.jsdelivr.net/pyodide/v314.0.8/full/pyodide.mjs';

// GitHub Pages serves the repository itself, so these are the files in web/ and scripts/ of the same commit
const SCRIPTS = ['builder.py', '../scripts/prepare_apk.py', '../scripts/patch_plt_calls.py',
  '../scripts/vita_glyphs.py', '../scripts/extract_art.py', '../scripts/make_livearea.py'];
// The prebuilt loader (copied from build/app/ to web/base/) and the LiveArea layout, by their name in /base
const LOADER = { 'eboot.bin': 'base/eboot.bin', 'param.sfo': 'base/param.sfo', 'template.xml': '../livearea/template.xml' };
const VPK = '/out/KatanaZero.vpk';

let python = null;    // Promise of the Pyodide instance with builder imported
let apk = null;       // path of the mounted APK once check() accepted it

async function download(url) {
  const response = await fetch(url, { cache: 'no-cache' });
  if (!response.ok) throw new Error(`Could not download ${url} (HTTP ${response.status}).`);
  return new Uint8Array(await response.arrayBuffer());
}

async function startPython() {
  const py = await loadPyodide();
  // loadPackage reports a failed download to errorCallback instead of throwing
  const failed = [];
  await py.loadPackage(['lz4', 'pillow', 'micropip'], { messageCallback() {}, errorCallback: (m) => failed.push(m) });
  if (failed.length) throw new Error(`Could not download the builder. Check the connection and try again. (${failed[0]})`);
  for (const dir of ['/py', '/base', '/out']) py.FS.mkdirTree(dir);
  const files = [
    ...SCRIPTS.map((url) => [url, '/py/' + url.split('/').pop()]),
    ...Object.entries(LOADER).map(([name, url]) => [url, '/base/' + name]),
  ];
  for (const [url, path] of files) py.FS.writeFile(path, await download(url));
  await py.runPythonAsync('import micropip\nawait micropip.install("pyelftools==0.33")');
  py.runPython('import sys\nsys.path.insert(0, "/py")\nimport builder');
  return py;
}

function pythonReady() {
  python ??= startPython().catch((e) => { python = null; throw e; });
  return python;
}

const api = {
  // Mounts the APK and asks builder.inspect() whether it can be used
  async check(file) {
    apk = null;
    const py = await pythonReady();
    if (py.FS.analyzePath('/apk').exists) {
      try { py.FS.unmount('/apk'); } catch { /* only made, never mounted */ }
    } else {
      py.FS.mkdir('/apk');
    }
    py.FS.mount(py.FS.filesystems.WORKERFS, { files: [file] }, '/apk');
    const verdict = py.globals.get('builder').inspect('/apk/' + file.name).toJs({ dict_converter: Object.fromEntries });
    if (verdict.ok) apk = '/apk/' + file.name;
    return verdict;
  },

  // Builds the VPK and hands it over as a Blob
  async build() {
    if (!apk) throw new Error('Choose your APK first.');
    const py = await pythonReady();
    py.globals.get('builder').build(apk, VPK, '/base', true,
      (text) => self.postMessage({ event: 'line', text }),
      (fraction, text) => self.postMessage({ event: 'step', fraction, text }));
    // MEMFS holds the file in a typed array; the Blob takes it from there and the file goes
    const node = py.FS.lookupPath(VPK).node;
    const bytes = node.contents instanceof Uint8Array ? node.contents.subarray(0, node.usedBytes) : py.FS.readFile(VPK);
    const vpk = new Blob([bytes], { type: 'application/octet-stream' });
    py.FS.unlink(VPK);
    return { vpk };
  },
};

self.onmessage = async ({ data: { id, fn, args } }) => {
  try {
    self.postMessage({ id, result: await api[fn](...args) });
  } catch (e) {
    // A Python traceback goes to the build log; the page shows its last line
    let failure = (e && e.message) || String(e);
    if (failure.includes('Traceback (most recent call last)')) {
      self.postMessage({ event: 'line', text: failure });
      failure = failure.trim().split('\n').pop();
    }
    self.postMessage({ id, failure });
  }
};
