# `LibMCP` (`Libraries/LibMCP`)

> Embedded HTTP + JSON-RPC 2.0 Model Context Protocol (`MCP`) server for Ladybird (`0aa20e0a`).  
> See [Fork Architecture and Capabilities](../../Documentation/ForkArchitectureAndCapabilities.md), [Automation README](../LibWebView/Automation/README.md), and [Extensions README](../LibWebView/Extensions/README.md).

## 1. Architecture and Files

`LibMCP` embeds a local MCP server inside Ladybird's `Hub` (`WebView::Application`) backend with no external runtime dependencies:

| File | Responsibility |
| :--- | :--- |
| [`Forward.h`](Forward.h) | Forward declarations (`MCPServer`, `MCPDelegate`, `TabInfo`, `ToolCallResult`) |
| [`MCPDelegate.h`](MCPDelegate.h) | Pure-virtual delegate interface bridging `MCPServer` requests to `WebView::Application` |
| [`MCPServer.h`](MCPServer.h) | Public server lifecycle (`create`, `local_port`, `auth_token`, `session_id`) |
| [`MCPServer.cpp`](MCPServer.cpp) | Loopback TCP listener, HTTP parser/validation, JSON-RPC 2.0 router, schema definitions for 43 tools |
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

## 3. Transport, Authentication, and Protocol (`43` Tools)

- **Endpoint & JSON-RPC 2.0 (`MCPServer::Connection`)**: Serves `POST /mcp` (`Content-Type: application/json`) and stateless `DELETE /mcp` (`200 {"ok":true}`); non-`/mcp` paths return `404`; other HTTP methods return `405` (`Allow: POST, DELETE`). `initialize` returns `protocolVersion: "2025-03-26"`, `capabilities.tools.listChanged: false`, `instructions`, and header `Mcp-Session-Id: <uuid>`. Notifications (no `"id"`) return `202 Accepted`; `"id": null` returns JSON-RPC error `-32600` (`Invalid Request`).
- **43 Registered Tools (`tools/list` / `tools/call`, top-level `additionalProperties: false`)**:

| Group | Count | Tools |
| :--- | :---: | :--- |
| Tabs & Navigation | 9 | `list_tabs`, `get_active_tab`, `open_tab`, `switch_tab`, `close_tab`, `navigate`, `reload`, `go_back`, `go_forward` |
| DOM, Input & Diagnostics | 18 | `get_accessibility_tree`, `get_dom_tree`, `get_page_source`, `find_elements`, `click`, `type_text`, `get_page_snapshot` (`@e1` handles), `press_key`, `scroll`, `hover`, `drag`, `upload_file`, `select_option`, `wait_for`, `get_console_logs`, `get_network_requests`, `eval_js`, `take_screenshot` (Base64 PNG `image`) |
| Automation Flows | 8 | `list_flows`, `get_flow`, `save_flow`, `delete_flow`, `run_flow`, `list_flow_runs`, `get_flow_run`, `cancel_flow_run` |
| Extensions | 6 | `list_extensions`, `install_unpacked_extension`, `set_extension_enabled`, `reload_extension`, `remove_extension`, `inject_extension` |
| WebMCP Page Tools | 2 | `list_page_tools`, `call_page_tool` |

  For compatibility with clients that serialize nested objects as strings, `save_flow` (`flow`) and `call_page_tool` (`arguments`) accept both JSON objects and JSON-encoded strings.

## 4. Security Boundaries and Known Caveats

