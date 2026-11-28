/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { AudioStudioEngine } from './src/engine.js';
import { WaveformView, SpectrumView } from './src/waveform.js';
import { registerAudioStudioWebMCP } from './src/webmcp.js';

const engine = new AudioStudioEngine();
registerAudioStudioWebMCP(engine);

const el = (id) => document.getElementById(id);

const waveformView = new WaveformView(el('waveform-canvas'), engine);
const spectrumView = new SpectrumView(el('spectrum-canvas'), engine);

function formatBytes(bytes) {
  if (!Number.isFinite(bytes)) return '0 B';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

function renderAccounts() {
  el('account-pill').textContent = engine.currentAccount?.id || 'default';
  const sel = el('account-select');
  sel.innerHTML = '';
  const list = engine.accounts.length > 0 ? engine.accounts : [engine.currentAccount];
  for (const acct of list) {
    const opt = document.createElement('option');
    opt.value = acct.id;
    opt.textContent = `${acct.label || acct.id} (${acct.id})`;
    if (acct.id === engine.currentAccount?.id) opt.selected = true;
    sel.appendChild(opt);
  }
}

function renderRuntime() {
  const badge = el('boot-badge');
  const sys = engine.systemInfo;
  if (engine.bootError) {
    badge.dataset.state = 'error';
    badge.textContent = 'error';
    el('runtime-meta').textContent = engine.bootError;
  } else {
    badge.dataset.state = 'ready';
    badge.textContent = 'ready';
    el('mcp-access-pill').textContent = `mcp:${sys?.mcpAccess || 'off'}`;
    el('runtime-meta').textContent = `origin ${window.location.origin} · mcpPort ${sys?.mcpPort || 0}`;
  }
}

function renderFiles() {
  const activePath = engine.getActiveDocument()?.sourcePath || null;

  const populate = (containerId, items) => {
    const ul = el(containerId);
    ul.innerHTML = '';
    if (!items || items.length === 0) {
      const li = document.createElement('li');
      li.className = 'muted-line';
      li.textContent = 'Empty';
      ul.appendChild(li);
      return;
    }
    for (const item of items) {
      const li = document.createElement('li');
      li.className = 'file-item' + (item.path === activePath ? ' active' : '');

      const openBtn = document.createElement('button');
      openBtn.type = 'button';
      openBtn.className = 'file-open-btn';
      openBtn.textContent = item.name;
      openBtn.title = `${item.path} (${formatBytes(item.size)})`;
      openBtn.addEventListener('click', () => {
        engine.openStoredFile(item.path).then(
          () => engine.log('gui', `Opened ${item.path}`),
          (err) => engine.log('gui', err.message, 'error')
        );
      });

      const meta = document.createElement('span');
      meta.className = 'file-meta';
      meta.textContent = formatBytes(item.size);

      const delBtn = document.createElement('button');
      delBtn.type = 'button';
      delBtn.className = 'icon-btn';
      delBtn.textContent = '×';
      delBtn.title = `Delete ${item.path}`;
      delBtn.addEventListener('click', () => {
        engine.deleteStoredFile(item.path).then(
          () => engine.log('gui', `Deleted ${item.path}`),
          (err) => engine.log('gui', err.message, 'error')
        );
      });

      li.append(openBtn, meta, delBtn);
      ul.appendChild(li);
    }
  };

  populate('audio-file-list', engine.files.audio);
  populate('export-file-list', engine.files.exports);
}

function renderDocumentTabs() {
  const bar = el('doc-tabs');
  bar.innerHTML = '';
  for (const doc of engine.documents) {
    const tab = document.createElement('div');
    tab.className = 'doc-tab' + (doc.id === engine.activeDocId ? ' active' : '');
    tab.setAttribute('role', 'tab');

    const titleBtn = document.createElement('button');
    titleBtn.type = 'button';
    titleBtn.className = 'file-open-btn';
    titleBtn.textContent = `${doc.name}${doc.dirty ? ' *' : ''}`;
    titleBtn.addEventListener('click', () => engine.setActiveDocument(doc.id));

    const closeBtn = document.createElement('button');
    closeBtn.type = 'button';
    closeBtn.className = 'icon-btn';
    closeBtn.textContent = '×';
    closeBtn.addEventListener('click', () => engine.closeDocument(doc.id));

    tab.append(titleBtn, closeBtn);
    bar.appendChild(tab);
  }
}

function renderTransportAndSelection() {
  const doc = engine.getActiveDocument();
  const dur = doc && doc.sampleRate > 0 ? doc.length / doc.sampleRate : 0;
  el('timecode').textContent = `${engine.transport.playheadSec.toFixed(3)}s / ${dur.toFixed(3)}s`;
  el('loop-btn').setAttribute('aria-pressed', String(engine.transport.loop));
  el('volume-slider').value = String(engine.transport.volume);
  el('zoom-reset-btn').textContent = `${waveformView.zoom.toFixed(1).replace(/\.0$/, '')}x`;

  const hasDoc = Boolean(doc);
  el('play-btn').disabled = !hasDoc;
  el('pause-btn').disabled = !hasDoc || !engine.transport.playing;
  el('stop-btn').disabled = !hasDoc;
  el('undo-btn').disabled = !doc || doc.undoStack.length === 0;
  el('redo-btn').disabled = !doc || doc.redoStack.length === 0;
  el('export-btn').disabled = !hasDoc;

  if (doc?.selection) {
    const span = doc.selection.endSec - doc.selection.startSec;
    el('selection-readout').textContent =
      `Selection: ${doc.selection.startSec.toFixed(3)}s → ${doc.selection.endSec.toFixed(3)}s (${span.toFixed(3)}s)`;
  } else if (doc) {
    el('selection-readout').textContent =
      `${doc.sampleRate} Hz · ${doc.channels.length} ch · ${doc.length} frames · ${dur.toFixed(3)}s`;
  } else {
    el('selection-readout').textContent = 'No selection (drag on waveform or call select_region)';
  }
}

function renderAnalysis() {
  const a = engine.getActiveDocument()?.lastAnalysis;
  if (!a) {
    for (const id of ['m-peak', 'm-rms', 'm-crest', 'm-dom', 'm-cent', 'm-clip']) el(id).textContent = '—';
    return;
  }
  el('m-peak').textContent = `${a.peakDb.toFixed(2)} dBFS`;
  el('m-rms').textContent = `${a.rmsDb.toFixed(2)} dBFS`;
  el('m-crest').textContent = `${a.crestFactorDb.toFixed(2)} dB`;
  el('m-dom').textContent = `${a.dominantFreqHz.toFixed(1)} Hz`;
  el('m-cent').textContent = `${a.spectralCentroidHz.toFixed(1)} Hz`;
  el('m-clip').textContent = String(a.clippingSamples);
}

function renderMarkers() {
  const ul = el('markers-list');
  ul.innerHTML = '';
  const doc = engine.getActiveDocument();
  if (!doc || doc.markers.length === 0) {
    const li = document.createElement('li');
    li.className = 'muted-line';
    li.textContent = 'No markers on active track.';
    ul.appendChild(li);
    return;
  }
  for (const m of doc.markers) {
    const li = document.createElement('li');
    li.className = 'marker-item';

    const jumpBtn = document.createElement('button');
    jumpBtn.type = 'button';
    jumpBtn.className = 'file-open-btn';
    jumpBtn.textContent = `${m.timeSec.toFixed(3)}s — ${m.label}`;
    jumpBtn.addEventListener('click', () => engine.seek(m.timeSec));

    const rmBtn = document.createElement('button');
    rmBtn.type = 'button';
    rmBtn.className = 'icon-btn';
    rmBtn.textContent = '×';
    rmBtn.addEventListener('click', () => engine.manageMarkers({ action: 'remove', id: m.id }));

    li.append(jumpBtn, rmBtn);
    ul.appendChild(li);
  }
}

function renderActivity() {
  const ul = el('activity-log');
  ul.innerHTML = '';
  for (const item of engine.activityLog) {
    const li = document.createElement('li');
    li.className = 'activity-entry';
    li.dataset.source = item.source;
    li.dataset.level = item.level;
    li.textContent = `[${item.ts}] [${item.source}] ${item.message}`;
    ul.appendChild(li);
  }
}

function renderAll(reason = 'all') {
  if (reason === 'playhead') {
    renderTransportAndSelection();
    waveformView.render();
    spectrumView.render();
    return;
  }
  renderAccounts();
  renderRuntime();
  renderFiles();
  renderDocumentTabs();
  renderTransportAndSelection();
  renderAnalysis();
  renderMarkers();
  renderActivity();
  waveformView.render();
  spectrumView.render();
}

engine.subscribe((reason) => renderAll(reason));

// Sidebar account & library wiring
el('open-account-btn').addEventListener('click', async () => {
  const target = el('account-select').value;
  if (target && engine.bridge) {
    await engine.bridge.accounts.launch(target, { headless: false }).catch((err) => engine.log('gui', err.message, 'error'));
  }
});

el('create-account-btn').addEventListener('click', async () => {
  const raw = el('new-account-input').value.trim();
  if (!raw || !engine.bridge) return;
  try {
    await engine.bridge.accounts.create(raw, raw);
    engine.accounts = await engine.bridge.accounts.list();
    el('new-account-input').value = '';
    engine.log('gui', `Created account '${raw}'`);
    renderAccounts();
  } catch (err) {
    engine.log('gui', err.message, 'error');
  }
});

el('synth-btn').addEventListener('click', async () => {
  const waveform = el('synth-waveform').value;
  const freq = Number(el('synth-freq').value) || 440;
  const durationSec = Number(el('synth-dur').value) || 2.0;
  const filename = `${waveform}-${Math.round(freq)}.wav`;
  try {
    await engine.synthesizeAudio({ filename, waveform, freq, durationSec, gainDb: -6, channels: 1 });
    engine.log('gui', `Synthesized audio/${filename}`);
  } catch (err) {
    engine.log('gui', err.message, 'error');
  }
});

el('import-file-input').addEventListener('change', async (e) => {
  const files = [...(e.target.files || [])];
  if (files.length === 0) return;
  try {
    await engine.importBrowserFiles(files);
    engine.log('gui', `Imported ${files.length} file(s)`);
  } catch (err) {
    engine.log('gui', err.message, 'error');
  }
  e.target.value = '';
});

el('refresh-files-btn').addEventListener('click', () => engine.refreshFiles());

// Transport & zoom
el('play-btn').addEventListener('click', () => engine.play());
el('pause-btn').addEventListener('click', () => engine.pause());
el('stop-btn').addEventListener('click', () => engine.stop());
el('loop-btn').addEventListener('click', () => engine.setLoop(!engine.transport.loop));
el('volume-slider').addEventListener('input', (e) => engine.setVolume(e.target.value));
el('zoom-in-btn').addEventListener('click', () => {
  waveformView.setZoom(waveformView.zoom * 1.5);
  renderTransportAndSelection();
});
el('zoom-out-btn').addEventListener('click', () => {
  waveformView.setZoom(waveformView.zoom / 1.5);
  renderTransportAndSelection();
});
el('zoom-reset-btn').addEventListener('click', () => {
  waveformView.setZoom(1);
  renderTransportAndSelection();
});
el('undo-btn').addEventListener('click', () => engine.undo());
el('redo-btn').addEventListener('click', () => engine.redo());
el('export-btn').addEventListener('click', async () => {
  try {
    const res = await engine.exportWav({});
    engine.log('gui', `Exported ${res.path} (${res.bytesWritten} B)`);
  } catch (err) {
    engine.log('gui', err.message, 'error');
  }
});
el('clear-selection-btn').addEventListener('click', () => engine.clearSelection());

// Dock tabs
for (const btn of document.querySelectorAll('.dock-tab')) {
  btn.addEventListener('click', () => {
    const target = btn.dataset.tab;
    for (const t of document.querySelectorAll('.dock-tab')) t.classList.toggle('active', t === btn);
    for (const p of document.querySelectorAll('.dock-pane')) p.classList.toggle('active', p.dataset.pane === target);
    spectrumView.render();
  });
}

// DSP & Edit buttons
el('dsp-gain-db').addEventListener('input', (e) => {
  el('dsp-gain-val').textContent = `${Number(e.target.value).toFixed(1)} dB`;
});

const runDspGui = (effect, paramsFn = () => ({})) => {
  try {
    engine.applyDsp(effect, paramsFn());
    engine.log('gui', `DSP ${effect} applied`);
  } catch (err) {
    engine.log('gui', err.message, 'error');
  }
};

el('apply-gain-btn').addEventListener('click', () => runDspGui('gain', () => ({ gainDb: Number(el('dsp-gain-db').value) })));
el('dsp-norm-btn').addEventListener('click', () => runDspGui('normalize', () => ({ targetDb: -1.0 })));
el('dsp-fadein-btn').addEventListener('click', () => runDspGui('fade_in', () => ({ durationSec: 0.25 })));
el('dsp-fadeout-btn').addEventListener('click', () => runDspGui('fade_out', () => ({ durationSec: 0.25 })));
el('dsp-reverse-btn').addEventListener('click', () => runDspGui('reverse'));
el('dsp-invert-btn').addEventListener('click', () => runDspGui('invert'));
el('dsp-trimsilence-btn').addEventListener('click', () => runDspGui('trim_silence', () => ({ thresholdDb: -45, padSec: 0.05 })));
el('apply-filter-btn').addEventListener('click', () =>
  runDspGui(el('filter-type').value, () => ({
    freq: Number(el('filter-freq').value) || 1200,
    q: Number(el('filter-q').value) || 0.707,
  }))
);

const runEditGui = (action) => {
  try {
    engine.editRegion(action);
    engine.log('gui', `Edit ${action} applied`);
  } catch (err) {
    engine.log('gui', err.message, 'error');
  }
};

el('edit-trim-btn').addEventListener('click', () => runEditGui('trim'));
el('edit-cut-btn').addEventListener('click', () => runEditGui('cut'));
el('edit-silence-btn').addEventListener('click', () => runEditGui('silence'));

el('detect-silence-btn').addEventListener('click', async () => {
  try {
    const res = await engine.detectSilencesActive({
      thresholdDb: Number(el('sil-thresh').value) || -40,
      minDurationSec: Number(el('sil-min-dur').value) || 0.25,
      addMarkers: true,
    });
    el('silence-result').textContent = `${res.count} silent segment(s)`;
    engine.log('gui', `Detected ${res.count} silent segment(s)`);
  } catch (err) {
    engine.log('gui', err.message, 'error');
  }
});

el('add-marker-btn').addEventListener('click', async () => {
  try {
    const label = el('marker-label-input').value.trim();
    await engine.manageMarkers({ action: 'add', label });
    el('marker-label-input').value = '';
  } catch (err) {
    engine.log('gui', err.message, 'error');
  }
});

el('clear-markers-btn').addEventListener('click', () => {
  engine.manageMarkers({ action: 'clear' }).catch((err) => engine.log('gui', err.message, 'error'));
});

window.addEventListener('resize', () => {
  waveformView.render();
  spectrumView.render();
});

engine.init();
