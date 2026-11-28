# Flow Studio (`FlowStudioApp`)

Local-first browser workflow automation studio built on **Ladybird App Mode** (`window.ladybirdApp` + WebMCP). Automates authenticated web sessions in isolated per-service containers without requiring third-party OAuth tokens.

## Capabilities (v1.0)

- **Dynamic Services & Multi-Account Containers**: Connect built-in or runtime services (`registerService` / `removeService`) with explicit `origins` and isolated `container` profiles (`src/main.js`, `ladybird-app.json`).
- **Control Flow & Resilience (`src/lib/runner.js`, `src/lib/workflow.js`)**:
  - Single-service native `FlowDefinition` execution and hybrid multi-service pipelines.
  - `IF / ELSE` conditional branching, `For Each` loops (`maxItems`), FIFO run queue, retries with backoff, `onError: 'continue'`, abort cancellation, `Pin Data`, `startFromStep` (`Run from here`), login pause/auto-resume (`AUTH_REQUIRED`), failure screenshots, and Human-in-the-Loop approval prompts.
- **Visual Recorder & Auto-Healing**:
  - `src/lib/recorder.js`: Normalizes semantic clicks, collapsed fills, key presses, and text extractions into steps.
  - `src/lib/healing.js`: Captures DOM candidate attributes on locator failures (`HEAL_INSPECT_SCRIPT`) and ranks semantic replacements (`rankHealCandidates`) with 1-click **Apply & Retry**.
- **Sandboxed Transforms, Encrypted Secrets & Scheduling**:
  - `src/lib/sandbox.js` + `src/lib/transform-worker.js`: Runs user `transform` code in a nested worker owned by a network-disabled, opaque-origin sandbox frame, with timeout termination.
  - `src/lib/secrets.js`: Stores encrypted secrets in SQLite KV (`flowstudio:secrets:v1`), injects `{{secrets.NAME}}` into HTTP steps, and redacts secret values from outputs/exports.
  - `src/lib/cron.js`: 5-field Cron expression parser and scheduler (`parseCronExpression`, `matchesCron`, `nextCronRuns`).
- **Inbound Local Webhooks & Portable FlowPacks**:
  - Webhook triggers bound to `POST /__ladybird_webhook/<triggerId>` with generated 32-char `X-Webhook-Token` / `Bearer` authentication and cooldown throttling.
  - `src/lib/packs.js`: Exports/imports sanitized `.flowpack.json` bundles with SHA-256 integrity verification (`sha256-...`) and permission/service/secret install diffs (`diffFlowPackInstall`).
- **Localization & Security Hygiene**:
  - `src/lib/i18n.js` + `src/lib/locales/{es,pt}.js`: Full `en` / `es` / `pt` translation parity and zero `innerHTML` usage across `src/`.

## WebMCP Tools (`navigator.modelContext`)

- `flowstudio_list_workflows`
- `flowstudio_list_site_actions`
- `flowstudio_list_triggers`
- `flowstudio_list_runs`
- `flowstudio_list_services`
- `flowstudio_list_secrets`
- `flowstudio_list_packs`
- `flowstudio_export_pack`
- `flowstudio_run_workflow`

## Running, Packaging & Testing

```bash
# Validate app manifest
python Meta/ladybird-app.py validate Examples/FlowStudioApp

# Launch interactively
python Meta/ladybird-app.py run Examples/FlowStudioApp --account personal

# Create a standalone portable bundle (add --zip for .zip archive + .sha256 checksum)
python Meta/ladybird-app.py package Examples/FlowStudioApp --out dist --runtime Build/release/bin --zip

# Run Flow Studio unit & static hygiene tests
node Examples/FlowStudioApp/tests/test_flowstudio.mjs

# Run consolidated Ladybird App Mode E2E suite (includes FlowStudioApp [7/7])
python Meta/test_app_mode_e2e.py
```
