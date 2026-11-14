// The page as a VCR: one `deck` object says where things stand, render() puts it on screen, and the work is done
// by worker.js, whose functions call() runs by name.
'use strict';

const el = Object.fromEntries([...document.querySelectorAll('[id]')].map((node) => [node.id, node]));

// tape: empty -> reading -> (rejected | loaded) -> recording -> (recorded | jammed)
const deck = {
  tape: 'empty',
  fileNote: null,     // [kind, text] under the slot
  buildNote: null,    // [kind, text] under the build button
  step: [0, ''],      // progress fraction and label while recording
  started: 0,
  stopped: 0,
  vpk: null,          // {url, size} once recorded
  log: [],
};

const MB = (bytes) => `${Math.round(bytes / 1048576)} MB`;

function clock(ms) {
  const t = Math.max(0, Math.floor(ms / 1000));
  const mm = String(Math.floor(t / 60) % 60).padStart(2, '0');
  const ss = String(t % 60).padStart(2, '0');
  return `${Math.floor(t / 3600)}:${mm}:${ss}`;
}

// What the on-screen display shows for each tape state: [mode, counter]; without a counter it shows the time
function osdFor(tape) {
  const elapsed = clock((deck.stopped || Date.now()) - deck.started);
  return {
    empty: ['⏏ NO TAPE', null],
    reading: ['◀◀ REW', null],
    rejected: ['⏏ EJECT', null],
    loaded: ['■ STOP', '0:00:00'],
    recording: ['● REC', elapsed],
    recorded: ['▶ PLAY', elapsed],
    jammed: ['■ STOP', null],
  }[tape];
}

// The VCR clock: local time, the colon blinking every second
function wallClock() {
  const now = new Date();
  const time = now.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
  return now.getSeconds() % 2 ? time.replace(':', ' ') : time;
}

function note(node, value) {
  node.hidden = !value;
  if (value) {
    node.className = 'msg ' + value[0];
    node.textContent = value[1];
  }
}

function render() {
  const { tape } = deck;
  const [mode, counter] = osdFor(tape);
  el['osd-mode'].textContent = mode;
  el['osd-mode'].classList.toggle('recording', tape === 'recording');
  el['osd-counter'].textContent = counter ?? wallClock();

  note(el['file-status'], deck.fileNote);
  note(el['build-status'], deck.buildNote);

  const canRecord = ['loaded', 'recorded', 'jammed'].includes(tape);
  el['step-build'].setAttribute('aria-disabled', String(!canRecord && tape !== 'recording'));
  el.build.disabled = !canRecord;
  el.build.classList.toggle('on', tape === 'recording');

  el.progress.hidden = !['recording', 'recorded', 'jammed'].includes(tape);
  el['progress-fill'].style.width = `${Math.round(deck.step[0] * 100)}%`;
  el['progress-text'].textContent = deck.step[1];
  el['progress-time'].textContent = counter ?? '';

  el.download.hidden = tape !== 'recorded';
  if (deck.vpk) el.download.href = deck.vpk.url;

  el['log-box'].hidden = deck.log.length === 0;
  el.log.textContent = deck.log.join('\n');
}

// --- the worker ---------------------------------------------------------------------------------------------------

let worker = null;
const waiting = new Map();
let lastId = 0;

function onWorkerMessage({ data }) {
  if (data.event === 'line') {
    deck.log.push(data.text);
  } else if (data.event === 'step') {
    deck.step = [data.fraction, data.text];
  } else {
    const { resolve, reject } = waiting.get(data.id);
    waiting.delete(data.id);
    if ('failure' in data) reject(new Error(data.failure));
    else resolve(data.result);
    return;
  }
  render();
}

function onWorkerCrash(event) {
  // usually the tab running out of memory; every pending call fails and the next call starts a fresh worker
  const reason = `The builder stopped (${event.message || 'no details'}). The browser may have run out of memory: ` +
    'close other tabs and try again.';
  for (const { reject } of waiting.values()) reject(new Error(reason));
  waiting.clear();
  worker.terminate();
  worker = null;
}

function call(fn, ...args) {
  if (!worker) {
    worker = new Worker('worker.js', { type: 'module' });
    worker.addEventListener('message', onWorkerMessage);
    worker.addEventListener('error', onWorkerCrash);
  }
  const id = ++lastId;
  return new Promise((resolve, reject) => {
    waiting.set(id, { resolve, reject });
    worker.postMessage({ id, fn, args });
  });
}

// --- actions ------------------------------------------------------------------------------------------------------

async function insert(file) {
  if (!file || deck.tape === 'reading' || deck.tape === 'recording') return;
  if (deck.vpk) URL.revokeObjectURL(deck.vpk.url);
  Object.assign(deck, {
    tape: 'reading', vpk: null, buildNote: null, log: [], step: [0, ''],
    fileNote: ['', `Reading ${file.name} (${MB(file.size)}). The first time also loads the builder, about 20 MB.`],
  });
  render();
  try {
    const verdict = await call('check', file);
    if (!verdict.ok) {
      Object.assign(deck, { tape: 'rejected', fileNote: ['err', verdict.error] });
    } else {
      deck.tape = 'loaded';
      deck.fileNote = verdict.warning ? ['warn', verdict.warning] : ['ok', `Katana ZERO ${verdict.version}. Ready to build.`];
    }
  } catch (e) {
    Object.assign(deck, { tape: 'rejected', fileNote: ['err', e.message] });
  }
  render();
}

async function record() {
  if (deck.vpk) URL.revokeObjectURL(deck.vpk.url);
  Object.assign(deck, { tape: 'recording', vpk: null, buildNote: null, log: [], step: [0, 'Starting'],
    started: Date.now(), stopped: 0 });
  const tick = setInterval(render, 250);
  render();
  try {
    const { vpk } = await call('build');
    Object.assign(deck, {
      tape: 'recorded', step: [1, 'Done'], vpk: { url: URL.createObjectURL(vpk), size: vpk.size },
      buildNote: ['ok', `KatanaZero.vpk is ready (${MB(vpk.size)}).`],
    });
  } catch (e) {
    Object.assign(deck, { tape: 'jammed', buildNote: ['err', e.message] });
  } finally {
    deck.stopped = Date.now();
    clearInterval(tick);
    render();
  }
}

// --- wiring -------------------------------------------------------------------------------------------------------

el.drop.addEventListener('click', () => el.file.click());
el.file.addEventListener('change', () => {
  insert(el.file.files[0]);
  el.file.value = '';  // choosing the same file again still fires 'change'
});

// Drag and drop: the slot lights up while a file is over it. A file dropped anywhere else on the page is ignored
// instead of being opened by the browser.
for (const type of ['dragover', 'drop']) document.addEventListener(type, (e) => e.preventDefault());
el.drop.addEventListener('dragover', () => el.drop.classList.add('over'));
el.drop.addEventListener('dragleave', () => el.drop.classList.remove('over'));
el.drop.addEventListener('drop', (e) => {
  el.drop.classList.remove('over');
  insert(e.dataTransfer.files[0]);
});

el.build.addEventListener('click', record);

setInterval(render, 1000);  // the clock
render();
