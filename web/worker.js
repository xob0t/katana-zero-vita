// Builds KatanaZero.vpk in a worker: Pyodide runs builder.py and the repository's own scripts/ on the user's APK.
// The APK is read in place (WORKERFS) and never leaves the browser.
//   main -> worker: {cmd: 'inspect', file} | {cmd: 'build'}
//   worker -> main: {type: 'inspect', ok, version, warning, error} | {type: 'log', line}
//                   {type: 'progress', value, label} | {type: 'done', blob} | {type: 'error', stage, message}
import { loadPyodide } from 'https://cdn.jsdelivr.net/pyodide/v314.0.8/full/pyodide.mjs';

// The site is the repository itself (GitHub Pages serves this branch), so the scripts are the ones in scripts/
const PYTHON = ['builder.py', '../scripts/prepare_apk.py', '../scripts/patch_plt_calls.py',
  '../scripts/vita_glyphs.py', '../scripts/extract_art.py', '../scripts/make_livearea.py'];
// The prebuilt loader (build/app/eboot.bin and KatanaZero.vpk_param.sfo, copied to web/base/) and LiveArea layout
const BASE = { 'eboot.bin': 'base/eboot.bin', 'param.sfo': 'base/param.sfo', 'template.xml': '../livearea/template.xml' };

let pyPromise = null;
let apkPath = null;
let ready = false;

function post(m) { self.postMessage(m); }

async function fetchBytes(path) {
  const r = await fetch(path, { cache: 'no-cache' });
  if (!r.ok) throw new Error('could not download ' + path + ' (' + r.status + ')');
  return new Uint8Array(await r.arrayBuffer());
}

function getPython() {
  if (!pyPromise) {
    pyPromise = (async () => {
      const py = await loadPyodide();
      await py.loadPackage(['lz4', 'pillow', 'micropip'], { messageCallback: () => {} });
      py.FS.mkdirTree('/py');
      py.FS.mkdirTree('/base');
      for (const path of PYTHON) py.FS.writeFile('/py/' + path.split('/').pop(), await fetchBytes(path));
      for (const [name, path] of Object.entries(BASE)) py.FS.writeFile('/base/' + name, await fetchBytes(path));
      await py.runPythonAsync(`
import micropip, sys
await micropip.install("pyelftools==0.33")
sys.path.insert(0, "/py")
import builder
`);
      return py;
    })();
    pyPromise.catch(() => { pyPromise = null; });
  }
  return pyPromise;
}

function mountApk(py, file) {
  try { py.FS.unmount('/in'); } catch (e) { /* not mounted yet */ }
  py.FS.mkdirTree('/in');
  py.FS.mount(py.FS.filesystems.WORKERFS, { files: [file] }, '/in');
  return '/in/' + file.name;
}

async function inspect(file) {
  ready = false;
  const py = await getPython();
  apkPath = mountApk(py, file);
  const r = py.globals.get('builder').inspect(apkPath).toJs({ dict_converter: Object.fromEntries });
  ready = r.ok;
  post({ type: 'inspect', ok: r.ok, version: r.version, warning: r.warning, error: r.error });
}

async function build() {
  if (!ready) throw new Error('choose your APK first');
  const py = await getPython();
  const out = '/out/KatanaZero.vpk';
  py.FS.mkdirTree('/out');
  py.globals.get('builder').build(apkPath, out, '/base', true,
    (line) => post({ type: 'log', line }),
    (value, label) => post({ type: 'progress', value, label }));
  // MEMFS keeps the file in a typed array: wrap it without another copy, then drop the file
  const node = py.FS.lookupPath(out).node;
  const data = node.contents instanceof Uint8Array ? node.contents.subarray(0, node.usedBytes) : py.FS.readFile(out);
  const blob = new Blob([data], { type: 'application/octet-stream' });
  py.FS.unlink(out);
  post({ type: 'done', blob });
}

self.onmessage = (ev) => {
  const m = ev.data;
  const run = m.cmd === 'inspect' ? inspect(m.file) : m.cmd === 'build' ? build() : Promise.resolve();
  run.catch((e) => post({ type: 'error', stage: m.cmd, message: (e && e.message) || String(e) }));
};
