# Native MV3 Extensions and `declarativeNetRequest` (`Libraries/LibWebView/Extensions`)

> Unpacked Manifest V3 (`MV3`) extension loader (loaded directly from local folders), `declarativeNetRequest` (`DNR`) rule compiler, content-script injector, and `about:extensions` integration (`feat/extensions-dnr-and-dom-tools` / `main`).  
> See [Fork Architecture and Capabilities](../../../Documentation/ForkArchitectureAndCapabilities.md), [LibMCP README](../../LibMCP/README.md), and [Automation README](../Automation/README.md).

## 1. Architecture and Files

| File | Responsibility |
| :--- | :--- |
| [`ExtensionTypes.h`](ExtensionTypes.h), [`ExtensionTypes.cpp`](ExtensionTypes.cpp) | Manifest schema & parser, URL match patterns, `resolve_safe_extension_file_path`, DNR-to-adblock translator, JS/CSS wrapper builder |
| [`ExtensionStore.h`](ExtensionStore.h), [`ExtensionStore.cpp`](ExtensionStore.cpp) | Registry (`<data_dir>/extensions/registry.json`), per-extension `storage/<id>.json`, install/reload/remove, example generator |
| [`ExtensionManager.h`](ExtensionManager.h), [`ExtensionManager.cpp`](ExtensionManager.cpp) | Bundled flow synchronization, combined filter list generation, `did_start_load` / `did_finish_load` (invoked by `CanonicalTraversable::commit_navigation` / `WebContentPage::did_finish_loading`), and manual injection |
| [`../WebUI/ExtensionsUI.h`](../WebUI/ExtensionsUI.h), [`../WebUI/ExtensionsUI.cpp`](../WebUI/ExtensionsUI.cpp) | `about:extensions` WebUI handlers (`loadState`, `installUnpacked`, `createExampleExtension`, `setEnabled`, `setAllowInPrivate`, `reloadExtension`, `removeExtension`, `getStorage`, `setStorage`, `injectActiveTab`) |
| [`Base/res/ladybird/about-pages/extensions.html`](../../../Base/res/ladybird/about-pages/extensions.html) | Extension management WebUI page |

## 2. Supported MV3 Manifest Subset

### Minimal Manifest Example

```json
{
  "manifest_version": 3,
  "name": "Example Extension",
  "version": "1.0.0",
  "permissions": ["storage", "declarativeNetRequest", "flows"],
  "content_scripts": [{ "matches": ["https://*/*"], "js": ["content.js"], "run_at": "document_idle" }],
  "declarative_net_request": { "rule_resources": [{ "id": "ads", "enabled": true, "path": "rules.json" }] },
  "ladybird": { "id": "example-ext", "flows": ["flows/smoke.json"], "filter_lists": ["filters.txt"] }
}
```

### Manifest Fields

- **Required**: `manifest_version: 3`, non-empty `name`, non-empty `version`.
- **ID resolution**: Top-level JSON `"id"` -> `ladybird.id` -> `fallback_id` (sanitized directory basename in `ExtensionStore::install_unpacked`; existing ID in `ExtensionStore::reload_extension`) -> sanitized `name`. Validated by `is_valid_extension_id` (`1..64` chars, `[a-z0-9][a-z0-9_-]*`).
- **Permissions & Host Permissions**: Validates `permissions` against `"storage"`, `"tabs"`, `"scripting"`, `"flows"`, `"mcp"`, `"activeTab"`, `"contextMenus"`, `"declarativeNetRequest"`, and parses `host_permissions` (array of strings). *(Note: parsed for manifest validation and UI display; not enforced at runtime.)*
- **`content_scripts`**: Requires non-empty `matches` and at least one relative `js` or `css` path (validated by `resolve_safe_extension_file_path`); optional `exclude_matches` (omitted when serialized by `serialize_extension`) and `run_at` (`"document_start"`, `"document_end"`, or `"document_idle"` default). URL matching supports `http:`/`https:` schemes only (`<all_urls>`, scheme wildcards, `*.subdomain`, exact host; ports ignored).
- **`action`**: `default_title`, `default_popup` (existence-checked, not rendered in UI), `default_flow_id` (metadata only; no popup/toolbar trigger wired).
- **`declarative_net_request.rule_resources`**: Array of `{ id, enabled, path }` JSON rulesets.
- **`ladybird` manifest key**: `id`, `flows` (synced into `FlowStore` when enabled; disabled or removed extensions delete their synced flows), and `filter_lists` (adblock-syntax text files).

## 3. DNR Translation, Content Blocker Sync, and Injection

1. **DNR Translation (`translate_dnr_ruleset_to_adblock_rules`)**:
   - Translates `"block"` and `"allow"` (`@@` exception prefix) rules into adblock lines using `urlFilter` (takes precedence over `requestDomains` on the same rule), `initiatorDomains`, `excludedInitiatorDomains`, and mapped `resourceTypes` (`script`, `stylesheet`, `image`, `xmlhttprequest`, `font`, `media`, `websocket`, `other`, `sub_frame` -> `subdocument`).
   - `ExtensionManager::collect_combined_filter_rules()` merges all enabled extensions' `filter_lists` and enabled DNR rulesets into `Application::load_content_blocker_lists()` (`AnonymousBuffer` pushed via `async_set_content_blockers` to `WebContentClient` -> Rust `adblock`).
