# Ladybird App Mode & Agentic Runtime

Ladybird App Mode turns local web directories or remote origins into isolated, multi-account desktop applications with native SQLite storage, jailed filesystem access, embedded MCP automation, and WebMCP privacy controls — without requiring Electron or a separate backend server.

## Architecture Overview

| Component | Location | Responsibility |
| :--- | :--- | :--- |
| Manifest parser & validator | `Libraries/LibWebView/AppMode/AppManifest.{h,cpp}` | Parses `ladybird-app.json`, confines relative asset paths, validates `mcp.access` & permissions, and derives a source-bound storage ID from the canonical app root or remote origin |
| Loopback app server & IPC bridge | `Libraries/LibWebView/AppMode/AppModeServer.{h,cpp}` | Serves local assets over deterministic loopback ports, injects `/__ladybird_bridge.js` (`window.ladybirdApp` + synchronous WebMCP shim), enforces Origin + `X-Ladybird-App-Token` on `/__ladybird_ipc` |
| Persistent KV store | `Libraries/LibWebView/AppMode/AppKVStore.{h,cpp}` | SQLite-backed JSON key-value store (`AppStore.db`) scoped per app and account profile |
| Multi-account registry | `Libraries/LibWebView/AppMode/AppAccountRegistry.{h,cpp}` | Tracks registered account slugs, labels, timestamps, and profile mappings in `<data>/AppAccounts/<storage-id>.json` |
| Application & runtime discovery | `Libraries/LibWebView/Application.{h,cpp}` | Synchronous App Mode MCP launch, `--account`, `--mcp-webmcp-only`, and live `<data>/AppRuntime/<storage-id>--<account-id>.json` tracking |
| GUI tab integration | `Services/Hub/Application.cpp`, `UI/Qt/BrowserWindow.cpp` | Chromeless window sizing/icon/title and automatic `MCPAccessMode` policy grant (`Full`, `WebMCPOnly` when `--mcp-webmcp-only` / `mcp.access: "webmcp-only"`, or container policy ceiling) on non-private App Mode views |
| MCP server privacy boundary | `Libraries/LibMCP/{MCPDelegate.h,MCPServer.cpp}` | Enforces `webmcp-only` boundary across `initialize`, `tools/list`, `tools/call`, `resources/*`, and `skills/*` |
| Developer & lifecycle CLI | `Meta/ladybird-app.py` | `init`, `validate`, `run`, `connect`, `status`, `stop`, `accounts`, `package` |

---

## Manifest Schema (`ladybird-app.json`)

```json
{
  "id": "my-app",
  "name": "My App",
  "version": "0.1.0",
  "entry": "index.html",
  "icon": "icon.svg",
  "window": {
    "width": 1080,
    "height": 720,
    "minWidth": 360,
    "minHeight": 240,
    "resizable": true,
    "showTabBar": false
  },
  "mcp": {
    "enabled": true,
    "port": 0,
    "allowWebMCP": true,
    "access": "full",
    "allowedDomains": []
  },
  "permissions": {
    "network": "unrestricted",
    "crossOriginIsolated": false,
    "fsRead": ["workspace"],
    "fsWrite": ["workspace"],
    "storage": true,
    "clipboardWrite": false,
    "systemInfo": true,
    "dynamicServices": false
  },
  "services": {
    "mail": {
      "label": "Mail",
      "defaultUrl": "https://mail.example.com/",
      "origins": ["https://mail.example.com"],
      "container": "personal",
      "permissions": ["open", "navigate", "notifications", "eval", "flows", "network"]
    }
  }
}
```

### Field Notes
- `id`: `^[a-z0-9][a-z0-9_-]{0,63}$` (auto-derived from `name` if omitted).
- `storage.port` / `preferred_port`: Optional fixed port in `[1024, 65535]`. When omitted, `AppModeServer` computes a deterministic port in `[42000, 58999]` from `SHA-256(app_id + "\n" + account_id)` and falls back to an ephemeral OS port if already in use.
- `mcp.access`:
  - `"full"` (default): Standard Ladybird MCP server capabilities.
  - `"webmcp-only"`: Requires `mcp.allowWebMCP: true`. Restricts MCP agents strictly to `list_tabs`, `get_active_tab`, `list_page_tools`, and `call_page_tool`.
- `permissions.dynamicServices`: When `true`, enables `window.ladybirdApp.tabs.registerService(...)` and `removeService(...)` at runtime and persists dynamic services per account profile.
- `permissions.network`: `"unrestricted"` (default) or `"offline"`. When `"offline"`, `AppModeServer` emits an airgap `Content-Security-Policy` confining script, fetch/XHR/WebSocket (`connect-src 'self'`), frames, workers, media, and forms strictly to the app's own local origin (`127.0.0.1:<port>`).
- `permissions.crossOriginIsolated`: When `true`, `AppModeServer` emits `Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp`, and `Cross-Origin-Resource-Policy: same-origin` (including on SSE event streams), enabling `self.crossOriginIsolated`, `SharedArrayBuffer`, and shared Wasm memory.
- `services.<id>`: Declares isolated site tabs backed by dedicated container profiles (`container`), strict origin allowlists (`origins`), and per-service capabilities (`open`, `navigate`, `notifications`, `read`, `webmcp`, `eval`, `flows`, `network`).

