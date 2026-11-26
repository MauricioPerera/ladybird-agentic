# `LibMCP` (`Libraries/LibMCP`)

> Embedded HTTP + JSON-RPC 2.0 Model Context Protocol (`MCP`) server for Ladybird (`feat/extensions-dnr-and-dom-tools` / `main`).  
> See [Fork Architecture and Capabilities](../../Documentation/ForkArchitectureAndCapabilities.md), [Automation README](../LibWebView/Automation/README.md), and [Extensions README](../LibWebView/Extensions/README.md).

## 1. Architecture and Files

`LibMCP` embeds a local MCP server inside Ladybird's `Hub` (`WebView::Application`) backend with no external runtime dependencies:

| File | Responsibility |
| :--- | :--- |
| [`Forward.h`](Forward.h) | Forward declarations (`MCPServer`, `MCPDelegate`, `TabInfo`, `ToolCallResult`) |
| [`MCPDelegate.h`](MCPDelegate.h) | Pure-virtual delegate interface (`TabInfo`, `SkillMatchHint`, `CredentialMatchHint`, `ProtocolCallResult`, `OnProtocolCallComplete`) bridging `MCPServer` requests to `WebView::Application` |
| [`MCPServer.h`](MCPServer.h) | Public server lifecycle (`create`, `local_port`, `auth_token`, `session_id`) |
| [`MCPServer.cpp`](MCPServer.cpp) | Loopback TCP listener, HTTP parser/validation, JSON-RPC 2.0 + SEP-2640 router, schema definitions for 50 tools |
| [`CMakeLists.txt`](CMakeLists.txt) | Builds `LibMCP` linking `PRIVATE` targets `LibCore`, `LibHTTP`, and `LibURL` |

## 2. Quickstart

1. **Launch Ladybird with MCP enabled**:

   ```powershell
   # Windows
   .\Build\release\bin\Ladybird.exe --mcp
   # Linux / macOS
   ./Build/release/bin/Ladybird --mcp
   ```

   Pass `--mcp-token <token>` for a fixed token, or read the generated 64-hex-character token from stdout (`MCP server listening on http://127.0.0.1:7331/mcp (token: ...)`). In the UI, click **Copy Config** on the bottom `MCPBanner` to copy `{"url":"http://127.0.0.1:7331/mcp","headers":{"Authorization":"Bearer <token>"}}` to the clipboard, and click **Allow This Tab** (or use the tab context menu -> **Allow MCP Access**) on any user tab you want to expose.

2. **Initialize a client session**:

   ```json
   POST http://127.0.0.1:7331/mcp
   Authorization: Bearer <token>
   Content-Type: application/json

   {"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
   ```

## 3. Transport, Authentication, and Protocol (`50` Tools + `5` SEP-2640 Methods)

- **Endpoint & JSON-RPC 2.0 (`MCPServer::Connection`)**: Serves `POST /mcp` (`Content-Type: application/json`) and stateless `DELETE /mcp` (`200 {"ok":true}`); non-`/mcp` paths return `404`; other HTTP methods return `405` (`Allow: POST, DELETE`). `initialize` negotiates `protocolVersion` `"2025-03-26"` or `"2026-07-28"` (with `resources` and `extensions["io.modelcontextprotocol/skills"].directoryRead: true` capabilities), returns `instructions`, and sets header `Mcp-Session-Id: <uuid>`. Notifications (no `"id"`) return `202 Accepted`; `"id": null` returns JSON-RPC error `-32600` (`Invalid Request`).
- **SEP-2640 & Discovery JSON-RPC Methods (5)**: `server/discover`, `skills/list`, `skills/get`, `resources/read`, `resources/directory/read`.
- **50 Registered Tools (`tools/list` / `tools/call`, top-level `additionalProperties: false`)**:

| Group | Count | Tools |
| :--- | :---: | :--- |
| Tabs & Navigation | 9 | `list_tabs`, `get_active_tab`, `open_tab` (supports `container_id`), `switch_tab`, `close_tab`, `navigate`, `reload`, `go_back`, `go_forward` |
| DOM, Input & Diagnostics | 18 | `get_accessibility_tree`, `get_dom_tree`, `get_page_source`, `find_elements`, `click`, `type_text`, `get_page_snapshot` (`@e1` handles across open Shadow DOM & same-origin frames), `press_key`, `scroll`, `hover`, `drag`, `upload_file`, `select_option`, `wait_for`, `get_console_logs`, `get_network_requests`, `eval_js`, `take_screenshot` (Base64 PNG `image`) |
| Automation Flows | 8 | `list_flows`, `get_flow`, `save_flow`, `delete_flow`, `run_flow`, `list_flow_runs`, `get_flow_run`, `cancel_flow_run` |
| Extensions | 6 | `list_extensions`, `install_unpacked_extension`, `set_extension_enabled`, `reload_extension`, `remove_extension`, `inject_extension` |
| WebMCP Page Tools | 2 | `list_page_tools`, `call_page_tool` |
| Agent Skills | 4 | `list_skills`, `read_skill`, `save_skill` (`--mcp-allow-skill-write`), `delete_skill` (`--mcp-allow-skill-write`) |
| Blind Credential Vault | 3 | `list_credentials`, `fill_credential`, `login_with_credential` |

  For compatibility with clients that serialize nested objects as strings, `save_flow` (`flow`), `call_page_tool` (`arguments`), and `save_skill` (`skill`) accept both JSON objects and JSON-encoded strings. `TabInfo` exposes `container_id`, `matching_skills`, and `matching_credentials`.

## 4. Security Boundaries and Known Caveats

