// The page as a VCR: one `deck` object says where things stand, render() puts it on screen, and the work is done
// by worker.js, whose functions call() runs by name.
'use strict';

const el = Object.fromEntries([...document.querySelectorAll('[id]')].map((node) => [node.id, node]));

// tape: empty -> reading -> (rejected | recording -> (recorded | jammed)). Inserting an APK starts the build.
const deck = {
  tape: 'empty',
  note: null,         // [kind, text] under the slot
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
    recording: ['● REC', elapsed],
    recorded: ['▶ PLAY', elapsed],
    jammed: ['⏏ EJECT', null],
  }[tape];
}

// The VCR clock: local time, the colon blinking every second
function wallClock() {
  const now = new Date();
  const time = now.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
  return now.getSeconds() % 2 ? time.replace(':', ' ') : time;
}

function accepting() {
  return ['empty', 'rejected', 'jammed'].includes(deck.tape);
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

  note(el.status, deck.note);
  // INSERT TAPE only while there's no tape to work on: before a build, or after one was rejected or failed
  el.drop.disabled = !accepting();
  el.drop.dataset.tape = tape;
  el.hint.hidden = !accepting();
  el['drop-label'].textContent = { reading: 'Reading tape', recording: 'Recording', recorded: 'Recorded' }[tape]
    ?? 'Insert tape';

  el.progress.hidden = !['recording', 'recorded', 'jammed'].includes(tape);
  const filled = Math.round(deck.step[0] * 20);  // a VCR menu bar: 20 blocks
  el['progress-fill'].textContent = '█'.repeat(filled) + '░'.repeat(20 - filled);
  el['progress-text'].textContent = deck.step[1];

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

// An APK went in: check it, then record the VPK straight away
async function insert(file) {
  if (!file || !accepting()) return;
  if (deck.vpk) URL.revokeObjectURL(deck.vpk.url);
  Object.assign(deck, {
    tape: 'reading', vpk: null, log: [], step: [0, ''],
    note: ['', `Reading ${file.name} (${MB(file.size)}). The first time also loads the builder, about 20 MB.`],
  });
  render();
  let verdict;
  try {
    verdict = await call('check', file);
  } catch (e) {
    verdict = { ok: false, error: e.message };
  }
  if (!verdict.ok) {
    Object.assign(deck, { tape: 'rejected', note: ['err', verdict.error] });
    render();
    return;
  }
  await record(verdict.warning ? ['warn', verdict.warning] : ['ok', `Katana ZERO ${verdict.version}.`]);
}

async function record(note) {
  Object.assign(deck, { tape: 'recording', note, step: [0, 'Starting'], started: Date.now(), stopped: 0 });
  const tick = setInterval(render, 250);
  render();
  try {
    const { vpk } = await call('build');
    Object.assign(deck, {
      tape: 'recorded', step: [1, 'Done'], vpk: { url: URL.createObjectURL(vpk), size: vpk.size },
      note: ['ok', `KatanaZero.vpk is ready (${MB(vpk.size)}).`],
    });
  } catch (e) {
    Object.assign(deck, { tape: 'jammed', note: ['err', `${e.message} Insert the APK again to retry.`] });
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

// A file dropped anywhere on the page goes in. While one is dragged over the page, the screen says so.
let dragDepth = 0;
function dragging(on) {
  document.body.classList.toggle('dragging', on);
}
document.addEventListener('dragenter', (e) => {
  if (!accepting() || ![...e.dataTransfer.types].includes('Files')) return;
  dragDepth += 1;
  dragging(true);
});
document.addEventListener('dragleave', () => {
  dragDepth = Math.max(0, dragDepth - 1);
  if (dragDepth === 0) dragging(false);
});
document.addEventListener('dragover', (e) => e.preventDefault());
document.addEventListener('drop', (e) => {
  e.preventDefault();
  dragDepth = 0;
  dragging(false);
  insert(e.dataTransfer.files[0]);
});

setInterval(render, 1000);  // the clock
render();
