# Ladybird Fork Architecture and Capabilities (`feat/extensions-dnr-and-dom-tools` / `main`)

> **Scope and Baseline:** This manual documents the architecture, subsystems, security model, and verified behavior on branch `feat/extensions-dnr-and-dom-tools` (fork baseline through Agent Skills `SEP-2640`, Blind Credential Vault, MCP policy hardening, Tab Containers, Web Compat fixes, and persistent UA presets).

## 1. Executive Summary

This fork extends upstream multi-process Ladybird with seven capability layers while keeping real-time automation, skills, credential security, extensions, session isolation, and media tooling unified inside one profile:

1. **Embedded Model Context Protocol (`MCP`, `58` Tools + `9` Protocol Methods + SSE Push), Agent Security Shield v1/v2, and WebMCP Phase 1**: An in-browser HTTP + JSON-RPC 2.0 + SSE (`GET /mcp`) `MCPServer` (`LibMCP`) exposing 58 tools across tab/container control (`open_tab` defaulting to an ephemeral in-memory `agent-ephemeral-*` container that auto-destroys on close/migration/`DELETE /mcp`, plus `list_containers`, `create_container`, and live `migrate_tab_container`), Shadow DOM/iframe Document Object Model (`DOM`) inspection (with hardened visibility/opacity/clip/off-screen filtering, visible-only `TreeWalker` `text_excerpt`, modal-prioritized `get_page_snapshot`, and `root_selector`), `MCPContentShield` anti-injection sanitization (strips invisible Unicode Tags/BiDi/zero-width/soft-hyphen/Braille-blank/variation chars & `<!-- ... -->`/`--!>` HTML comments and attaches per-response `security_envelope.boundary_nonce` and `injection_risk_signals`), sandboxed remote site manifest inspection (`inspect_site_manifests` for `/llms.txt`, `/agents.txt`, `/skills.txt`, `/prompts.md` with `credentials: omit`, `redirect: manual`, and strict `untrusted_remote_site` boundary), input synthesis (`wheel`, coordinate `click`/`hover`, `drag`/`press_key` modifiers, `type_text` keyboard mode), Upload Directory listing (`list_upload_files`), browser notification buffer/wait/SSE resource subscriptions (`list_notifications`, `wait_for_notification`, `ladybird://notifications*`), diagnostics, reusable flows, unpacked extensions, Agent Skills (`4`, including built-in `ladybird-agentic-guide` seeded on first profile init), Blind Credential Vault (`3`), per-tab 4-mode access (`Blocked`/`ReadOnly`/`SameOrigin`/`Full` locking onto committed origin and auto-blocking privileged `about:`/`resource:` URLs), **Cross-Tab / Egress Taint Guard**, Pause/Resume Kill Switch (canceling active MCP flows/approvals/queues/waits; blocking `tools/call`, `skills/*`, and `resources/read` with `-32001`; and suppressing SSE broadcasts while preserving active SSE streams/subscriptions and egress taint state), TOCTOU-guarded native Qt approvals (including `install_unpacked_extension` and `FlowRunner` `upload_file`), expanded `about:welcome` onboarding + `mcpAutoStart`, live **`about:agent`** Shield console, and `document.modelContext` / `navigator.modelContext` WebMCP page tools, plus a robot agent tab badge (`ChromeIcon::Agent`) and purple `"Ephemeral"` badge.
2. **Per-Page Agent Skills (`Skills` and `about:skills`, SEP-2640)**: Persistent disk-backed `SKILL.md` skills (`SkillStore`, `SkillTypes`, `SkillsUI`) with YAML frontmatter, URL match hints on `TabInfo`, SHA-256 digests, symlink/traversal guards, starter templates (`youtube`, `youtube-studio`, `google-maps`, `google-search`, `linkedin`), and SEP-2640 `skills/list`, `skills/get`, `resources/read`, and `resources/directory/read`.
3. **Blind Credential Vault and MCP Security Policies (`Vault` and `about:vault`)**: Encrypted credential storage (`VaultStore`, `VaultTypes`, `VaultUI` using AES-256-GCM + Windows DPAPI / PBKDF2 / `master.key`), RFC 6238 TOTP generation, origin & container pinning, `<input type="password">` + form-action guards, active-document-change taint quarantine, secret scrubbing, and CLI policy flags (`--mcp-allowed-domains`, `--mcp-blocked-domains`, `--mcp-allowed-containers`, `--mcp-disable-eval-js`, `--mcp-allow-skill-write`, `--mcp-max-tabs`, `--mcp-vault-passphrase`).
4. **Declarative Browser Automation (`Automation` and `about:flows`)**: A persistent JSON flow store (`FlowStore` under `<data_dir>/flows/`), state-machine executor (`FlowRunner`), browser-based `about:flows` builder (`FlowsUI`), and an external Node.js Engine Protocol v1 adapter ([`Tools/e2e-ladybird-engine/README.md`](../Tools/e2e-ladybird-engine/README.md)).
5. **Native Manifest V3 (`MV3`) Extensions and `declarativeNetRequest` (`DNR`)**: A native unpacked extension registry (`ExtensionStore`), content-script and adblock orchestrator (`ExtensionManager`) feeding `Web::ContentBlocker`, the `about:extensions` management page (`ExtensionsUI`), three bundled extensions, and Windows CryptoAPI / Authority Information Access (`AIA`) TLS chain fallback in `RequestServer`.
6. **Native Tab Containers (`ContainerStore`, `BrowsingSession`, `UI/Qt`)**: Persistent multi-account tab containers (`personal`, `work`, `banking`, `shopping`, `agent`, plus custom containers) with isolated cookies, DOM storage, SQLite databases, `FileDownloader` transfers, HTTP caches, `WorkerAgentManager` session routing, `SessionStore` v13 persistence, and Qt tab visual badges/menus (`open_tab` defaults to `"agent"`).
7. **Real-Time WebRTC Media, Segmented `MP4` Tab Recording & Web Compat**: Windows Media Foundation (`WMF`) camera capture, H.264 / VP8 Media Foundation Transform (`MFT`) encoding, Windows Audio Session API (`WASAPI`) capture and recovery, Rust `webrtc` media/ICE transport, sequential `MP4` tab recording with speaker diarization (`TabRecorder` and `SpeakerTimeline`), persisted `about:settings` UA presets (`Chrome Windows Desktop`), `Location.ancestorOrigins`, `StorageManager.persist/persisted`, `CredentialsContainer`/`PublicKeyCredential` availability, `Navigator` protocol handler validation, and speculative parser CSP nonce/integrity propagation.