- **Transport Controls**: Binds strictly to `127.0.0.1` (`MCPServer::create`); enforces a `30 s` HTTP read timeout (`408 Request Timeout`); rejects non-loopback `Host` headers (`403`) and any request carrying an `Origin` header (`403`); validates `Authorization: Bearer <token>` in constant time (`401`); sets `Connection: close`, `Cache-Control: no-store`, and `X-Content-Type-Options: nosniff`.
- **Request Cap (`4 MiB` Body / `64 KiB` Headers), Session ID & Connection Cap**: `HTTP::HttpRequest::from_raw_request` caps individual request-line/header tokens at `65,536` bytes while permitting request bodies up to `4 MiB` (`MAX_REQUEST_BYTES`), returning `413` above either bound. Non-empty `Mcp-Session-Id` headers are validated in constant time against the active server session (`404` on mismatch), and `MCPServer::on_new_client` caps concurrent active sockets at `64` (`503 Service Unavailable` when exceeded).
- **Per-Tab Opt-In & Private-Tab Isolation**: User tabs are excluded from `list_tabs` and rejected by tool calls until opted in (`ViewImplementation::set_mcp_access_allowed(true)`); `open_tab` and `Application::open_automation_tab` auto-enable access on non-private tabs (`Hub::Application::open_mcp_new_tab` seeds only from non-private tabs and forces `IsPrivate::No`). Private views (`IsPrivate::Yes`) are rejected in `ViewImplementation::set_mcp_access_allowed`, `find_allowed_mcp_view`, `find_automation_view`, `list_allowed_tabs`, `active_allowed_tab`, `FlowsUI`, `BrowserWindow`, `Tab`, and `RemoteView`.
- **Command Queue Timeout (`35 s`) & Recovery**: `Application::dispatch_next_mcp_content_command` waits `35 s` (exceeding `WebDriverConnection`'s `30 s` `script_timeout`) and calls `ViewImplementation::cancel_webdriver_content_command` on timeout or revocation (`fail_mcp_requests_for_view`) so `m_webdriver_input_command_running` is cleared cleanly and queued input commands never wedge. `WebDriverConnection::run_command` supersedes any stale in-flight command instead of hitting `VERIFY(!m_current_command_id.has_value())`.
- **`drag` & `upload_file` Guards**: `Application::drag` rejects inactive or hidden tabs immediately (`switch_tab` required). `Application::upload_file` verifies the target element is `<input type="file">` (for both `ref`/`selector` JS resolution and direct `element_id` via `get_element_tag_name` + `get_element_attribute`), checks tab access before touching disk, and delegates to `validate_mcp_upload_path` (rejects relative paths, `\0`/`\r`/`\n` control characters, UNC `\\`/`//` paths, `..`, leaf symlinks via `FileSystem::is_link`, canonicalization failures on `FileSystem::real_path`, directories/non-regular files, and credential/profile paths including `.ssh`, `.gnupg`, `.aws`, `.kube`, `.azure`, `/.config/gcloud/`, `/ladybird/profiles/`, `/appdata/local/ladybird/`, `/appdata/roaming/ladybird/`, `/.local/share/ladybird/`, `/library/application support/ladybird/`, `.env*`, private keys, `Cookies`, `Login Data`, `Web Data`, `SAM`, `NTUSER.DAT`). `WebDriverConnection::element_send_keys_impl` enforces a `128 MiB` per-file upload cap; when `--mcp-upload-dir` is set, both canonicalized and un-resolved relative directory segments are checked with `FileSystem::is_link`; when omitted, non-symlink regular files outside the blocklist are permitted (note `FileSystem::real_path` does not expand NTFS junctions on Windows).
- **Extension & Skill Write Gates**: `Application::install_unpacked_extension` requires `--mcp-allow-extension-install`; `Application::save_skill_tool` and `delete_skill_tool` require `--mcp-allow-skill-write`.
- **CLI Policy Controls**: `--mcp-allowed-containers`, `--mcp-allowed-domains`, `--mcp-blocked-domains`, `--mcp-disable-eval-js` (also enforced in `call_page_tool` and MCP `FlowRunner` `eval`), and `--mcp-max-tabs` (default `16`).
- **Blind Credential Vault & Quarantine**: Encrypted via AES-256-GCM (`--mcp-vault-passphrase`, `LADYBIRD_VAULT_PASSPHRASE`, Windows DPAPI `master.dpapi`, or POSIX `0600` `master.key`), with origin/container pinning, `<input type="password">` + `action`/`formaction` checks, active-document-change taint quarantine across inspection/eval/snapshot/flow steps, and `scrub_secrets_from_string` output redaction. Privileged `about:*` pages (such as `about:vault` and `about:skills`) are excluded from MCP access.
- **WebMCP Phase 1 (`document.modelContext` / `navigator.modelContext`)**: `inject_webmcp_shim_if_needed` injects post-commit via `CanonicalTraversable::did_activate_history_entry` -> `did_start_load_for_extensions` on `http:`/`https:` MCP-allowed non-private tabs, exposes frozen (`Object.freeze`) `modelContext` globals (`configurable: false`) on Secure Contexts (`window.isSecureContext`, `https:`, `localhost`, `127.0.0.1`, `[::1]`), freezes `window.__ladybirdWebMCP` (`api`), captures built-in references (`Map`, `Set`, `Array.from`, `Object.defineProperty`, `Object.freeze`, `Object.prototype.hasOwnProperty`, `JSON.parse`, `JSON.stringify`), scans `document` and open `shadowRoot` `form[toolname]` elements, marks tools `untrustedContentHint: true`, and checks `confirm_consequential` in JS. Remaining caveats: injection still arrives asynchronously in the page's Main World (so early inline `<head>` scripts may run first) and `confirm_consequential` has no native Qt confirmation dialog.

## 5. Testing

Run the unit test suite (`13` test cases):

```powershell
./Build/release/bin/TestLibWebView[.exe] --suite TestMCPAutomation
```

Covers: `mcp_auth_and_origin_guards`, `mcp_initialize_and_tools_list_schemas`, `mcp_automation_flow_lifecycle`, `mcp_extensions_lifecycle`, `mcp_agentic_tools_roundtrip`, `mcp_upload_path_security_guards`, `mcp_skills_store_and_url_matching`, `mcp_skills_sep2640_protocol`, `mcp_skills_tools_and_write_gate`, `mcp_vault_crypto_totp_and_origin_pinning`, `mcp_vault_blind_fill_and_quarantine`, `mcp_security_policy_guards`, and `settings_user_agent_preset_roundtrip`. Note that `TestMCPAutomation` tests `MCPServer` against `FakeMCPDelegate` plus `VaultStore`, `SkillStore`, `Settings`, and policy/upload helpers directly; full end-to-end `WebView::Application` UI/page paths run against `Ladybird --mcp`.
