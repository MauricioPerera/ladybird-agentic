/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

export function registerAudioStudioWebMCP(engine) {
  let registered = false;

  const install = () => {
    if (registered) return;
    const ctx = document.modelContext || navigator.modelContext;
    if (!ctx || typeof ctx.registerTool !== 'function') return;
    registered = true;

    const wrap = (name, fn) => async (args = {}) => {
      try {
        const res = await fn(args || {});
        engine.log('webmcp', `${name} OK`);
        return res;
      } catch (err) {
        const msg = err?.message || String(err);
        engine.log('webmcp', `${name} FAILED: ${msg}`, 'error');
        throw err;
      }
    };

    ctx.registerTool({
      name: 'get_studio_state',
      description: 'Return Audio Studio runtime metadata, account, capabilities, jailed files, and active document summary.',
      inputSchema: { type: 'object', properties: {}, additionalProperties: false },
      execute: wrap('get_studio_state', async () => engine.getStateSummary()),
    });

    ctx.registerTool({
      name: 'list_audio_files',
      description: 'List audio and exported files stored in jailed per-account storage (audio/ and exports/).',
      inputSchema: {
        type: 'object',
        properties: {
          folder: { type: 'string', enum: ['all', 'audio', 'exports'], description: 'Filter folder (default: all)' },
        },
        additionalProperties: false,
      },
      execute: wrap('list_audio_files', async ({ folder = 'all' }) => {
        const files = await engine.refreshFiles();
        if (folder === 'audio') return { folder: 'audio', items: files.audio };
        if (folder === 'exports') return { folder: 'exports', items: files.exports };
        return files;
      }),
    });

    ctx.registerTool({
      name: 'probe_wav_header',
      description: 'Inspect RIFF/WAVE metadata of a stored file using a 44-byte HTTP Range read without decoding the full audio.',
      inputSchema: {
        type: 'object',
        properties: {
          path: { type: 'string', description: 'Relative path under audio/ or exports/ (e.g. audio/tone.wav)' },
        },
        required: ['path'],
        additionalProperties: false,
      },
      execute: wrap('probe_wav_header', async ({ path }) => engine.probeWavHeader(path)),
    });

    ctx.registerTool({
      name: 'synthesize_audio',
      description: 'Synthesize deterministic audio (sine, square, sawtooth, triangle, noise, chord), save as WAV under audio/, and open it.',
      inputSchema: {
        type: 'object',
        properties: {
          filename: { type: 'string', description: 'Target filename inside audio/ (e.g. qa-tone.wav)' },
          waveform: { type: 'string', enum: ['sine', 'square', 'sawtooth', 'triangle', 'noise', 'chord'] },
          freq: { type: 'number', description: 'Fundamental frequency in Hz (default: 440)' },
          durationSec: { type: 'number', description: 'Duration in seconds (default: 2.0)' },
          sampleRate: { type: 'number', description: 'Sample rate in Hz (default: 44100)' },
          gainDb: { type: 'number', description: 'Target peak level in dBFS (default: -6)' },
          channels: { type: 'integer', minimum: 1, maximum: 2, description: '1 = mono, 2 = stereo (default: 1)' },
          save: { type: 'boolean', description: 'Save WAV to audio/ storage (default: true)' },
          open: { type: 'boolean', description: 'Open as active document (default: true)' },
        },
        additionalProperties: false,
      },
      execute: wrap('synthesize_audio', async (args) => engine.synthesizeAudio(args)),
    });

    ctx.registerTool({
      name: 'open_audio_file',
      description: 'Decode and open a stored audio file from audio/ or exports/ as the active Audio Studio document.',
      inputSchema: {
        type: 'object',
        properties: {
          path: { type: 'string', description: 'Relative path (e.g. audio/qa-tone.wav)' },
        },
        required: ['path'],
        additionalProperties: false,
      },
      execute: wrap('open_audio_file', async ({ path }) => ({ document: await engine.openStoredFile(path) })),
    });

    ctx.registerTool({
      name: 'select_region',
      description: 'Select a time range [startSec, endSec] on the active document, or clear selection when clear=true.',
      inputSchema: {
        type: 'object',
        properties: {
          startSec: { type: 'number' },
          endSec: { type: 'number' },
          clear: { type: 'boolean' },
        },
        additionalProperties: false,
      },
      execute: wrap('select_region', async ({ startSec, endSec, clear }) => {
        const selection = clear ? engine.clearSelection() : engine.setSelection(startSec, endSec);
        return { selection };
      }),
    });

    ctx.registerTool({
      name: 'analyze_audio',
      description: 'Compute privacy-safe aggregate metrics (peakDb, rmsDb, crestFactorDb, clippingSamples, dominantFreqHz, spectralCentroidHz, bands, 64-bin envelope) on the active document or selection.',
      inputSchema: {
        type: 'object',
        properties: {
          useSelection: { type: 'boolean', description: 'Scope to current selection if present (default: true)' },
          startSec: { type: 'number' },
          endSec: { type: 'number' },
        },
        additionalProperties: false,
      },
      execute: wrap('analyze_audio', async (args) => engine.analyzeActive(args)),
    });

    ctx.registerTool({
      name: 'detect_silences',
      description: 'Detect silent segments below thresholdDb for at least minDurationSec; optionally adds timeline markers.',
      inputSchema: {
        type: 'object',
        properties: {
          thresholdDb: { type: 'number', description: 'Silence threshold in dBFS (default: -40)' },
          minDurationSec: { type: 'number', description: 'Minimum segment length in seconds (default: 0.25)' },
          addMarkers: { type: 'boolean', description: 'Create timeline markers at detected silences (default: false)' },
        },
        additionalProperties: false,
      },
      execute: wrap('detect_silences', async (args) => engine.detectSilencesActive(args)),
    });

    ctx.registerTool({
      name: 'apply_dsp',
      description: 'Apply deterministic DSP to the active document (or selection): gain, normalize, fade_in, fade_out, reverse, invert, lowpass, highpass, bandpass, notch, trim_silence.',
      inputSchema: {
        type: 'object',
        properties: {
          effect: {
            type: 'string',
            enum: ['gain', 'normalize', 'fade_in', 'fade_out', 'reverse', 'invert', 'lowpass', 'highpass', 'bandpass', 'notch', 'trim_silence'],
          },
          params: {
            type: 'object',
            description: 'Effect parameters (gainDb, targetDb, durationSec, curve, freq, q, thresholdDb, padSec, useSelection, startSec, endSec)',
          },
        },
        required: ['effect'],
        additionalProperties: false,
      },
      execute: wrap('apply_dsp', async ({ effect, params }) => ({ document: engine.applyDsp(effect, params || {}) })),
    });

    ctx.registerTool({
      name: 'edit_region',
      description: 'Edit the active document over a region or current selection: trim, cut (delete), or silence.',
      inputSchema: {
        type: 'object',
        properties: {
          action: { type: 'string', enum: ['trim', 'cut', 'silence'] },
          startSec: { type: 'number' },
          endSec: { type: 'number' },
        },
        required: ['action'],
        additionalProperties: false,
      },
      execute: wrap('edit_region', async ({ action, startSec, endSec }) => ({
        document: engine.editRegion(action, startSec, endSec),
      })),
    });

    ctx.registerTool({
      name: 'manage_markers',
      description: 'List, add, remove, or clear timeline markers on the active document (persisted in SQLite KV).',
      inputSchema: {
        type: 'object',
        properties: {
          action: { type: 'string', enum: ['list', 'add', 'remove', 'clear'] },
          timeSec: { type: 'number' },
          label: { type: 'string' },
          color: { type: 'string' },
          id: { type: 'string' },
        },
        required: ['action'],
        additionalProperties: false,
      },
      execute: wrap('manage_markers', async (args) => engine.manageMarkers(args)),
    });

    ctx.registerTool({
      name: 'undo_redo',
      description: 'Undo or redo the last DSP or region edit on the active document.',
      inputSchema: {
        type: 'object',
        properties: {
          action: { type: 'string', enum: ['undo', 'redo'] },
        },
        required: ['action'],
        additionalProperties: false,
      },
      execute: wrap('undo_redo', async ({ action }) => (action === 'redo' ? engine.redo() : engine.undo())),
    });

    ctx.registerTool({
      name: 'export_wav',
      description: 'Encode and export the active document (or selection) as PCM16 or Float32 WAV to exports/ (or audio/).',
      inputSchema: {
        type: 'object',
        properties: {
          filename: { type: 'string', description: 'Output filename (default: active document name .wav)' },
          folder: { type: 'string', enum: ['exports', 'audio'], description: 'Target folder (default: exports)' },
          bitDepth: { type: 'integer', enum: [16, 32], description: '16 = PCM16, 32 = Float32 (default: 16)' },
          selectionOnly: { type: 'boolean', description: 'Export only the current selection (default: false)' },
        },
        additionalProperties: false,
      },
      execute: wrap('export_wav', async (args) => engine.exportWav(args)),
    });

    ctx.registerTool({
      name: 'delete_audio_file',
      description: 'Delete a stored file from jailed audio/ or exports/ storage.',
      inputSchema: {
        type: 'object',
        properties: {
          path: { type: 'string', description: 'Relative path (e.g. audio/old.wav)' },
        },
        required: ['path'],
        additionalProperties: false,
      },
      execute: wrap('delete_audio_file', async ({ path }) => engine.deleteStoredFile(path)),
    });

    engine.emit('webmcp');
  };

  window.addEventListener('ladybird:webmcp-ready', install);
  install();
}