*Architectural rationale:* These layers share a single host controller (`WebView::Application`) so extensions can bundle reusable automation flows and filter lists, MCP agents can invoke flows and WebMCP tools inside the user's authenticated session, and W3C WebDriver detection (`navigator.webdriver`) remains `false` outside external WebDriver sessions.

---

## 2. Terminology, Process Topology, and Platform Support

### 2.1 Glossary of Ladybird-Internal Terms

- **`Hub` (`Services/Hub` and `WebView::Application`)**: The privileged browser backend layer that owns browsing sessions, tab view state, helper process lifetimes, content blockers, and profiles.
- **`WebContent` (`WebContentClient` / `WebContentPage`)**: The per-session / per-site renderer process executing the DOM, `LibJS`, layout/painting, `Web::ContentBlocker` (Rust `adblock`), and `WebDriverConnection`. `WebContentClient` and `WebContentPage` are the Hub-side IPC client and view bindings.
- **`WebUI`**: Ladybird's internal bridge connecting privileged `about:*` HTML/JS pages (such as `about:flows` and `about:extensions`) to C++ handlers (`FlowsUI`, `ExtensionsUI`) via `ladybird.sendMessage`.
- **`BackingStores` and `AnonymousBuffer`**: Shared-memory pixel/data buffers (`AnonymousBuffer`) passed across IPC between processes to present rendered frames cleanly or push compiled content-blocker lists.
- **`WebDriverConnection`**: The in-renderer script and synthetic-input executor shared by external W3C WebDriver clients and internal `MCP` / `Automation` / `Extensions` commands.
- **`VERIFY(...)` and `MUST(...)`**: Core assertion macros (`AK`) that remain active in release builds and abort the process immediately on failure.
- **`Lagom`**: Build prefix used for Ladybird's cross-platform host libraries (`lagom-webview.dll`, `lagom-mcp.dll`, etc.).

### 2.2 Multi-Process Topology

Depending on the host OS, Ladybird runs `Services/Hub` either as a separate helper executable (`LADYBIRD_HUB_IN_OWN_PROCESS` enabled on Linux and macOS in `UI/CMakeLists.txt`) or linked directly into the Qt UI executable (`UI/Qt/Application.cpp` implementing `Hub::ApplicationHost` and instantiating `Hub::Application` / `WebView::Application` over an in-memory `IPC::TransportHandle::create_pair()` on Windows and BSDs). Both modes use the same `HubServer.ipc` / `HubClient.ipc` Inter-Process Communication (`IPC`) protocol.

```mermaid
flowchart TB
  subgraph BrowserHost["UI and Hub Layer (Unsandboxed on Windows)"]
    UI["Qt UI Process (UI/Qt/Application, BrowserWindow, Tab, MCPBanner, TabRecorder)"]
    HUB["Hub / WebView::Application (Services/Hub + LibWebView/Application)"]
    MCP["Embedded MCPServer (127.0.0.1:7331 /mcp JSON-RPC 2.0)"]
    FLOWS["Automation (FlowStore + FlowRunner)"]
    EXT["Extensions (ExtensionStore + ExtensionManager)"]
    UI <-->|"HubServer.ipc / HubClient.ipc"| HUB
    HUB --- MCP
    HUB --- FLOWS
    HUB --- EXT
  end

  subgraph CoreServices["Network, Graphics, and Audio Helpers"]
    REQ["RequestServer (libcurl + OpenSSL + Windows CryptoAPI/AIA fallback)"]
    COMP["Compositor (Skia + ANGLE on Windows + Rust compositor_core)"]
    IMG["ImageDecoder"]
    AUD["AudioServer (WASAPI capture & playback recovery on Windows)"]
  end

  subgraph ContentAndRTC["Renderer and Real-Time Media Processes"]
    WC["WebContent (DOM, LibJS, WebDriverConnection, ContentBlocker)"]
    WRTC["WebRTCClient (Rust webrtc crate + Tokio, sandboxed on Linux/macOS)"]
  end

  HUB -->|"Manages processes, content blockers, and tab state"| REQ
  HUB -->|"Spawns per browsing session / site"| WC
  HUB <-->|"execute_script / execute_async_script / perform_actions"| WC
  WC -->|"BackingStores / display lists"| COMP
  WC -->|"Resource loads"| REQ
  WC -->|"Image decode handles"| IMG
  WC <-->|"WebRTCClientServer.ipc / WebRTCClientClient.ipc"| WRTC
  WC <-->|"Audio streams"| AUD
  UI -->|"Tab snapshot + WASAPI loopback & mic -> WMF MP4 (Windows)"| UI
```

### 2.3 Key Hosting Facts and Module References

- **In-process Hub (Windows and BSDs)**: `UI/Qt/Application.cpp` hosts `Hub::Application` in-process and links `hubservice` when `!LINUX && !APPLE`. On Windows (`if(WIN32)`), the Qt target links `LibDevTools` and `LibMCP` directly; `Libraries/LibWebView/CMakeLists.txt` also links `LibDevTools` and `LibMCP` as `PRIVATE` dependencies of `LibWebView` (covering BSD builds transitively). On Windows, running `Ladybird.exe` locks `lagom-webview.dll` and `lagom-mcp.dll`, so browser processes must be terminated before relinking. In headless mode, the in-process Hub bypasses Qt UI bindings.
- **Out-of-process Hub (Linux and macOS)**: `UI/Qt/Application.cpp` spawns the `Hub` executable over a Mach port pair (macOS) or `AF_LOCAL` `socketpair` (Linux). `Services/Hub/main.cpp` spawns a `UIExitWatcher` thread (`kqueue` on macOS/BSD, `pidfd_open` on Linux) that currently has no Windows implementation (`FIXME`).