2. **Script & CSS Injection (`ExtensionManager` + `CanonicalTraversable` / `WebContentPage`)**:
   - `CanonicalTraversable::commit_navigation` calls `Application::did_start_load_for_extensions` -> `ExtensionManager::did_start_load` (`InjectionPhase::DocumentStart`, `run_at == "document_start"`) immediately after committing the destination document. `WebContentPage::did_finish_loading` calls `did_finish_load_for_extensions` -> `ExtensionManager::did_finish_load` (`InjectionPhase::DocumentEndOrIdle`, `run_at != "document_start"`).
   - Manual injection (`ExtensionManager::inject_extension_into_view` / `inject_extension` MCP tool) uses `InjectionPhase::All` with `match_url = false`, skipping only `about:` pages while checking `allow_in_private`.
   - `build_extension_injection_script` injects `<style data-ladybird-ext-id="...">` elements and executes JS in the page's **Main World** (shared JS environment, no isolated world) via `WebDriverConnection::execute_script` (`navigator.webdriver` stays `false`) inside an IIFE receiving `(browser, ladybirdExt)` backed by one runtime/storage object (`{ id, name, runtime: { id }, storage: { local: { _snapshot, get } } }`).

## 4. Security Boundaries and Known Caveats

- **Symlink & Path Escape Guard (`resolve_safe_extension_file_path`)**: Enforced by both `ExtensionStore::verify_manifest_files_exist` and `ExtensionManager::read_extension_text_file`. Rejects absolute paths, `\`, `:`, `.`, `..`, NUL bytes, checks every directory segment with `FileSystem::is_link`, canonicalizes via `FileSystem::real_path`, enforces containment under `root_path`, and verifies `FileSystem::is_regular_file`. Note that `install_unpacked` stores `LexicalPath::canonicalized_path` rather than `real_path` as `root_path`.
- **MCP Install Gate**: Installing unpacked extensions via MCP (`install_unpacked_extension`) is blocked by default unless Ladybird launches with `--mcp-allow-extension-install` AND receives native Qt user approval (or `--headless` with `--mcp-auto-approve`); `about:extensions` remains interactive.
- **Private Tab Isolation**: Content script injection and `ExtensionManager::collect_combined_filter_rules(IsPrivate)` both check `ext.allow_in_private` (`false` default); `Application::load_content_blocker_lists()` builds separate normal (`m_content_blocker_list_buffer`) and private (`m_private_content_blocker_list_buffer`) buffers per `WebContentClient::is_private()`.
- **Post-Commit Main World Injection**: `did_start_load_for_extensions` fires after `CanonicalTraversable::commit_navigation` (preventing cross-origin injection onto the outgoing document), though `execute_script` still delivers into the destination document's **Main World** asynchronously over IPC rather than synchronously inside `document` creation.
- **Re-Entrancy, DNR Sanitization & Collision Guards**: `ExtensionManager` passes `notify_observers = false` to `ExtensionStore::set_last_error` so filter/flow/injection errors never trigger re-entrant `extensions_changed()` cascades. `translate_dnr_ruleset_to_adblock_rules` validates domains (`[a-zA-Z0-9.-]`, `<= 253` chars) and rejects `urlFilter` strings containing `\r`, `\n`, `\0`, `$`, `##`, `#@#`, `#?#`, or leading `!`/`[` (still ignores `redirect`, `modifyHeaders`, and `regexFilter`). `install_unpacked` rejects installing a different directory under an already-installed `manifest.id`, and `sync_bundled_flows` refuses overwriting unowned flow IDs in `FlowStore`. Manual injection bypasses `matches` / `exclude_matches` and allows `file:` / `data:` tabs.

## 5. Bundled Extensions

- `Tools/example-extension/` (`ladybird-page-inspector`): Heading/link badge + sample flow (`ext-page-inspector-smoke`).
- `Tools/extensions/table-enhancer/` (`ladybird-table-enhancer`): HTML table column filtering, multi-type sorting, reset, and CSV export (automatically skips webmail/app layout tables such as Google Chat/Mail/Calendar/Outlook, `role="grid|treegrid|presentation"`, and `>= 8` controls/nested tables).
- `Tools/extensions/youtube-clean-and-adblock/` (`ladybird-youtube-clean-adblock`): 7 DNR block rules (`rules.json`), 18 adblock filters (`filters.txt`), `early.js` (`fetch`/`XHR`/`ytInitialPlayerResponse` ad pruning), and `content.js`/`content.css` (DOM cleanup, ad skip/fast-forward, Shorts redirect).

## 6. Testing

Run the unit test suite (`9` test cases):

```powershell
./Build/release/bin/TestLibWebView[.exe] --suite TestExtensions
```

Covers ID/relative path validation, match patterns, manifest errors, store + example creation, injection script generation, DNR translation, DNR/filter file checks, and `resolve_safe_extension_file_path` escape/symlink rejection (symlink subtest runs on non-Windows hosts). `ExtensionManager` runtime injection, `allow_in_private`, and `ExtensionsUI` currently lack unit test coverage.
