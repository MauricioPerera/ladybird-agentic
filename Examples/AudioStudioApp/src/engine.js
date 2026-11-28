/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import * as wav from './wav.js';
import * as dsp from './dsp.js';

const MAX_UNDO = 20;
const ALLOWED_DIRS = new Set(['audio', 'exports']);

export function normalizeRelPath(rawPath) {
  const cleaned = String(rawPath || '')
    .trim()
    .replace(/\\/g, '/')
    .replace(/^\/+/, '');
  if (!cleaned || cleaned.includes('..')) {
    throw new Error(`Invalid path: ${rawPath}`);
  }
  const top = cleaned.split('/')[0];
  if (!ALLOWED_DIRS.has(top)) {
    throw new Error(`Path must reside under audio/ or exports/: ${cleaned}`);
  }
  return cleaned;
}

export function sanitizeFilename(name, fallback = 'untitled.wav', forceWav = false) {
  let base = String(name || fallback)
    .replace(/^.*[\\/]/, '')
    .replace(/[<>:"/\\|?*\x00-\x1f]/g, '_')
    .trim();
  if (!base || base === '.' || base === '..') base = fallback;
  if (forceWav && !base.toLowerCase().endsWith('.wav')) {
    base = `${base}.wav`;
  }
  return base;
}

export class AudioStudioEngine {
  constructor() {
    this.listeners = new Set();
    this.documents = [];
    this.activeDocId = null;
    this.nextDocNum = 1;
    this.files = { audio: [], exports: [] };
    this.systemInfo = null;
    this.accounts = [];
    this.currentAccount = { id: 'default', label: 'Default', profileName: 'default' };
    this.activityLog = [];
    this.bootError = null;
    this.ready = false;

    this.transport = {
      playing: false,
      playheadSec: 0,
      loop: false,
      volume: 0.85,
      ctx: null,
      source: null,
      gainNode: null,
      analyserNode: null,
      startedAtCtxTime: 0,
      startOffsetSec: 0,
      rafId: 0,
    };
  }

  get bridge() {
    return typeof window !== 'undefined' && window.ladybirdApp ? window.ladybirdApp : null;
  }

  subscribe(fn) {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  emit(reason = 'state') {
    for (const fn of this.listeners) {
      try {
        fn(reason, this);
      } catch (err) {
        console.error('AudioStudioEngine listener error:', err);
      }
    }
  }

  log(source, message, level = 'info') {
    const entry = {
      ts: new Date().toISOString().slice(11, 19),
      source,
      level,
      message,
    };
    this.activityLog.unshift(entry);
    if (this.activityLog.length > 80) this.activityLog.length = 80;
    this.emit('log');
    return entry;
  }

  async init() {
    const b = this.bridge;
    if (!b) {
      this.bootError = 'Ladybird App Mode bridge is unavailable';
      this.ready = true;
      this.emit('init');
      return;
    }

    try {
      await Promise.all([
        b.fs.mkdir('audio').catch(() => {}),
        b.fs.mkdir('exports').catch(() => {}),
      ]);

      const [sys, acctCur, acctList, prefsEntry] = await Promise.all([
        b.system.info().catch(() => null),
        b.accounts.current().catch(() => null),
        b.accounts.list().catch(() => []),
        b.kv.get('studio:prefs').catch(() => null),
      ]);

      const prefs = prefsEntry?.found ? prefsEntry.value : null;
      this.systemInfo = sys;
      if (acctCur) this.currentAccount = acctCur;
      if (Array.isArray(acctList)) this.accounts = acctList;
      if (prefs && typeof prefs === 'object') {
        if (typeof prefs.volume === 'number') this.transport.volume = Math.max(0, Math.min(1, prefs.volume));
        if (typeof prefs.loop === 'boolean') this.transport.loop = prefs.loop;
      }

      await this.refreshFiles();
      this.log('system', `Ready (${this.currentAccount.id}, mcp=${sys?.mcpAccess || 'unknown'})`);
    } catch (err) {
      this.bootError = err?.message || String(err);
      this.log('system', `Boot error: ${this.bootError}`, 'error');
    } finally {
      this.ready = true;
      this.emit('init');
    }
  }

  async refreshFiles() {
    const b = this.bridge;
    if (!b) return this.files;

    const loadFolder = async (folder) => {
      await b.fs.mkdir(folder).catch(() => {});
      const listing = await b.fs.listDir(folder).catch(() => ({ entries: [] }));
      const items = [];
      for (const entry of listing.entries || []) {
        if (entry.isDirectory) continue;
        const path = `${folder}/${entry.name}`;
        let st = null;
        try {
          st = await b.fs.stat(path);
        } catch {}
        items.push({
          name: entry.name,
          folder,
          path,
          size: st?.size ?? entry.size ?? 0,
          modifiedAtMs: st?.modifiedAtMs ?? 0,
        });
      }
      items.sort((a, bItem) => a.name.localeCompare(bItem.name));
      return items;
    };

    const [audio, exports] = await Promise.all([loadFolder('audio'), loadFolder('exports')]);
    this.files = { audio, exports };
    this.emit('files');
    return this.files;
  }

  getActiveDocument() {
    return this.documents.find((d) => d.id === this.activeDocId) || null;
  }

  summarizeDoc(doc) {
    return {
      id: doc.id,
      name: doc.name,
      sourcePath: doc.sourcePath,
      sampleRate: doc.sampleRate,
      channels: doc.channels.length,
      frames: doc.length,
      durationSec: doc.sampleRate > 0 ? Number((doc.length / doc.sampleRate).toFixed(4)) : 0,
      bitDepth: doc.bitDepth,
      dirty: doc.dirty,
      undoCount: doc.undoStack.length,
      redoCount: doc.redoStack.length,
      selection: doc.selection ? { ...doc.selection } : null,
      markers: doc.markers.map((m) => ({ ...m })),
      lastAnalysis: doc.lastAnalysis,
    };
  }

  getStateSummary() {
    const active = this.getActiveDocument();
    return {
      origin: window.location.origin,
      ready: this.ready,
      bootError: this.bootError,
      system: this.systemInfo,
      account: this.currentAccount,
      accounts: this.accounts,
      capabilities: {
        hasWebGL2: Boolean(document.createElement('canvas').getContext('webgl2')),
        hasAudioContext: typeof AudioContext === 'function',
        hasAudioWorklet: typeof AudioWorkletNode === 'function',
        hasWebAssembly: typeof WebAssembly === 'object',
      },
      transport: {
        playing: this.transport.playing,
        playheadSec: Number(this.transport.playheadSec.toFixed(4)),
        loop: this.transport.loop,
        volume: this.transport.volume,
      },
      files: {
        audioCount: this.files.audio.length,
        exportsCount: this.files.exports.length,
        audio: this.files.audio,
        exports: this.files.exports,
      },
      documents: this.documents.map((d) => this.summarizeDoc(d)),
      activeDocumentId: this.activeDocId,
      activeDocument: active ? this.summarizeDoc(active) : null,
    };
  }

  async probeWavHeader(rawPath) {
    const b = this.bridge;
    if (!b) throw new Error('Ladybird bridge unavailable');
    const relPath = normalizeRelPath(rawPath);
    const st = await b.fs.stat(relPath);
    if (!st?.exists) {
      throw new Error(`File not found: ${relPath}`);
    }
    const headerBytes = await b.fs.readBytes(relPath, { offset: 0, count: 44 });
    const parsed = wav.parseWavHeader(headerBytes, st.size);
    return {
      path: relPath,
      fileSize: st.size,
      modifiedAtMs: st.modifiedAtMs || 0,
      ...parsed,
    };
  }

  async createOrReplaceDocument({ name, sourcePath, sampleRate, channels, bitDepth = 16, dirty = false }) {
    const length = channels[0]?.length || 0;
    let markers = [];
    if (sourcePath && this.bridge) {
      try {
        const saved = await this.bridge.kv.get(`markers:${sourcePath}`);
        if (saved?.found && Array.isArray(saved.value)) markers = saved.value;
      } catch {}
    }

    const existing = sourcePath ? this.documents.find((d) => d.sourcePath === sourcePath) : null;
    const analysis = dsp.analyzeAudio(channels, sampleRate);

    if (existing) {
      existing.name = name;
      existing.sampleRate = sampleRate;
      existing.channels = channels;
      existing.length = length;
      existing.bitDepth = bitDepth;
      existing.dirty = dirty;
      existing.undoStack = [];
      existing.redoStack = [];
      existing.selection = null;
      existing.markers = markers;
      existing.lastAnalysis = analysis;
      this.activeDocId = existing.id;
      this.transport.playheadSec = 0;
      this.emit('document');
      return existing;
    }

    const doc = {
      id: `doc-${this.nextDocNum++}`,
      name,
      sourcePath,
      sampleRate,
      channels,
      length,
      bitDepth,
      dirty,
      undoStack: [],
      redoStack: [],
      selection: null,
      markers,
      lastAnalysis: analysis,
    };
    this.documents.push(doc);
    this.activeDocId = doc.id;
    this.transport.playheadSec = 0;
    this.emit('document');
    return doc;
  }

  async synthesizeAudio(opts = {}) {
    const cleanName = sanitizeFilename(
      opts.filename || `${opts.waveform || 'sine'}-${Math.round(opts.freq || 440)}.wav`,
      'tone.wav',
      true
    );
    const { sampleRate, channels } = dsp.generateTone(opts);
    const wavBytes = wav.encodeWav(channels, sampleRate, 16);

    const shouldSave = opts.save !== false;
    const shouldOpen = opts.open !== false;
    let saved = null;
    let relPath = null;

    if (shouldSave) {
      if (!this.bridge) throw new Error('Ladybird bridge unavailable');
      relPath = `audio/${cleanName}`;
      await this.bridge.fs.mkdir('audio').catch(() => {});
      const writeRes = await this.bridge.fs.writeBytes(relPath, wavBytes);
      saved = {
        path: relPath,
        bytesWritten: writeRes?.bytesWritten ?? wavBytes.byteLength,
      };
      await this.refreshFiles();
    }

    let doc = null;
    if (shouldOpen) {
      this.stop();
      doc = await this.createOrReplaceDocument({
        name: cleanName,
        sourcePath: relPath,
        sampleRate,
        channels,
        bitDepth: 16,
        dirty: false,
      });
    }

    return {
      saved,
      document: doc ? this.summarizeDoc(doc) : null,
      analysis: doc ? doc.lastAnalysis : dsp.analyzeAudio(channels, sampleRate),
    };
  }

  async openStoredFile(rawPath) {
    const b = this.bridge;
    if (!b) throw new Error('Ladybird bridge unavailable');
    const relPath = normalizeRelPath(rawPath);
    const bytes = new Uint8Array(await b.fs.readBytes(relPath));
    const isRiff =
      bytes.byteLength >= 12 &&
      bytes[0] === 0x52 &&
      bytes[1] === 0x49 &&
      bytes[2] === 0x46 &&
      bytes[3] === 0x46 &&
      bytes[8] === 0x57 &&
      bytes[9] === 0x41 &&
      bytes[10] === 0x56 &&
      bytes[11] === 0x45;

    let decoded;
    if (relPath.toLowerCase().endsWith('.wav') || isRiff) {
      decoded = wav.decodeWavBytes(bytes);
    } else {
      const ctx = this.ensureAudioContext();
      if (!ctx) {
        throw new Error('AudioContext is required to decode non-WAV audio');
      }
      const audioBuf = await ctx.decodeAudioData(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
      const chs = [];
      for (let c = 0; c < audioBuf.numberOfChannels; c++) {
        chs.push(new Float32Array(audioBuf.getChannelData(c)));
      }
      decoded = {
        sampleRate: audioBuf.sampleRate,
        channels: chs,
        bitDepth: 32,
        length: audioBuf.length,
      };
    }

    this.stop();
    const name = relPath.split('/').pop();
    const doc = await this.createOrReplaceDocument({
      name,
      sourcePath: relPath,
      sampleRate: decoded.sampleRate,
      channels: decoded.channels,
      bitDepth: decoded.bitDepth,
      dirty: false,
    });
    return this.summarizeDoc(doc);
  }

  async importBrowserFiles(fileList) {
    const b = this.bridge;
    if (!b) throw new Error('Ladybird bridge unavailable');
    await b.fs.mkdir('audio').catch(() => {});
    const opened = [];
    for (const file of fileList) {
      const cleanName = sanitizeFilename(file.name, 'imported.wav', false);
      const relPath = `audio/${cleanName}`;
      const buf = new Uint8Array(await file.arrayBuffer());
      await b.fs.writeBytes(relPath, buf);
      const summary = await this.openStoredFile(relPath);
      opened.push(summary);
    }
    await this.refreshFiles();
    return opened;
  }

  setActiveDocument(docId) {
    const target = this.documents.find((d) => d.id === docId);
    if (!target) throw new Error(`Document not found: ${docId}`);
    if (this.activeDocId !== docId) {
      this.stop();
      this.activeDocId = docId;
      this.transport.playheadSec = 0;
      this.emit('document');
    }
    return this.summarizeDoc(target);
  }

  closeDocument(docId) {
    const idx = this.documents.findIndex((d) => d.id === docId);
    if (idx === -1) return false;
    if (this.activeDocId === docId) this.stop();
    this.documents.splice(idx, 1);
    if (this.activeDocId === docId) {
      const fallback = this.documents[idx] || this.documents[idx - 1] || null;
      this.activeDocId = fallback ? fallback.id : null;
      this.transport.playheadSec = 0;
    }
    this.emit('document');
    return true;
  }

  setSelection(startSec, endSec) {
    const doc = this.requireActiveDoc();
    const dur = doc.sampleRate > 0 ? doc.length / doc.sampleRate : 0;
    if (startSec == null || endSec == null) {
      doc.selection = null;
      this.emit('selection');
      return null;
    }
    let s = Math.max(0, Math.min(dur, Number(startSec)));
    let e = Math.max(0, Math.min(dur, Number(endSec)));
    if (e < s) [s, e] = [e, s];
    if (Math.abs(e - s) < 1e-4) {
      doc.selection = null;
    } else {
      doc.selection = { startSec: Number(s.toFixed(4)), endSec: Number(e.toFixed(4)) };
    }
    this.emit('selection');
    return doc.selection ? { ...doc.selection } : null;
  }

  clearSelection() {
    return this.setSelection(null, null);
  }

  requireActiveDoc() {
    const doc = this.getActiveDocument();
    if (!doc) throw new Error('No active audio document open');
    return doc;
  }

  pushUndoSnapshot(doc) {
    doc.undoStack.push({
      channels: dsp.cloneChannels(doc.channels),
      selection: doc.selection ? { ...doc.selection } : null,
      markers: doc.markers.map((m) => ({ ...m })),
    });
    if (doc.undoStack.length > MAX_UNDO) doc.undoStack.shift();
    doc.redoStack = [];
  }

  commitChannelUpdate(doc, nextChannels, nextSelection = doc.selection) {
    if (this.transport.playing) this.stop();
    doc.channels = nextChannels;
    doc.length = nextChannels[0]?.length || 0;
    const dur = doc.sampleRate > 0 ? doc.length / doc.sampleRate : 0;
    if (this.transport.playheadSec > dur) {
      this.transport.playheadSec = Math.max(0, dur);
    }
    if (nextSelection) {
      const s = Math.max(0, Math.min(dur, nextSelection.startSec));
      const e = Math.max(s, Math.min(dur, nextSelection.endSec));
      doc.selection = e - s > 1e-4 ? { startSec: Number(s.toFixed(4)), endSec: Number(e.toFixed(4)) } : null;
    } else {
      doc.selection = null;
    }
    doc.dirty = true;
    doc.lastAnalysis = dsp.analyzeAudio(doc.channels, doc.sampleRate);
    this.emit('audio');
  }

  applyDsp(effect, params = {}) {
    const doc = this.requireActiveDoc();
    const kind = String(effect || '').toLowerCase();
    const useSelection = params.useSelection === true || (params.useSelection !== false && params.startSec != null);
    const range =
      params.startSec != null && params.endSec != null
        ? { startSec: Number(params.startSec), endSec: Number(params.endSec) }
        : useSelection && doc.selection
          ? doc.selection
          : null;

    this.pushUndoSnapshot(doc);
    let nextChannels;

    switch (kind) {
      case 'gain':
        nextChannels = dsp.applyGain(doc.channels, Number(params.gainDb ?? 0), range, doc.sampleRate);
        break;
      case 'normalize':
        nextChannels = dsp.normalizePeak(doc.channels, Number(params.targetDb ?? -1.0), range, doc.sampleRate);
        break;
      case 'fade_in':
        nextChannels = dsp.applyFade(
          doc.channels,
          { direction: 'in', durationSec: Number(params.durationSec ?? 0.25), curve: params.curve || 'linear' },
          doc.sampleRate,
          range
        );
        break;
      case 'fade_out':
        nextChannels = dsp.applyFade(
          doc.channels,
          { direction: 'out', durationSec: Number(params.durationSec ?? 0.25), curve: params.curve || 'linear' },
          doc.sampleRate,
          range
        );
        break;
      case 'reverse':
        nextChannels = dsp.reverseAudio(doc.channels, range, doc.sampleRate);
        break;
      case 'invert':
        nextChannels = dsp.invertPhase(doc.channels, range, doc.sampleRate);
        break;
      case 'lowpass':
      case 'highpass':
      case 'bandpass':
      case 'notch':
        nextChannels = dsp.biquadFilter(
          doc.channels,
          doc.sampleRate,
          { type: kind, freq: Number(params.freq ?? 1000), q: Number(params.q ?? 0.707) },
          range
        );
        break;
      case 'trim_silence':
        nextChannels = dsp.trimSilence(doc.channels, doc.sampleRate, {
          thresholdDb: Number(params.thresholdDb ?? -45),
          padSec: Number(params.padSec ?? 0.05),
        });
        break;
      default:
        doc.undoStack.pop();
        throw new Error(`Unsupported DSP effect: ${effect}`);
    }

    this.commitChannelUpdate(doc, nextChannels, kind === 'trim_silence' ? null : doc.selection);
    return this.summarizeDoc(doc);
  }

  editRegion(action, startSec = null, endSec = null) {
    const doc = this.requireActiveDoc();
    const act = String(action || '').toLowerCase();
    const s = startSec != null ? Number(startSec) : doc.selection?.startSec;
    const e = endSec != null ? Number(endSec) : doc.selection?.endSec;
    if (!Number.isFinite(s) || !Number.isFinite(e) || Math.abs(e - s) <= 0) {
      throw new Error('A valid startSec/endSec or active selection is required');
    }
    const low = Math.min(s, e);
    const high = Math.max(s, e);

    this.pushUndoSnapshot(doc);
    let nextChannels;
    let nextSelection = null;

    if (act === 'trim') {
      nextChannels = dsp.trimRange(doc.channels, doc.sampleRate, low, high);
      const newDur = nextChannels[0].length / doc.sampleRate;
      doc.markers = doc.markers
        .map((m) => ({ ...m, timeSec: Number((m.timeSec - low).toFixed(4)) }))
        .filter((m) => m.timeSec >= 0 && m.timeSec <= newDur);
    } else if (act === 'cut') {
      nextChannels = dsp.cutRange(doc.channels, doc.sampleRate, low, high, 'delete');
      const cutDur = high - low;
      doc.markers = doc.markers
        .filter((m) => m.timeSec < low || m.timeSec > high)
        .map((m) => (m.timeSec > high ? { ...m, timeSec: Number((m.timeSec - cutDur).toFixed(4)) } : { ...m }));
    } else if (act === 'silence') {
      nextChannels = dsp.cutRange(doc.channels, doc.sampleRate, low, high, 'silence');
      nextSelection = { startSec: low, endSec: high };
    } else {
      doc.undoStack.pop();
      throw new Error(`Unsupported edit action: ${action}`);
    }

    this.commitChannelUpdate(doc, nextChannels, nextSelection);
    return this.summarizeDoc(doc);
  }

  undo() {
    const doc = this.requireActiveDoc();
    if (doc.undoStack.length === 0) {
      return { applied: false, document: this.summarizeDoc(doc) };
    }
    doc.redoStack.push({
      channels: dsp.cloneChannels(doc.channels),
      selection: doc.selection ? { ...doc.selection } : null,
      markers: doc.markers.map((m) => ({ ...m })),
    });
    const prev = doc.undoStack.pop();
    doc.markers = prev.markers;
    this.commitChannelUpdate(doc, prev.channels, prev.selection);
    return { applied: true, document: this.summarizeDoc(doc) };
  }

  redo() {
    const doc = this.requireActiveDoc();
    if (doc.redoStack.length === 0) {
      return { applied: false, document: this.summarizeDoc(doc) };
    }
    doc.undoStack.push({
      channels: dsp.cloneChannels(doc.channels),
      selection: doc.selection ? { ...doc.selection } : null,
      markers: doc.markers.map((m) => ({ ...m })),
    });
    const next = doc.redoStack.pop();
    doc.markers = next.markers;
    this.commitChannelUpdate(doc, next.channels, next.selection);
    return { applied: true, document: this.summarizeDoc(doc) };
  }

  analyzeActive({ useSelection = true, startSec = null, endSec = null } = {}) {
    const doc = this.requireActiveDoc();
    const range =
      startSec != null && endSec != null
        ? { startSec: Number(startSec), endSec: Number(endSec) }
        : useSelection && doc.selection
          ? doc.selection
          : null;
    const analysis = dsp.analyzeAudio(doc.channels, doc.sampleRate, range);
    if (!range) {
      doc.lastAnalysis = analysis;
      this.emit('analysis');
    }
    return {
      documentId: doc.id,
      name: doc.name,
      scopedToSelection: Boolean(range),
      selection: range,
      analysis,
    };
  }

  async detectSilencesActive({ thresholdDb = -40, minDurationSec = 0.25, addMarkers = false } = {}) {
    const doc = this.requireActiveDoc();
    const silences = dsp.detectSilences(doc.channels, doc.sampleRate, { thresholdDb, minDurationSec });
    if (addMarkers && silences.length > 0) {
      for (let i = 0; i < silences.length; i++) {
        await this.manageMarkers({
          action: 'add',
          timeSec: silences[i].startSec,
          label: `Silence ${i + 1} (${silences[i].durationSec}s)`,
          color: '#38bdf8',
        });
      }
    }
    return { documentId: doc.id, count: silences.length, silences, markers: doc.markers };
  }

  async persistDocMarkers(doc) {
    if (!this.bridge || !doc.sourcePath) return;
    await this.bridge.kv.set(`markers:${doc.sourcePath}`, doc.markers).catch(() => {});
  }

  async manageMarkers({ action = 'list', timeSec = null, label = '', color = '#f59e0b', id = null } = {}) {
    const doc = this.requireActiveDoc();
    const act = String(action || 'list').toLowerCase();
    const dur = doc.sampleRate > 0 ? doc.length / doc.sampleRate : 0;

    if (act === 'add') {
      const t = Math.max(0, Math.min(dur, Number(timeSec ?? this.transport.playheadSec)));
      const marker = {
        id: `m-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 6)}`,
        timeSec: Number(t.toFixed(4)),
        label: String(label || `Marker ${doc.markers.length + 1}`).slice(0, 80),
        color: String(color || '#f59e0b'),
      };
      doc.markers.push(marker);
      doc.markers.sort((a, b) => a.timeSec - b.timeSec);
      await this.persistDocMarkers(doc);
      this.emit('markers');
      return { action: 'add', marker, markers: doc.markers };
    }

    if (act === 'remove') {
      const before = doc.markers.length;
      doc.markers = doc.markers.filter((m) => m.id !== id && m.label !== label);
      await this.persistDocMarkers(doc);
      this.emit('markers');
      return { action: 'remove', removed: before - doc.markers.length, markers: doc.markers };
    }

    if (act === 'clear') {
      doc.markers = [];
      await this.persistDocMarkers(doc);
      this.emit('markers');
      return { action: 'clear', markers: [] };
    }

    return { action: 'list', markers: doc.markers };
  }

  async exportWav({ filename = null, folder = 'exports', bitDepth = 16, selectionOnly = false } = {}) {
    const b = this.bridge;
    if (!b) throw new Error('Ladybird bridge unavailable');
    const doc = this.requireActiveDoc();
    const targetFolder = folder === 'audio' ? 'audio' : 'exports';
    const baseFallback = doc.name.replace(/\.[^.]+$/, '') + '.wav';
    const cleanName = sanitizeFilename(filename || baseFallback, baseFallback, true);
    const relPath = `${targetFolder}/${cleanName}`;

    let channelsToWrite = doc.channels;
    if (selectionOnly && doc.selection) {
      channelsToWrite = dsp.trimRange(doc.channels, doc.sampleRate, doc.selection.startSec, doc.selection.endSec);
    }

    const depth = Number(bitDepth) === 32 ? 32 : 16;
    const wavBytes = wav.encodeWav(channelsToWrite, doc.sampleRate, depth);
    await b.fs.mkdir(targetFolder).catch(() => {});
    const res = await b.fs.writeBytes(relPath, wavBytes);

    if (!selectionOnly) {
      doc.dirty = false;
    }
    await this.refreshFiles();
    this.emit('document');

    const frames = channelsToWrite[0]?.length || 0;
    return {
      path: relPath,
      bytesWritten: res?.bytesWritten ?? wavBytes.byteLength,
      sampleRate: doc.sampleRate,
      channels: channelsToWrite.length,
      frames,
      durationSec: doc.sampleRate > 0 ? Number((frames / doc.sampleRate).toFixed(4)) : 0,
      bitDepth: depth,
    };
  }

  async deleteStoredFile(rawPath) {
    const b = this.bridge;
    if (!b) throw new Error('Ladybird bridge unavailable');
    const relPath = normalizeRelPath(rawPath);
    await b.fs.deleteFile(relPath);
    await this.refreshFiles();
    return { deleted: relPath };
  }

  ensureAudioContext() {
    if (this.transport.ctx) return this.transport.ctx;
    if (typeof AudioContext !== 'function') return null;
    try {
      const ctx = new AudioContext();
      const gainNode = ctx.createGain();
      gainNode.gain.value = this.transport.volume;
      const analyserNode = ctx.createAnalyser();
      analyserNode.fftSize = 1024;
      gainNode.connect(analyserNode);
      analyserNode.connect(ctx.destination);
      this.transport.ctx = ctx;
      this.transport.gainNode = gainNode;
      this.transport.analyserNode = analyserNode;
      return ctx;
    } catch {
      return null;
    }
  }

  async play(fromSec = null) {
    const doc = this.getActiveDocument();
    if (!doc || doc.length === 0) return false;
    const ctx = this.ensureAudioContext();
    if (!ctx) return false;

    if (ctx.state === 'suspended') {
      await ctx.resume().catch(() => {});
    }

    this.stopSourceOnly();

    const dur = doc.length / doc.sampleRate;
    let offset = fromSec != null ? Number(fromSec) : this.transport.playheadSec;
    if (offset >= dur - 0.005) offset = doc.selection ? doc.selection.startSec : 0;
    offset = Math.max(0, Math.min(dur, offset));

    const audioBuffer = ctx.createBuffer(doc.channels.length, doc.length, doc.sampleRate);
    for (let c = 0; c < doc.channels.length; c++) {
      audioBuffer.copyToChannel(doc.channels[c], c);
    }

    const src = ctx.createBufferSource();
    src.buffer = audioBuffer;
    src.loop = this.transport.loop;
    if (this.transport.loop && doc.selection) {
      src.loopStart = doc.selection.startSec;
      src.loopEnd = doc.selection.endSec;
    }
    src.connect(this.transport.gainNode);

    this.transport.source = src;
    this.transport.playing = true;
    this.transport.startedAtCtxTime = ctx.currentTime;
    this.transport.startOffsetSec = offset;
    this.transport.playheadSec = offset;

    src.onended = () => {
      if (this.transport.source === src && this.transport.playing) {
        this.transport.playing = false;
        this.transport.playheadSec = 0;
        this.stopRaf();
        this.emit('transport');
      }
    };

    try {
      src.start(0, offset);
      this.startRaf();
      this.emit('transport');
      return true;
    } catch {
      this.transport.playing = false;
      this.transport.source = null;
      return false;
    }
  }

  pause() {
    if (!this.transport.playing) return;
    this.updatePlayheadFromClock();
    this.stopSourceOnly();
    this.transport.playing = false;
    this.stopRaf();
    this.emit('transport');
  }

  stop() {
    this.stopSourceOnly();
    this.transport.playing = false;
    this.transport.playheadSec = 0;
    this.stopRaf();
    this.emit('transport');
  }

  seek(sec) {
    const doc = this.getActiveDocument();
    const dur = doc && doc.sampleRate > 0 ? doc.length / doc.sampleRate : 0;
    const clamped = Math.max(0, Math.min(dur, Number(sec) || 0));
    const wasPlaying = this.transport.playing;
    this.transport.playheadSec = clamped;
    if (wasPlaying) {
      this.play(clamped);
    } else {
      this.emit('transport');
    }
  }

  setVolume(vol) {
    const v = Math.max(0, Math.min(1, Number(vol)));
    this.transport.volume = v;
    if (this.transport.gainNode) {
      this.transport.gainNode.gain.value = v;
    }
    this.savePrefs();
    this.emit('transport');
  }

  setLoop(enabled) {
    this.transport.loop = Boolean(enabled);
    if (this.transport.source) {
      this.transport.source.loop = this.transport.loop;
    }
    this.savePrefs();
    this.emit('transport');
  }

  savePrefs() {
    if (!this.bridge) return;
    this.bridge.kv
      .set('studio:prefs', {
        volume: this.transport.volume,
        loop: this.transport.loop,
      })
      .catch(() => {});
  }

  stopSourceOnly() {
    if (this.transport.source) {
      const s = this.transport.source;
      this.transport.source = null;
      try {
        s.onended = null;
        s.stop();
        s.disconnect();
      } catch {}
    }
  }

  updatePlayheadFromClock() {
    const doc = this.getActiveDocument();
    if (!doc || !this.transport.ctx || !this.transport.playing) return;
    const elapsed = this.transport.ctx.currentTime - this.transport.startedAtCtxTime;
    const dur = doc.length / doc.sampleRate;
    let pos = this.transport.startOffsetSec + elapsed;
    if (dur > 0) {
      if (this.transport.loop) {
        if (doc.selection && doc.selection.endSec > doc.selection.startSec) {
          const span = doc.selection.endSec - doc.selection.startSec;
          pos = doc.selection.startSec + (Math.max(0, pos - doc.selection.startSec) % span);
        } else {
          pos = pos % dur;
        }
      } else {
        pos = Math.min(dur, pos);
      }
    }
    this.transport.playheadSec = pos;
  }

  startRaf() {
    this.stopRaf();
    const tick = () => {
      if (!this.transport.playing) return;
      this.updatePlayheadFromClock();
      this.emit('playhead');
      this.transport.rafId = requestAnimationFrame(tick);
    };
    this.transport.rafId = requestAnimationFrame(tick);
  }

  stopRaf() {
    if (this.transport.rafId) {
      cancelAnimationFrame(this.transport.rafId);
      this.transport.rafId = 0;
    }
  }
}