| Subsystem | Windows | Linux | macOS |
| :--- | :--- | :--- | :--- |
| Hub hosting | In-process (`UI/Qt`) | Out-of-process (`Hub`) | Out-of-process (`Hub`) |
| Process sandbox (`WebContent` / `WebRTCClient`) | Unsandboxed (`SandboxUnimplemented`) | Seccomp + Landlock | Seatbelt |
| TLS fallback (`RequestServer`) | OpenSSL + `CURLSSLOPT_NATIVE_CA` + CryptoAPI/AIA | OpenSSL + AIA | OpenSSL + AIA |
| Audio backend (`AudioServer`) | WASAPI (capture + device-loss reopen) | PulseAudio / PipeWire / platform | CoreAudio |
| WebRTC Video TX / RX | WMF H.264/VP8 TX + WMF/FFmpeg RX | No WMF TX; FFmpeg RX | No WMF TX; FFmpeg RX |
| `TabRecorder` (`MP4`) | Supported (`Q_OS_WIN` + WMF) | Unsupported (returns error popup) | Unsupported (returns error popup) |

Module documentation:

- [LibMCP Module README](../Libraries/LibMCP/README.md)
- [Automation Module README](../Libraries/LibWebView/Automation/README.md)
- [Extensions Module README](../Libraries/LibWebView/Extensions/README.md)
- [e2e Engine Protocol Adapter README](../Tools/e2e-ladybird-engine/README.md)

---

## 3. Embedded MCP Server and WebMCP Phase 1

The embedded `LibMCP` server (`Libraries/LibMCP/MCPServer.cpp`) binds exclusively to `127.0.0.1` (default port `7331` when launched with `--mcp`) and delegates actions through `MCP::MCPDelegate`, implemented by `WebView::Application` (`Libraries/LibWebView/Application.cpp`).

### 3.1 Tool Surface (`58` Tools + `9` Protocol Methods + SSE Push)

1. **Tabs, Containers, and Navigation (12)**: `list_tabs`, `get_active_tab`, `list_containers`, `create_container`, `open_tab`, `migrate_tab_container`, `switch_tab`, `close_tab`, `navigate`, `reload`, `go_back`, `go_forward`.
2. **DOM, Input, Notifications, and Diagnostics (22)**: `get_accessibility_tree`, `get_dom_tree`, `get_page_source`, `find_elements`, `click` (supports `@ref`/`selector`/`element_id`, viewport `(x, y)` coordinates, `button`, `click_count`, and `modifiers`), `type_text` (supports `use_keyboard` trusted keyboard dispatch), `get_page_snapshot` (assigns compact `@e1` handles across open `shadowRoot` trees and same-origin frames until DOM replacement or navigation, prioritizes visible open modals/dialogs, and supports optional `root_selector`), `press_key` (supports `modifiers`), `scroll`, `wheel`, `hover` (supports viewport `(x, y)` coordinates), `drag` (coordinate/delta support, interpolated steps/duration, and `modifiers`), `upload_file`, `list_upload_files`, `select_option`, `wait_for`, `list_notifications`, `wait_for_notification`, `get_console_logs`, `get_network_requests`, `eval_js`, `take_screenshot`.
3. **Automation Flows (8)**: `list_flows`, `get_flow`, `save_flow`, `delete_flow`, `run_flow`, `list_flow_runs`, `get_flow_run`, `cancel_flow_run`.
4. **Extensions (6)**: `list_extensions`, `install_unpacked_extension`, `set_extension_enabled`, `reload_extension`, `remove_extension`, `inject_extension`.
5. **WebMCP & Site Manifests (3)**: `list_page_tools`, `call_page_tool`, `inspect_site_manifests`.
6. **Agent Skills & Resources (4 + Protocol/SSE)**: `list_skills`, `read_skill`, `save_skill`, `delete_skill`, plus JSON-RPC methods `ping`, `server/discover`, `skills/list`, `skills/get`, `resources/list`, `resources/subscribe`, `resources/unsubscribe`, `resources/read` (skills + `ladybird://notifications*`), `resources/directory/read`, and `GET /mcp` SSE notifications `notifications/resources/updated` & `notifications/message` (`protocolVersion` `"2025-03-26"` or `"2026-07-28"`).
7. **Blind Credential Vault (3)**: `list_credentials`, `fill_credential`, `login_with_credential` (`TabInfo` exposes `container_id`, `mcp_access_mode`, `matching_skills`, and `matching_credentials`).

### 3.2 Per-Tab Access Modes, Kill Switch, Content Shield, and Native Approvals