- **Transport Controls**: Binds strictly to `127.0.0.1` (`MCPServer::create`); enforces a `30 s` HTTP read timeout (`408 Request Timeout`); rejects non-loopback `Host` headers (`403`) and any request carrying an `Origin` header (`403`); validates `Authorization: Bearer <token>` in constant time (`401`); sets `Connection: close`, `Cache-Control: no-store`, and `X-Content-Type-Options: nosniff`.
- **Effective Request Cap (`64 KiB` vs `4 MiB`)**: Although `MAX_REQUEST_BYTES` buffers up to `4 MiB` on the socket, `HTTP::HttpRequest::from_raw_request` rejects any request buffer `> 65,536` bytes (`>= 65,537` bytes) with HTTP `413`. Large `eval_js` or `save_flow` payloads exceeding `64 KiB` fail.
- **Session ID & Connection Count**: `Mcp-Session-Id` is emitted by `initialize` but not validated on later calls, and `MCPServer::on_new_client` imposes no connection count cap.
- **Per-Tab Opt-In & Private-Tab Isolation**: User tabs are excluded from `list_tabs` and rejected by tool calls until opted in (`ViewImplementation::set_mcp_access_allowed(true)`); `open_tab` and `Application::open_automation_tab` auto-enable access on non-private tabs (`Hub::Application::open_mcp_new_tab` seeds only from non-private tabs and forces `IsPrivate::No`). Private views (`IsPrivate::Yes`) are rejected in `ViewImplementation::set_mcp_access_allowed`, `find_allowed_mcp_view`, `find_automation_view`, `list_allowed_tabs`, `active_allowed_tab`, `FlowsUI`, `BrowserWindow`, `Tab`, and `RemoteView`.
- **Command Queue Timeout (`35 s`) & Recovery**: `Application::dispatch_next_mcp_content_command` waits `35 s` (exceeding `WebDriverConnection`'s `30 s` `script_timeout`) and calls `ViewImplementation::cancel_webdriver_content_command` on timeout or revocation (`fail_mcp_requests_for_view`) so `m_webdriver_input_command_running` is cleared cleanly and queued input commands never wedge. `WebDriverConnection::run_command` supersedes any stale in-flight command instead of hitting `VERIFY(!m_current_command_id.has_value())`.
- **`drag` & `upload_file` Guards**: `Application::drag` rejects inactive or hidden tabs immediately (`switch_tab` required). `Application::upload_file` verifies the target element is `<input type="file">` (for both `ref`/`selector` JS resolution and direct `element_id` via `get_element_tag_name` + `get_element_attribute`), checks tab access before touching disk, and delegates to `validate_mcp_upload_path` (rejects relative paths, `\0`/`\r`/`\n` control characters, UNC `\\`/`//` paths, `..`, leaf symlinks via `FileSystem::is_link`, directories/non-regular files, and credential/profile paths including `.ssh`, `.gnupg`, `.aws`, `.kube`, `.azure`, `/.config/gcloud/`, `/ladybird/profiles/`, `/appdata/local/ladybird/`, `/appdata/roaming/ladybird/`, `/.local/share/ladybird/`, `/library/application support/ladybird/`, `.env*`, private keys, `Cookies`, `Login Data`, `Web Data`, `SAM`, `NTUSER.DAT`). When `--mcp-upload-dir` is omitted, remaining non-symlink regular files outside the blocklist are permitted, parent directory symlinks are checked only inside `--mcp-upload-dir`, and `WebDriverConnection` still reads selected upload files via `read_until_eof()`.
- **Extension Install Gate**: `Application::install_unpacked_extension` returns an error over MCP unless Ladybird was launched with `--mcp-allow-extension-install`.
- **WebMCP Phase 1 (`document.modelContext` / `navigator.modelContext`)**: `inject_webmcp_shim_if_needed` injects on `http:`/`https:` MCP-allowed non-private tabs, exposes frozen (`Object.freeze`) `modelContext` globals (`configurable: false`) on Secure Contexts (`window.isSecureContext`, `https:`, `localhost`, `127.0.0.1`, `[::1]`), freezes `window.__ladybirdWebMCP` (`api`), captures built-in references (`Map`, `Set`, `Array.from`, `Object.defineProperty`, `Object.freeze`, `Object.prototype.hasOwnProperty`, `JSON.parse`, `JSON.stringify`), scans `document` and open `shadowRoot` `form[toolname]` elements, marks tools `untrustedContentHint: true`, and checks `confirm_consequential` in JS. Remaining caveats: injection happens asynchronously in the page's Main World after load starts (early `<head>` scripts may run first) and `confirm_consequential` has no native Qt confirmation dialog.

## 5. Testing

Run the unit test suite (`6` test cases at commit `0aa20e0a`):

```powershell
./Build/release/bin/TestLibWebView[.exe] --suite TestMCPAutomation
```

Covers: `mcp_auth_and_origin_guards`, `mcp_initialize_and_tools_list_schemas`, `mcp_automation_flow_lifecycle`, `mcp_extensions_lifecycle`, `mcp_agentic_tools_roundtrip`, and `mcp_upload_path_security_guards`. Note that `TestMCPAutomation` tests `MCPServer` against `FakeMCPDelegate` plus `validate_mcp_upload_path` directly; `WebView::Application` handler paths are tested via ad-hoc live Python scripts against `Ladybird --mcp`.
