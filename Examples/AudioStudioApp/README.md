# Audio Studio (`Examples/AudioStudioApp`)

Zero-dependency, BSD-2-Clause, WebMCP-first audio workstation running directly on Ladybird App Mode (`LibWeb` + `WebAudio` + Canvas 2D + jailed binary `/__ladybird_fs` + per-account SQLite KV).

## Features

- **Pure WebAudio + Native ES Modules**: Zero third-party npm dependencies or bundler step required.
- **Deterministic Non-Blocking DSP & Editing**: Gain, Peak Normalization, Linear/Cosine Fade In/Out, Reverse, Phase Invert, RBJ Biquad Filters (`lowpass`, `highpass`, `bandpass`, `notch`), Trim, Cut, Silence, Trim Silence, and 20-step Undo/Redo.
- **Privacy-Safe Audio Telemetry (`--webmcp-only`)**: External MCP agents can synthesize, process, probe WAV headers via 44-byte HTTP `Range` reads, inspect spectral/loudness aggregates, manage persisted SQLite KV markers, and export WAVs without ever reading raw audio samples or inspecting the DOM.
- **Per-Account Isolation**: Each `--account <slug>` gets its own jailed `audio/` + `exports/` directories and SQLite `AppStore.db` (`markers:*`, `studio:prefs`).

## Quick Start

```bash
# Launch interactive GUI window
python ../../Meta/ladybird-app.py run . --account personal

# Launch headless in strict WebMCP-only privacy mode
python ../../Meta/ladybird-app.py connect . --account personal --headless --webmcp-only --json

# Promote background session to an interactive GUI window
python ../../Meta/ladybird-app.py connect . --account personal --gui

# Stop cleanly
python ../../Meta/ladybird-app.py stop . --account personal
```

## WebMCP Tools (14)

| Tool | Purpose |
| :--- | :--- |
| `get_studio_state` | Runtime info, account, browser capabilities, file counts, active document metadata |
| `list_audio_files` | Lists files under jailed `audio/` and `exports/` storage |
| `probe_wav_header` | Reads only the 44-byte RIFF/WAVE header via HTTP `Range` (`readBytes`) |
| `synthesize_audio` | Generates `sine`, `square`, `sawtooth`, `triangle`, `noise`, or `chord` WAVs |
| `open_audio_file` | Decodes and opens a stored file (`.wav`, `.m4a`, `.mp3`, `.ogg`, etc.) |
| `select_region` | Sets or clears `[startSec, endSec]` timeline selection |
| `analyze_audio` | Computes peak/RMS dBFS, crest factor, clipping, FFT centroid/dominant Hz, bands, and 64-bin envelope |
| `detect_silences` | Finds silent regions and optionally adds timeline markers |
| `apply_dsp` | Applies `gain`, `normalize`, `fade_in`, `fade_out`, `reverse`, `invert`, `lowpass`, `highpass`, `bandpass`, `notch`, `trim_silence` |
| `edit_region` | Applies `trim`, `cut`, or `silence` over a region or active selection |
| `manage_markers` | `list`, `add`, `remove`, or `clear` timeline markers persisted in SQLite KV |
| `undo_redo` | Steps `undo` or `redo` through non-destructive edit history |
| `export_wav` | Encodes PCM16 or Float32 WAV to `exports/` or `audio/` |
| `delete_audio_file` | Deletes a stored file under `audio/` or `exports/` |