- **Per-tab 4-mode access (`WebView::MCPAccessMode`)**: User-opened tabs and new tabs from local-authority `about:flows` runs start in `Blocked` (`0`). `MCPBanner` (`QComboBox`) and the tab context menu (`"MCP Access"`) switch between `Blocked` (`0`), `ReadOnly` (`1`, permits read/snapshot/notification inspection while refusing DOM/input/navigation mutations), `SameOrigin` (`2`, pins actions and navigations to the origin active when enabled), and `Full` (`3`). Tabs opened via `open_tab` (defaulting to an ephemeral in-memory `agent-ephemeral-*` container when no `container_id` is given) or MCP `run_flow` (`open_new_tab: true` with `require_mcp_permission = true`) auto-enable `Full` mode and display the robot indicator badge (`ChromeIcon::Agent`) in `UI/Qt/TabBar`; the Hub applies the mode before publishing the new view to Qt.
- **Pause/Resume Kill Switch (`MCPBanner` -> `MCPServer::set_paused`)**: Immediately cancels active MCP `FlowRunner` runs, pending approvals, `MCPViewQueue` commands, and waits; blocks `tools/call` (`isError: true`), `skills/*`, and `resources/read` (`-32001`); and suppresses SSE broadcasts while keeping the loopback server, active SSE streams/subscriptions, `resources/list`/`subscribe`/`unsubscribe`, and egress taint state alive for inspection or resumption.
- **Anti-injection sanitization (`Libraries/LibWebView/MCPContentShield.{h,cpp}`)**: Readout tools (`get_page_snapshot`, `get_page_source`, `get_dom_tree`, `get_accessibility_tree`, `get_console_logs`, `list_page_tools`, `list_notifications`, `wait_for_notification`, and `ladybird://notifications*`) strip invisible Unicode Tags (`U+E0000..U+E007F`), BiDi overrides, and zero-width characters; `get_page_source` also strips `<!-- ... -->` comments and wraps HTML in `framed_source`, attaching `security_envelope` (`untrusted_content: true`, `boundary_nonce`, `stripped_invisible_chars`, `stripped_html_comments`, `injection_risk_signals`). `get_page_snapshot` additionally filters near-zero-opacity (`<= 0.01`), tiny-font (`<= 0.1px`), `color === backgroundColor`, and far off-screen CSS decoy elements.
- **Native Qt human-in-the-loop approvals (`UI/Qt/Tab` -> `Hub` -> `Application::request_mcp_user_approval`)**: `upload_file`, `fill_credential` (`password`/`totp`), `login_with_credential`, `install_unpacked_extension`, egress guard confirmations, and consequential/destructive `call_page_tool` prompt via `QMessageBox` (`60 s` fail-closed timeout; fails closed immediately when no interactive UI handler is attached, unless `--mcp-auto-approve` is explicitly enabled in `--headless` mode and logged to `about:agent`).
- **Private & privileged tab exclusion**: `ViewImplementation::set_mcp_access_mode`, `find_allowed_mcp_view`, `find_automation_view`, `list_allowed_tabs`, `active_allowed_tab`, `MCPBanner`, `Tab` context menus, `FlowsUI`, `SkillsUI`, and `VaultUI` reject private views (`IsPrivate::Yes`) and privileged internal pages (`about:*` other than `about:blank`, and `resource:*`); committing a navigation to a privileged `about:` URL revokes MCP access automatically.
- Each view uses a serial `MCPViewQueue` in `Application`: commands dispatch into `WebDriverConnection` under a `35 s` single-shot `in_flight_timer` (exceeding WebContent's `30 s` script timeout). On timeout, revocation, or view close, `Application` calls `ViewImplementation::cancel_webdriver_content_command` so `m_webdriver_input_command_running` resets cleanly and queued commands do not wedge `WebDriverConnection`.
- **`navigator.webdriver` preservation**: `Services/WebContent/WebDriverConnection.cpp` enables `page.set_is_webdriver_active(true)` and disables pop-up blocking only inside `set_session_config` (invoked by external W3C WebDriver clients). Internal MCP, Flow, and Extension scripts leave `navigator.webdriver === false`.

### 3.3 Main World vs. Isolated World and WebMCP Phase 1

Unlike browser isolated worlds (separate JS global and prototype environments where Chrome/Firefox extensions run), both injected extension scripts and the WebMCP Phase 1 shim execute via `WebDriverConnection` inside the page's **Main World** (the same `window`, `document`, and prototypes used by webpage scripts) and arrive asynchronously over IPC.

- **Injection condition**: `inject_webmcp_shim_if_needed` injects `mcp_webmcp_shim_js` on `http:` and `https:` MCP-allowed, non-private tabs. The JS shim itself exposes `document.modelContext` and `navigator.modelContext` (`registerTool`, `unregisterTool`, `provideContext`, `clearContext`) only in Secure Contexts (`window.isSecureContext`, `https:`, `localhost`, `127.0.0.1`, or `[::1]`).
- **Declarative forms & safety annotations**: Scans `document` and open `shadowRoot` trees for `form[toolname]` (synthesizing JSON Schemas from `form.elements` and dispatching `toolactivated`), forces `untrustedContentHint: true` on all discovered page tools, and gates `consequentialHint` / `destructiveHint` execution server-side via `request_mcp_user_approval` (ignoring caller `confirm_consequential` self-approvals and binding each inspection to an active document and a single-use renderer ticket). The browser and WebContent recheck the document ID before execution. The shim consumes the ticket and verifies the exact registration or form and its execution metadata.

```mermaid
sequenceDiagram
  participant Agent as External MCP Agent
  participant Server as MCPServer (127.0.0.1:7331)
  participant App as WebView::Application
  participant UI as Qt Approval Dialog (or Headless Gate)
  participant Page as WebContent Document (Main World)

  App->>Page: inject_webmcp_shim_if_needed (HTTP/HTTPS, MCP-allowed, non-private)
  Note over Page: Exposes document.modelContext and navigator.modelContext on Secure Contexts
  Agent->>Server: POST /mcp list_page_tools(tab_id, include_declarative)
  Server->>App: MCPDelegate::list_page_tools
  App->>Page: execute_script to window.__ladybirdWebMCP.listTools(...)
  Page-->>Agent: Imperative tools + declarative form[toolname] schemas (untrustedContentHint: true)
  Agent->>Server: POST /mcp call_page_tool(tab_id, name, arguments)
  Server->>App: MCPDelegate::call_page_tool
  App->>Page: Inspect tool in the current document and bind registration to a single-use ticket
  alt consequentialHint, destructiveHint, or Egress Guard active
    App->>UI: request_mcp_user_approval (60 s fail-closed; --mcp-auto-approve in --headless only)
    UI-->>App: Approved / Rejected
    App->>App: Re-check pause, permission, quarantine, document ID & URL
  end
  App->>Page: Dispatch with the document ID; verify the ticket and registration before invocation
  Page-->>Agent: Async Promise result or safety rejection wrapped with security_envelope
```

### 3.4 CLI Flags and Security Controls

| Flag | Default | Effect | Source |
| :--- | :--- | :--- | :--- |
| `--mcp [port]` | Disabled (`7331` if port omitted) | Starts embedded `MCPServer` on `127.0.0.1` | `Libraries/LibWebView/Application.cpp`, `Options.h` |
| `--mcp-token <token>` | 32 random bytes (64 hex chars, printed to stdout) | Overrides the generated Bearer token | `Libraries/LibWebView/Application.cpp` |
| `--mcp-upload-dir <directory>` | Browser Upload Directory (`<Videos>/Ladybird Uploads` by default) | Overrides the browser setting and confines `upload_file` to `<directory>` with symlink checks | `Libraries/LibWebView/Options.h`, `Application.cpp` |
| `--mcp-allow-extension-install` | `false` | Enables `install_unpacked_extension` over MCP (`about:extensions` remains enabled in UI) | `Libraries/LibWebView/Options.h`, `Application.cpp` |
| `--mcp-allow-skill-write` | `false` | Enables `save_skill` / `delete_skill` over MCP (`about:skills` remains enabled in UI) | `Libraries/LibWebView/Options.h`, `Application.cpp` |
| `--mcp-vault-passphrase <pass>` | Unset (falls back to `LADYBIRD_VAULT_PASSPHRASE` / OS key) | Derives AES-256-GCM vault key with PBKDF2-HMAC-SHA256 | `Libraries/LibWebView/Vault/VaultStore.cpp` |
| `--mcp-allowed-containers <csv>` | Unfiltered (`open_tab` defaults to `agent`) | Restricts MCP view lookup, `open_tab`, and MCP `open_automation_tab` to listed container IDs | `Libraries/LibWebView/Options.h`, `Application.cpp` |
| `--mcp-allowed-domains <csv>` | Unfiltered | Domain allowlist (`*.domain.com`) enforced on `open_tab`, `navigate`, `FlowRunner`, and view lookups | `Libraries/LibWebView/Vault/VaultTypes.cpp`, `Application.cpp` |
| `--mcp-blocked-domains <csv>` | Unset | Domain denylist (`*.domain.com`, trailing-dot tolerant) taking precedence over allowlist | `Libraries/LibWebView/Vault/VaultTypes.cpp`, `Application.cpp` |
| `--mcp-allow-eval-js` | `false` (`eval_js` disabled by default) | Enables `eval_js`, `call_page_tool`, and MCP `run_flow` `eval` steps | `Libraries/LibWebView/Options.h`, `Application.cpp`, `FlowRunner.cpp` |
| `--mcp-disable-eval-js` | `false` (effective policy starts disabled via `!mcp_allow_eval_js \|\| mcp_disable_eval_js`) | Explicitly forces `eval_js`, `call_page_tool`, and MCP `run_flow` `eval` steps disabled (overrides `--mcp-allow-eval-js`) | `Libraries/LibWebView/Options.h`, `Application.cpp`, `FlowRunner.cpp` |
| `--mcp-disable-screenshots` | `false` | Blocks `take_screenshot` and MCP `FlowRunner` `screenshot` steps (DOM-only visual defense) | `Libraries/LibWebView/Options.h`, `Application.cpp`, `FlowRunner.cpp` |
| `--mcp-redact-screenshot-media` | `false` | Enforces `redact_media: true` on `take_screenshot` (hides `<img>`, `<picture>`, `<video>`, `<canvas>`, `<svg>`, CSS background images) | `Libraries/LibWebView/Options.h`, `Application.cpp` |
| `--mcp-auto-approve` | `false` | `--headless` mode only (ignored with warning in GUI): auto-approves uploads, blind vault credentials, consequential `call_page_tool`, egress decisions, and extension installs (recorded in `about:agent` audit log) | `Libraries/LibWebView/Options.h`, `Application.cpp` |
| `--mcp-max-tabs <count>` | `16` (`>= 1`) | Caps concurrent MCP-accessible tabs for `open_tab` and MCP `open_automation_tab` | `Libraries/LibWebView/Options.h`, `Application.cpp` |
| `--user-agent-preset <name>` | Persisted `Settings.json` preset | Overrides User-Agent preset (including `Chrome Windows Desktop`, configurable in `about:settings`) | `Libraries/LibWebView/UserAgent.cpp`, `Settings.cpp` |

---

## 4. Declarative Automation (`Automation` and `Tools/e2e-ladybird-engine`)

- **Engine (`Libraries/LibWebView/Automation/`)**: `FlowTypes` defines 16 step actions (including `upload_file`), 7 locator strategies (`css`, `xpath`, `role`, `label`, `placeholder`, `text`, `test_id` — 6 DOM/ARIA locators traverse open `shadowRoot` trees while `xpath` evaluates against `document`), flow provenance `source` (`"user"`, `"mcp"`, `"extension:<id>"`, `"unknown"`), `nth` bounded to `0..1000`, and single-pass `{{var}}` substitution. `FlowStore` persists flows under `<data_dir>/flows/` (or instance storage directory), returns an error from `delete_flow` when missing, caps run history at `max_in_memory_runs = 50`, and prunes evicted run JSONs and `_step_*.png` screenshots from disk. `FlowRunner` executes steps asynchronously against the browser-configured Upload Directory (default `<Videos>/Ladybird Uploads`, configurable in `about:flows` / `about:settings` or `--mcp-upload-dir`); `reload` checks for `about:` error pages, `extract`, `eval`, `screenshot`, and `upload_file` (`save_as`) populate `record.outputs` and `runtime_vars` (marking `extract` and `eval` keys as tainted so `FlowRunner` blocks them in `upload_file` paths and cross-origin `open` navigations), and when `require_mcp_permission` is set, `FlowRunner` enforces MCP domain policy, container policy, `max_tabs`, `--mcp-disable-eval-js`, and Vault secret taint quarantine, while MCP flow results are scrubbed via `scrub_mcp_tool_result`.
- **Built-in UI (`about:flows`)**: `FlowsUI` (`Libraries/LibWebView/WebUI/FlowsUI.cpp`) connects `Base/res/ladybird/about-pages/flows.html` directly to `FlowStore`, `FlowRunner`, and the Upload Directory (`Settings`), displaying provenance (`User`, `MCP`, `Extension`, or `Unknown`) and keeping it on edits. The UI confirms external flows and the runner enforces MCP policy for `"mcp"` and `"unknown"`, including edits of saved definitions. Local-authority runs open tabs in `Blocked` mode.
- **External harness adapter ([`Tools/e2e-ladybird-engine/README.md`](../Tools/e2e-ladybird-engine/README.md))**: Translates `e2e` Engine Protocol v1 (`Content-Length`-framed JSON-RPC over `stdio`) to HTTP `POST /mcp` calls using `LADYBIRD_MCP_URL` and `LADYBIRD_MCP_TOKEN` (requiring `--mcp-allow-eval-js` on Ladybird for `data-e2e-ref` stamping and DOM helpers), delegating `setInputFiles` to `upload_file`, and returning `STALE_REF`, `NOT_FOUND`, or `NOT_ACTIONABLE` diagnostics.

---

## 5. Native MV3 Extensions, DNR, and `RequestServer` TLS

- **Manifest & Storage (`Libraries/LibWebView/Extensions/`)**: `ExtensionStore` manages unpacked extensions (loaded directly from local folders) in `<data_dir>/extensions/registry.json` (`schema_version: 1`) and `<data_dir>/extensions/storage/<id>.json`. Parses MV3 `content_scripts`, `action`, `declarative_net_request.rule_resources`, and the custom `ladybird` manifest block (`id`, `flows`, `filter_lists`).
- **Path Safety**: `resolve_safe_extension_file_path` (`ExtensionTypes.cpp`) rejects disallowed relative paths (absolute paths, `\`, `:`, `.`, `..`, NUL bytes), checks every directory segment with `FileSystem::is_link`, canonicalizes with `FileSystem::real_path`, enforces containment under the extension root, and requires `FileSystem::is_regular_file`.
- **Execution & Content Blocking (`ExtensionManager`)**:
  - `CanonicalTraversable::commit_navigation` (`document_start` + WebMCP shim) and `WebContentPage::did_finish_loading` (`document_end` / `document_idle`) trigger `ExtensionManager::did_start_load` / `did_finish_load`, executing scripts in the page's Main World via `execute_script` with an IIFE receiving `(browser, ladybirdExt)` backed by a read-only `storage.local.get` snapshot.
  - Translates DNR `"block"` and `"allow"` (`@@` exception prefix) rules (`urlFilter`, `requestDomains`, `initiatorDomains`, `excludedInitiatorDomains`, mapped `resourceTypes`) plus raw `ladybird.filter_lists` adblock syntax (`$options`, `##` cosmetic hiding selectors) into `Application::load_content_blocker_lists()` -> `Web::ContentBlocker`.
  - Bundled extensions in `Tools/`: `example-extension` (`ladybird-page-inspector`), `extensions/table-enhancer` (`ladybird-table-enhancer`), and `extensions/youtube-clean-and-adblock` (`ladybird-youtube-clean-adblock`).
- **Windows TLS Chain Validation (`Services/RequestServer/`)**: `CURL.cpp` sets `CURLOPT_SSL_OPTIONS` to `CURLSSLOPT_NATIVE_CA` when no custom certificate path is configured. When OpenSSL encounters missing local issuer errors (`X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY`, `X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT`, or `X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN`), `AIA.cpp` falls back to `verify_chain_with_windows_cryptoapi` (`CertGetCertificateChain` with cached revocation + `CERT_CHAIN_POLICY_SSL`) alongside HTTP Authority Information Access (`AIA`) intermediate CA fetching.

---

## 5.5 Native Tab Containers (`ContainerStore`, Engine Isolation, Hub IPC, and Qt UI)

- **Registry (`Libraries/LibWebView/ContainerStore.{h,cpp}`)**: Persists `TabContainer { id, name, color }` entries in `<profile.paths().config>/Containers.json`, seeding five defaults (`personal`, `work`, `banking`, `shopping`, `agent`) on first load and exposing validation (`is_valid_container_id`, `is_valid_container_color`), creation, deletion, and `ContainerStoreObserver`.
- **Engine Isolation (`BrowsingSession`, `CookieJar`, `WorkerAgentManager`, `RequestServerManager`, `WebView::Application`)**:
  - Each active `container_id` resolves lazily to a distinct non-private `BrowsingSession` backed by `<profile.paths().data>/Containers/<id>/Ladybird.db` (cookies, localStorage/sessionStorage/IDB) and `<profile.paths().data>/Containers/<id>/Permissions.json`.
  - `CookieJar::TransientStorage` filters live cookie change broadcasts by `m_owner_jar` pointer identity so cookie updates never cross containers.
  - `WorkerAgentManager::post_broadcast_channel_message` and `WebWorkerClient` isolate `BroadcastChannel` and `SharedWorker` coordination by `session_id` rather than `IsPrivate`.
  - `RequestServerManager` isolates site HMAC keys by `container_id` and scopes RequestServer HTTP caches under `RequestServer/Containers/<id>`.
- **Hub IPC, Qt UI, Persistence & MCP**:
  - `Services/Hub` (`HubClient.ipc` / `HubServer.ipc`) synchronizes `TabContainerInfo`, `containers_changed`, `create_container`/`delete_container`, and per-view `container_id` across in-process and out-of-process Hub builds.
  - `UI/Qt` provides right-click on the `+` new-tab button, Ladybird menu -> *New Container Tab*, tab context menu -> *Reopen in Container*, duplicate-tab preservation, colored tab accent stripes/tooltips (`TabBar`), and `SessionStore` schema v13 (`SessionTabs.container_id`) persistence across restart/reopen.
  - `LibMCP` exposes `container_id` and `mcp_access_mode` on `TabInfo`, accepts optional `container_id` in `open_tab`, and defaults omitted `container_id` to `"agent"`.

---

## 6. Real-Time Media, WebRTC, and Segmented `MP4` Tab Recording

- **MediaCapture & AudioServer WASAPI**:
  - `WindowsMediaFoundationCamera.cpp` shares hardware cameras (`CameraHub` / `SharedCameraSession`) via async `IMFSourceReader`, converting `RGB32`, `ARGB32`, `NV12`, and `YUY2` samples into `BGRx8888`.
  - `Services/AudioServer/AudioDevicesWasapi.cpp`, `RecordStreamWasapi.cpp`, and `PlaybackStreamWasapi.cpp` implement WASAPI endpoint enumeration, hot-plug notifications (`IMMNotificationClient`), float32 auto-conversion with mix-format fallback, capture-loss recovery, and default render device reopening.
- **WebRTC (`Libraries/LibWebRTC/Rust/`, `Services/WebRTCClient/`, `Libraries/LibWeb/WebRTC/`)**:
  - Rust `webrtc` service caps resources (256 PeerConnections, 1,024 transceivers / senders / data channels), restricts Interactive Connectivity Establishment (`ICE`) to IPv4 UDP (`Udp4`), filters privileged STUN/TURN ports (`< 1024` except `53`, `80`, `443`), resolves hostnames via renderer `ResolveHost` (bypassing initialization deferral for `InitTransport` and RTP Synchronization Source `SSRC` lookups), applies Multicast DNS (`mDNS`) host obfuscation unless Microphone permission is granted, strips non-mDNS/`ssltcp` candidates, and registers `video/rtx` Retransmission Payload Types (`RTX`/`FID`) after default interceptors.
  - `video_rx.rs` implements out-of-order RTP sequence reassembly (`SeqUnwrapper`, 128-packet buffer) and depacketization for VP8, VP9, H.264 (caching `STAP-A` `SPS`/`PPS` parameter NALUs), and AV1 (`OBU` assembly).
  - `RTCPeerConnection` handles 48 kHz Opus audio TX/RX with Root-Mean-Square Voice Activity Detection (`RMS VAD`), Windows-only WMF H.264/VP8 video TX encoding, and WMF (Windows H.264) / FFmpeg video RX decoding.
- **`TabRecorder` and `SpeakerTimeline` (`UI/Qt/TabRecorder.cpp`, `Libraries/LibMediaCodecs/SpeakerTimeline.cpp`)**:
  - Windows-only (`Q_OS_WIN`) recording via toolbar `RecordButton` or `Ctrl+Shift+R`. Enables CPU backing stores (`set_recording_active`), samples frames up to `1920x1080` on a `Qt::PreciseTimer`, composites cursor/click indicators, mixes WASAPI loopback + microphone signed 16-bit little-endian (`S16LE`) `48 kHz` stereo PCM, and writes sequential `MP4` batches (`part_NNN.mp4`, `part_NNN.speakers.json`, `session.json`, `concat.txt`) inside `<recordings_dir>/<session_id>/` (rotating every `300 s` by default via `batch_duration_seconds`, configurable across presets `0`–`900 s` where `0` disables duration-based rotation) using `WindowsMediaFoundationMp4Recorder` (Multi-Threaded Apartment `MTA` COM `IMFSinkWriter`, H.264 + AAC) so completed parts persist if the session aborts unexpectedly.
  - `SpeakerTimeline` coalesces WebRTC activity and fallback WASAPI VAD turns using hardcoded Spanish speaker labels (`"Yo"` for local microphone, `"Asistente N"` for remote speakers), dropping segments `< 150 ms` (unless all raw segments are `< 150 ms`) and merging gaps `<= 500 ms`.

---

## 7. Security Model, Verified Risks, and Test Coverage

### 7.1 Hardened P0 Fixes and Remaining Architectural Risks

The P0 and Sprint 2 P1 hardening fixes resolve the critical audit findings across commits `81bb5684`–HEAD:

- **Private-tab isolation (`LibMCP` / `LibWebView` / `UI/Qt` / `Extensions`)**: `ViewImplementation::is_mcp_access_allowed`, `set_mcp_access_allowed`, `find_allowed_mcp_view`, `find_automation_view`, `list_allowed_tabs`, `active_allowed_tab`, `FlowsUI::send_state`, `BrowserWindow` (`MCPBanner`), `Tab` (`Allow MCP Access`), `RemoteView`, and `Hub::Application::open_automation_view` enforce `IsPrivate::No`. `ExtensionManager::collect_combined_filter_rules(IsPrivate)` and separate normal/private content-blocker buffers in `Application` enforce `allow_in_private` for DNR/adblock rules too.
- **MCP/HTTP, WebDriver & Upload hardening (`LibHTTP` / `LibMCP` / `LibWebView` / `WebContent`)**: `HttpRequest::from_raw_request` keeps `64 KiB` header limits while accepting up to `4 MiB` request bodies; `MCPServer` validates `Mcp-Session-Id` (`404` on mismatch) and caps concurrent active connections at `64` (`503`); `MCPViewQueue` uses a `35 s` command timeout with `ViewImplementation::cancel_webdriver_content_command` and safe `WebDriverConnection::run_command` superseding; `validate_mcp_upload_path` rejects `\r`/`\n`, UNC paths, and OS Ladybird profile dirs, `Application::upload_file` resolves `<input type="file">` (including via Shadow DOM upload triggers) within `effective_upload_directory()`, and `WebDriverConnection::element_send_keys_impl` enforces a `512 MiB` file size cap; `mcp_webmcp_shim_js` freezes `modelContext`/`api` (`Object.freeze`) and defines `document.modelContext`/`navigator.modelContext` `configurable: false`.
- **Extensions/DNR safety**: `ExtensionStore::set_last_error` supports `notify_observers = false` (eliminating `collect_combined_filter_rules` / `sync_bundled_flows` re-entrancy), `install_unpacked` rejects conflicting directory IDs, `sync_bundled_flows` rejects unowned `FlowStore` ID collisions, and `translate_dnr_ruleset_to_adblock_rules` validates domains and strips `\r`/`\n`/`$`/`##` injections.
- **WASAPI, WMF & `TabRecorder` (`AudioServer` / `LibMediaCodecs` / `UI/Qt`)**: `PlaybackStreamWasapi` and `RecordStreamWasapi` eliminate uninitialized `GetMixFormat`, fatal `VERIFY` format panics, `channel_volumes` races, detached render threads, and capture-thread self-deletion UAF. `WindowsMediaFoundationCamera` guards odd-dimension `NV12` bounds with `Checked<size_t>`, `WindowsMediaFoundationVideoEncoder` bounds `OutputTypeNeeded` renegotiation (`<= 4` attempts), `WindowsMediaFoundationVideoDecoder::ThreadData` releases `m_mutex` before completion callbacks (guarded by `m_callback_mutex`), and `TabRecorder` swaps `m_recorder` atomically during batch rotation, eliminates `< 1 ms` audio busy-spin, propagates worker errors via `handle_worker_failure`, and bounds `start()` init waits (`10 s`).
- **Windows AIA & WebRTC endpoint hardening (`RequestServer` / `LibWebRTC`)**: `AIA.cpp` filters non-public HTTP AIA URLs and enforces Subject Alternative Name presence after Windows CryptoAPI fallback; `service.rs` filters remote ICE candidates targeting ports `< 1024` (other than `53`, `80`, `443`) or link-local/unspecified/broadcast/multicast endpoints.

- **WebRTC (`RTCPeerConnection` / `LibWebRTC`)**: `RTCPeerConnection::finalize()` performs GC-safe native teardown (deactivating outgoing audio/video `FrameSink` objects, closing native connections, clearing decoders and pending request tables, and unregistering IDs without dereferencing swept GC siblings), and remote RTP video/audio packet reception allocates `FixedArray<u8>` fallibly on OOM.

Remaining hardening / P2 architectural risks:

| Severity | Subsystem | Verified Hazard and Location |
| :--- | :--- | :--- |
| **High (`P1`)** | `LibMCP` (`upload_file`) | On Windows, `FileSystem` inspects reparse points and `validate_mcp_upload_path` validates path components, though `FileSystem::real_path` itself returns an absolute path and path validation runs prior to `WebContent` file open (`WebDriverConnection`). `upload_file` confines uploads to `effective_upload_directory()` (default `<Videos>/Ladybird Uploads`, customizable in `about:flows`/`about:settings`, or locked via `--mcp-upload-dir`). |
| **High (`P1`)** | `Extensions` / WebMCP | Both systems inject asynchronously via `execute_script` into the Main World after commit (no isolated JS world; early `<head>` scripts may run first), and `permissions`/`host_permissions` remain unenforced metadata in JS. (`consequentialHint`/`destructiveHint` `call_page_tool` gates behind native Qt `QMessageBox` approval tied to the document and tool registration; `FlowRunner` blocks tainted `extract`/`eval` variables in `upload_file` paths and cross-origin `open` navigations.) |
| **Medium (`P2`)** | `WebRTC` / Windows | Windows `WebRTCClient` and `WebContent` are unsandboxed (`SandboxUnimplemented.cpp`). |

### 7.2 Verification Suites (Provenance & Scope)

| Test Binary / Suite | Scope / Verified Count | Command / Scope Covered |
| :--- | :---: | :--- |
| `TestLibWebView` (`TestContainers`) | `2/2` (Windows) | `./Build/release/bin/TestLibWebView[.exe] --suite TestContainers` — `ContainerStore` defaults/persistence and per-container `BrowsingSession`/`CookieJar` isolation |
| `TestLibWebView` (`TestExtensions`) | `9/9` (Windows) | `./Build/release/bin/TestLibWebView[.exe] --suite TestExtensions` — paths/symlinks, URL patterns, manifests, store, injection, DNR translation |
| `TestLibWebView` (`TestMCPAutomation`) | `24/24` (Windows, uses `FakeMCPDelegate` + direct store/shield helpers) | `./Build/release/bin/TestLibWebView[.exe] --suite TestMCPAutomation` — Auth/Host/Origin guards, 58 schemas + SSE & resource subscriptions (`ladybird://notifications*`), SEP-2640 protocol, flows/extensions, agentic/WebMCP roundtrips, upload path validation, Skills (`ladybird-agentic-guide` built-in seeding/preservation), Blind Vault/TOTP/quarantine, MCP policies, `MCPContentShield` anti-injection & nonce, pause kill switch, `on_mcp_tool_audit` hook, `take_screenshot` visual `security_envelope` & `redact_media`, `inspect_site_manifests` boundary, ephemeral/custom containers & live `migrate_tab_container`, UA/upload/`mcpAutoStart` settings |
| `TestLibWebView` (`TestAutomationFlows`) | `10` cases | `./Build/release/bin/TestLibWebView[.exe] --suite TestAutomationFlows` — IDs, `source` preservation during edits, legacy fallback, roundtrip JSON, template var extraction, `{{var}}`, locator escaping, `upload_file` + Shadow DOM script, `FlowStore` persistence |
| `TestLibWebView` (`TestOmnibox`) | `79/79` (Windows baseline) | `./Build/release/bin/TestLibWebView[.exe] --suite TestOmnibox` — URL, history, and `about:` WebUI suggestions (`about:agent`, `about:skills`, `about:vault`, `about:flows`, `about:extensions`) |
| `TestUIQt` (all Windows Qt suites) | `270/270` (Windows baseline) | `./Build/release/bin/TestUIQt[.exe]` — Qt chrome, session store schema v13 + `container_id` persistence (`TestSessionStore` `114/114`), snapshot storage (`61/61`), site settings, `TabRecorder` |
| `TestLibMedia` (`TestWindowsMediaFoundationEncode.cpp`) | `11` test cases (compiled on `WIN32`) | `./Build/release/bin/TestLibMedia[.exe] --suite TestWindowsMediaFoundationEncode` — WMF H.264/VP8, camera sharing, `SpeakerTimeline`, `WindowsMediaFoundationMp4Recorder` |
| `Tests/LibWeb/Text/input/WebRTC/*.html` | `17` text tests | PeerConnection lifecycle, audio/video loopback, mDNS obfuscation, permissions, RTX/FID, transforms |
| `Tests/LibWebView/test-webmcp-approval.mjs` | `4` cases | `node --test Tests/LibWebView/test-webmcp-approval.mjs` — renderer approval tickets, document and registration replacement, and declarative action changes |
| `Tools/e2e-ladybird-engine/test/engine-protocol.test.mjs` | `1` unit pass + `1` gated e2e integration (`E2E_REPO_PATH`) | `node --test Tools/e2e-ladybird-engine/test/engine-protocol.test.mjs` — Protocol framing + mocked MCP engine roundtrip (`e2e` live test skips unless `E2E_REPO_PATH` is set) |