---

## Multi-Account Isolation & Storage

Passing `--account <slug>` isolates every storage layer for the same app:
- **Source-bound identity**: `<storage-id>` combines the display ID with a SHA-256 digest of the canonical local app root or remote origin, so another source cannot claim the same persistent authority by copying a manifest ID.
- **Browser profile directory**: `app-<storage-id>` (for `default`) or `app-<storage-id>--<account-id>` (`Profiles/`).
- **Web storage**: Independent cookies and `localStorage` bound to the deterministic origin (`http://127.0.0.1:<port>`).
- **SQLite KV (`AppStore.db`)**: Per-profile persistent JSON storage under `AppData/<storage-id>/AppStore.db`.
- **Jailed filesystem**: Relative paths confined to `fsRead` / `fsWrite` top-level subdirectories under `AppData/<storage-id>/` with traversal and symlink checks.
- **Account registry**: `<instance-data>/AppAccounts/<storage-id>.json`.

The storage ID is stable across restarts and updates at the same canonical source. Moving a local app to a different root intentionally creates a separate storage namespace.

---

## Bridge API (`window.ladybirdApp`)

Include `<script src="/__ladybird_bridge.js"></script>` in `<head>` before app scripts.

- `window.ladybirdApp.info`: Frozen boot metadata (`id`, `name`, `version`, `origin`, `port`, `stablePort`, `mcpAccess`, `account`, `permissions` including `network` and `crossOriginIsolated`).
- `window.ladybirdApp.system.info()`: Live IPC query returning runtime state including `mcpPort`, `mcpAccess`, `networkPolicy`, and `crossOriginIsolated`.
- `window.ladybirdApp.fs`: `listDir(path)`, `readText(path)`, `writeText(path, content)`, `deleteFile(path)`, `stat(path)`, `mkdir(path)`, `readBytes(path, { offset?, count? })`, `writeBytes(path, data)`, `fileUrl(path)` (binary reads/writes use authenticated `/__ladybird_fs/<path>` up to 128 MiB with HTTP `Range` support).
- `window.ladybirdApp.kv`: `get(key)`, `set(key, value)`, `delete(key)`, `list(prefix?)`, `clear(prefix?)`.
- `window.ladybirdApp.accounts`: `current()`, `list()`, `create(id, label?)`, `launch(id, options?)`.
- `window.ladybirdApp.tabs`: `list()`, `open(serviceId, options?)`, `focus(serviceId)`, `navigate(serviceId, url)`, `close(serviceId)`, `registerService(service)` (triggers a native user consent dialog on GUI windows whenever origins, container, or permissions are new/changed), `removeService(serviceId)`, `notifications(options?)`, `network(options?)`, `events(options?)`, `readMarkdown(serviceId, options?)` (requires `"read"`, extracts visible page Markdown + scrubs vault secrets without exposing `"eval"`), `listTools(serviceId, options?)` and `callTool(serviceId, toolName, args?)` (require `"webmcp"`, invoke the target tab's WebMCP tools with interactive approval on consequential tools and vault secret scrubbing), `eval(serviceId, script)` (supports top-level `await` / returned Promises up to 10s), `runFlow(serviceId, options?)`, `onNotification(...)`, `onNetwork(...)`, `onEvent(...)` (backed by real-time `EventSource` SSE push on `GET /__ladybird_events?token=...` with automatic polling fallback).
- `window.ladybirdApp.flows`: `list()`, `get(flowId)`, `save(flow)`, `delete(flowId)`, `getRun(runId)`, `listRuns(options?)`, `cancelRun(runId)`.
- `window.ladybirdApp.triggers`: `list()`, `save(trigger)` (supports `interval`, `notification`, `network`, `navigation`, `webhook`, and native 5-field `cron` with `cronExpr`), `delete(id)`, `setEnabled(id, enabled)`.
- `window.ladybirdApp.app.quit()`: Cleanly exits the app instance.

When `mcp.enabled` and `mcp.allowWebMCP` are true (and `--mcp-disable-webmcp` is not set), `/__ladybird_bridge.js` also initializes `document.modelContext` / `navigator.modelContext` synchronously and dispatches `ladybird:webmcp-ready`. Both `tabs.eval` and MCP `eval_js` resolve returned Promises / top-level `await` up to 10 seconds.

---

## Inbound Local Webhooks & Native Cron

Triggers saved with `type: "webhook"` expose a loopback HTTP endpoint at `POST /__ladybird_webhook/<triggerId>` and auto-generate a 32-character hexadecimal `webhookSecret` on save when omitted (maximum body size: 65,536 bytes):

- Authenticate requests via `X-Webhook-Token: <secret>` or `Authorization: Bearer <secret>`.
- Status codes: `202` accepted, `401` missing/invalid token, `404` unknown/disabled webhook trigger, `405` non-`POST` method, `429` `cooldownMs` throttle active.
- Accepted requests record a `"webhook"` event in the service event stream (`tabs.events` and SSE `event: event`) and dispatch natively unless `clientHandled: true` is set on the trigger.

Triggers saved with `type: "cron"` require a valid 5-field `cronExpr` (`minute hour day-of-month month day-of-week`, supporting `*`, lists, ranges, and `/step` syntax), are evaluated by `AppModeServer` once per matching local minute slot (`YYYY-MM-DD HH:MM`), emit a `"cron"` service event, and dispatch natively unless `clientHandled: true`.

---

## Privacy Mode: `mcp.access = "webmcp-only"`

Designed for sensitive local apps (e.g. banking/accounting, medical notes, HR datasets) where raw records are rendered for the human user in the UI, while external MCP agents should only invoke structured WebMCP tools (such as aggregate summaries or code-to-data computations):

- `tools/list` advertises **only**: `list_tabs`, `get_active_tab`, `list_page_tools`, `call_page_tool`.
- All DOM/screenshot/console/network/navigation/eval/flow tools (`get_page_snapshot`, `get_dom_tree`, `get_accessibility_tree`, `get_page_source`, `get_page_markdown`, `take_screenshot`, `eval_js`, `get_console_logs`, `get_network_requests`, `click`, `open_tab`, `navigate`, `run_flow`, etc.) return an error and are recorded in the MCP audit log.
- `resources/list` returns `[]`; `skills/*` and `resources/*` return JSON-RPC error `-32003`; `list_tabs` omits `matching_skills` and `matching_credentials`.
- Can also be enforced from the CLI for any app via `--mcp-webmcp-only` (or `ladybird-app.py run|connect ... --webmcp-only`).

---

## On-Demand Lifecycle & CLI (`Meta/ladybird-app.py`)

Live non-temporary instances publish `<instance-data>/AppRuntime/<storage-id>--<account-id>.json` with `pid`, `uiPid`, `mode` (`"headless" | "gui"`), `appUrl`, `mcpPort`, `mcpUrl`, `mcpToken`, `mcpAccess`, and `startedAtMs`, removing it on shutdown if owned by the exiting PID. The lifecycle CLI derives the same storage ID from the app target while continuing to display the manifest ID.

```bash
# Scaffold & validate
python Meta/ladybird-app.py init ./MyApp --name "My App"
python Meta/ladybird-app.py validate Examples/BashWorkbenchApp

# Launch interactively (GUI) or headless
python Meta/ladybird-app.py run Examples/BashWorkbenchApp --account personal
python Meta/ladybird-app.py run Examples/BashWorkbenchApp --account personal --headless --webmcp-only

# On-demand discovery & reuse (returns JSON with mcpUrl + mcpToken)
python Meta/ladybird-app.py connect Examples/BashWorkbenchApp --account personal --headless --webmcp-only --json
python Meta/ladybird-app.py status Examples/BashWorkbenchApp --account personal --json
python Meta/ladybird-app.py connect Examples/BashWorkbenchApp --account personal --gui
python Meta/ladybird-app.py stop Examples/BashWorkbenchApp --account personal
```

---

## Examples & Verification

- **Reference apps**:
  - `Examples/AgenticNotesApp`: Markdown workspace + SQLite KV + multi-account WebMCP tools.
  - `Examples/BashWorkbenchApp`: Sandboxed UNIX shell (`just-bash`) + bidirectional `/workspace` disk sync + KV.
  - `Examples/AudioStudioApp`: Zero-dependency BSD-2-Clause WebAudio workstation with deterministic DSP, HTTP `Range` WAV probing, SQLite KV markers, and 14 `--webmcp-only`-compatible tools.
  - `Examples/SalesCRMApp`: Full-specification BSD-2-Clause Sales CRM & Pipeline workstation with sharded `AppStore.db` persistence, binary `/__ladybird_fs` attachments/backups, recurring MRR/ARR forecast, and 3-tier `--webmcp-only` PII isolation.
  - `Examples/FlowStudioApp`: Local-first multi-service browser workflow studio (`tests/test_flowstudio.mjs`) with dynamic services, container isolation, `IF/ELSE` & `For Each` pipelines, Worker sandbox, encrypted secrets, visual recorder, auto-healing selectors, inbound webhooks, portable `.flowpack.json` bundles, and 9 WebMCP tools.
- **C++ unit tests**:
  - `TestLibWebView "*app_*"` (`Tests/LibWebView/TestAppMode.cpp`)
  - `TestLibWebView "*mcp_*"` (`Tests/LibWebView/TestMCPAutomation.cpp`)
- **JS unit & E2E suites**:
  - `node Examples/FlowStudioApp/tests/test_flowstudio.mjs`
  - `python Meta/test_app_mode_e2e.py`
