// Page flow: choose the APK -> the worker checks it -> build -> download. The work happens in worker.js.
(function () {
  'use strict';
  const $ = (id) => document.getElementById(id);
  const drop = $('drop'), input = $('file');
  const fileStatus = $('file-status'), buildStatus = $('build-status');
  const buildBtn = $('build'), stepBuild = $('step-build'), xbox = $('xbox');
  const progress = $('progress'), fill = $('progress-fill'), ptext = $('progress-text'), ptime = $('progress-time');
  const download = $('download'), logBox = $('log-box'), log = $('log');
  const osdMode = $('osd-mode'), osdCounter = $('osd-counter'), screen = $('screen'), screenArt = $('screen-art');

  let worker = null, busy = false, downloadUrl = null, artUrl = null, timer = null, started = 0;

  // The on-screen display plays along: deck mode on the left, a blinking 12:00 until there's a tape to count
  function osd(mode, counter) {
    osdMode.textContent = mode;
    osdMode.className = mode.startsWith('●') ? 'recording' : '';
    osdCounter.textContent = counter || '12:00';
    osdCounter.className = counter ? '' : 'blink';
  }

  function tapeTime(ms) {
    const s = Math.floor(ms / 1000);
    return Math.floor(s / 3600) + ':' + String(Math.floor(s / 60) % 60).padStart(2, '0') + ':' + String(s % 60).padStart(2, '0');
  }

  function status(el, kind, text) {
    el.hidden = false;
    el.className = 'msg ' + kind;
    el.textContent = text;
  }

  function sizeMB(n) { return (n / 1048576).toFixed(0) + ' MB'; }

  function setProgress(value, label) {
    fill.style.width = Math.round(Math.max(0, Math.min(1, value)) * 100) + '%';
    if (label) ptext.textContent = label;
  }

  function stopTimer() { clearInterval(timer); timer = null; buildBtn.classList.remove('on'); }

  function resetBuild() {
    buildBtn.disabled = true;
    stepBuild.setAttribute('aria-disabled', 'true');
    progress.hidden = true;
    buildStatus.hidden = true;
    download.hidden = true;
    screen.hidden = true;
    if (downloadUrl) { URL.revokeObjectURL(downloadUrl); downloadUrl = null; }
  }

  function getWorker() {
    if (worker) return worker;
    worker = new Worker('worker.js', { type: 'module' });
    worker.onmessage = onMessage;
    worker.onerror = (e) => {
      busy = false;
      stopTimer();
      status(buildStatus, 'err', 'The builder crashed (' + (e.message || 'unknown error') +
        '). The browser may have run out of memory: close other tabs and try again.');
      worker = null;
      osd('■ STOP');
    };
    return worker;
  }

  function onMessage(ev) {
    const m = ev.data;
    if (m.type === 'inspect') {
      busy = false;
      if (!m.ok) {
        status(fileStatus, 'err', m.error);
        osd('⏏ EJECT');
        return;
      }
      osd('■ STOP', '0:00:00');
      if (m.warning) status(fileStatus, 'warn', m.warning);
      else status(fileStatus, 'ok', 'Katana ZERO ' + m.version + '. Ready to build.');
      buildBtn.disabled = false;
      stepBuild.setAttribute('aria-disabled', 'false');
    } else if (m.type === 'log') {
      logBox.hidden = false;
      log.textContent += m.line + '\n';
    } else if (m.type === 'art') {
      if (artUrl) URL.revokeObjectURL(artUrl);
      artUrl = URL.createObjectURL(new Blob([m.data], { type: 'image/png' }));
      screenArt.src = artUrl;
    } else if (m.type === 'progress') {
      setProgress(m.value, m.label);
    } else if (m.type === 'done') {
      busy = false;
      stopTimer();
      setProgress(1, 'Done');
      downloadUrl = URL.createObjectURL(m.blob);
      download.href = downloadUrl;
      download.hidden = false;
      status(buildStatus, 'ok', 'KatanaZero.vpk is ready (' + sizeMB(m.blob.size) + ').');
      buildBtn.disabled = false;
      if (artUrl) screen.hidden = false;
      osd('▶ PLAY', tapeTime(Date.now() - started));
    } else if (m.type === 'error') {
      busy = false;
      stopTimer();
      status(m.stage === 'inspect' ? fileStatus : buildStatus, 'err', m.message);
      buildBtn.disabled = m.stage === 'inspect';
      osd('■ STOP');
    }
  }

  function choose(file) {
    if (!file || busy) return;
    resetBuild();
    busy = true;
    status(fileStatus, '', 'Reading ' + file.name + ' (' + sizeMB(file.size) +
      '). The first time also loads the builder, about 20 MB.');
    osd('◀◀ REW');
    getWorker().postMessage({ cmd: 'inspect', file: file });
  }

  input.addEventListener('change', () => { choose(input.files[0]); input.value = ''; });
  drop.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); input.click(); }
  });
  ['dragenter', 'dragover'].forEach((t) => drop.addEventListener(t, (e) => {
    e.preventDefault(); drop.classList.add('over');
  }));
  ['dragleave', 'drop'].forEach((t) => drop.addEventListener(t, (e) => {
    e.preventDefault(); drop.classList.remove('over');
  }));
  drop.addEventListener('drop', (e) => choose(e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0]));
  // a file dropped next to the box must not open it in the tab
  window.addEventListener('dragover', (e) => e.preventDefault());
  window.addEventListener('drop', (e) => e.preventDefault());

  buildBtn.addEventListener('click', () => {
    if (busy) return;
    busy = true;
    buildBtn.disabled = true;
    download.hidden = true;
    buildStatus.hidden = true;
    progress.hidden = false;
    log.textContent = '';
    if (downloadUrl) { URL.revokeObjectURL(downloadUrl); downloadUrl = null; }
    setProgress(0, 'Starting');
    screen.hidden = true;
    buildBtn.classList.add('on');
    started = Date.now();
    ptime.textContent = '0:00:00';
    osd('● REC', '0:00:00');
    timer = setInterval(() => {
      ptime.textContent = tapeTime(Date.now() - started);
      osd('● REC', ptime.textContent);
    }, 250);
    getWorker().postMessage({ cmd: 'build', glyphs: !xbox.checked });
  });
})();
