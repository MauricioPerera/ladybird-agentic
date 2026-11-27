# Ladybird Agentic & Privacy Fork (`feat/extensions-dnr-and-dom-tools` / `main`)

> Private fork of [Ladybird](https://ladybird.org) extending upstream (`60d8303b`) with embedded MCP/WebMCP automation, per-page Agent Skills (`SEP-2640`), a Blind Credential Vault, isolated Tab Containers, native MV3 extensions + DNR content blocking, Windows TLS/WebRTC/MP4 recording support, and modern web-compatibility fixes.
>
> Upstream `README.md` is intentionally kept unmodified to keep future `upstream/master` merges clean.

## Capability Layers

1. **Embedded MCP Server (`LibMCP`, `54` Tools + `9` Protocol Methods + SSE Push) & Agent Security Shield v1/v2**: Loopback HTTP/JSON-RPC 2.0 + SSE (`GET /mcp`) server (`127.0.0.1:7331/mcp`) with Bearer auth, `Mcp-Session-Id`, `Origin`/`Host` (`localhost`/`127.0.0.1`/`[::1]`) guards, Shadow DOM & same-process iframe deep traversal (`@e1` snapshot refs with visible-only TreeWalker excerpts, visibility/opacity/clip/off-screen filtering, automatic modal prioritization, and optional `root_selector`), `MCPContentShield` anti-injection sanitization (strips invisible Unicode Tags/BiDi/zero-width/soft-hyphen/Braille-blank/variation chars & `<!-- ... -->`/`--!>` HTML comments, attaches randomized per-response `security_envelope.boundary_nonce` and `injection_risk_signals`), per-tab 4-mode MCP access (`Blocked`/`ReadOnly`/`SameOrigin`/`Full` with automatic `Blocked` lock on privileged `about:`/`resource:` pages and `about:blank` origin pinning), **Cross-Tab / Egress Taint Guard** gating cross-origin `open_tab`, `navigate`, unapproved mutations, `call_page_tool`, and `FlowRunner` steps after reading non-`"agent"` tabs, live **Pause/Resume Agent** Kill Switch canceling in-flight flows/queues/approvals/skills/SSE pushes, native TOCTOU-guarded Qt approval prompts for uploads/credentials/consequential `call_page_tool`/`install_unpacked_extension`/egress actions, live **`about:agent`** audit & shield console (accessible from `MCPBanner` -> **Shield**), isolated `"agent"` container default, and WebMCP Phase 1 (`document.modelContext` / `navigator.modelContext`). See [LibMCP README](Libraries/LibMCP/README.md).
2. **Per-Page Agent Skills (`about:skills`, `SkillStore`, SEP-2640)**: URL-matched Markdown instructions with YAML frontmatter, SHA-256 resource digests, symlink/traversal guards, starter templates (`youtube`, `youtube-studio`, `google-maps`, `google-search`, `linkedin`), and protocol discovery via `skills/list`, `skills/get`, `resources/read`, `resources/directory/read`.
3. **Blind Credential Vault & MCP Security Policies (`about:vault`, `VaultStore`)**: Encrypted credential storage (AES-256-GCM; Windows DPAPI `master.dpapi`, PBKDF2-HMAC-SHA256 via `--mcp-vault-passphrase` / `LADYBIRD_VAULT_PASSPHRASE`, or local `master.key`), RFC 6238 TOTP generation, origin & container pinning, `<input type="password">` + form-action guards, active-document-change taint quarantine, and output secret scrubbing.
   > [!WARNING]
   > **Non-Windows Vault Key Storage:** When neither `--mcp-vault-passphrase` nor `LADYBIRD_VAULT_PASSPHRASE` is set, Linux/macOS store the random 32-byte AES key in a `0600` plaintext `<data_dir>/vault/master.key` file (Windows wraps the key with user DPAPI `master.dpapi`). Pass a passphrase or set `LADYBIRD_VAULT_PASSPHRASE` for passphrase-derived protection on POSIX systems.
4. **Declarative Browser Automation (`about:flows`, `FlowStore`, `FlowRunner`)**: JSON flow persistence under `<data_dir>/flows/`, `16` actions (including `upload_file` + open Shadow DOM locator traversal), `7` locators, browser-managed Upload Directory (`about:flows` / `about:settings`, default `<Videos>/Ladybird Uploads`), and optional `e2e` Engine Protocol v1 adapter ([Tools/e2e-ladybird-engine](Tools/e2e-ladybird-engine/README.md)). See [Automation README](Libraries/LibWebView/Automation/README.md).
5. **Native MV3 Extensions & `declarativeNetRequest` (`about:extensions`)**: Unpacked MV3 content scripts, DNR-to-adblock translation into `Web::ContentBlocker`, bundled extensions in `Tools/`, and Windows CryptoAPI / AIA TLS chain fallback (`Services/RequestServer`). See [Extensions README](Libraries/LibWebView/Extensions/README.md).
6. **Isolated Tab Containers (`ContainerStore`, `SessionStore` v13)**: Persistent containers (`personal`, `work`, `banking`, `shopping`, `agent`, custom) isolating `BrowsingSession`, SQLite storage/cookies, `FileDownloader` requests, `RequestServer` site caches, `BroadcastChannel`/`SharedWorker` workers, and Qt tab badges (`open_tab` defaults to `"agent"`).
7. **Windows Real-Time WebRTC, `TabRecorder` & Modern Web Compat**: WMF camera sharing + H.264/VP8 TX/RX, WASAPI capture/playback recovery, segmented `MP4` tab recording with `SpeakerTimeline`, persisted User-Agent presets in `about:settings` (including `Chrome Windows Desktop`), `Location.ancestorOrigins`, `StorageManager.persist/persisted`, `CredentialsContainer` / `PublicKeyCredential` availability, `Navigator` protocol handlers, and speculative parser CSP nonce/integrity propagation.

Full architecture manual: [Documentation/ForkArchitectureAndCapabilities.md](Documentation/ForkArchitectureAndCapabilities.md).

## Quickstart & CLI Flags

```powershell
# Windows (PowerShell)
.\Build\release\bin\Ladybird.exe --mcp --mcp-token "dev-token"
# Linux / macOS
./Build/release/bin/Ladybird --mcp --mcp-token "dev-token"
# Headless Linux VPS (multi-tab MCP without X11/Xvfb) + SSH tunnel from client
./Ladybird.AppImage --headless=manual --mcp --mcp-token "dev-token"
ssh -N -L 7331:127.0.0.1:7331 <vps-host>
```

Linux Ubuntu 24.04 x86_64 AppImage/DEB builds can be triggered in GitHub Actions via [fork-linux-build.yml](.github/workflows/fork-linux-build.yml) (`workflow_dispatch`).

| Flag | Default | Purpose |
| :--- | :--- | :--- |
| `--mcp [port]` | Disabled (`7331` if port omitted) | Enable embedded loopback `MCPServer` |
| `--mcp-token <token>` | Random 64-hex-char token | Fixed Bearer authentication token |
| `--mcp-upload-dir <dir>` | Browser setting (defaults to `<Videos>/Ladybird Uploads`) | Override Upload Directory and confine `upload_file` to `<dir>` (with symlink checks) |
| `--mcp-allow-extension-install` | `false` | Allow `install_unpacked_extension` over MCP |
| `--mcp-allow-skill-write` | `false` | Allow `save_skill` / `delete_skill` over MCP |
| `--mcp-vault-passphrase <pass>` | Unset (falls back to env / OS key) | Decrypt/encrypt Blind Credential Vault via PBKDF2 |
| `--mcp-allowed-containers <csv>` | Unfiltered (`open_tab` defaults to `agent`) | Restrict MCP access & `open_tab` to listed container IDs |
| `--mcp-allowed-domains <csv>` | Unfiltered | Domain allowlist (`*.domain.com` supported) |
| `--mcp-blocked-domains <csv>` | Unset | Domain denylist (`*.domain.com` supported) |
| `--mcp-allow-eval-js` | `false` (`eval_js` disabled by default) | Re-enable `eval_js` and `call_page_tool` (`--mcp-disable-eval-js` also forces off) |
| `--mcp-disable-screenshots` | `false` | Disable `take_screenshot` and MCP `FlowRunner` `screenshot` steps (DOM-only visual prompt-injection defense) |
| `--mcp-redact-screenshot-media` | `false` | Force `redact_media: true` on `take_screenshot` (hides `<img>`, `<picture>`, `<video>`, `<canvas>`, `<svg>`, CSS background images during capture) |
| `--mcp-auto-approve` | `false` | Auto-approve uploads, blind vault credentials, and consequential `call_page_tool` without Qt dialog prompts (headless CI only) |
| `--mcp-max-tabs <count>` | `16` (`>= 1`) | Cap concurrent MCP-accessible tabs |
| `--user-agent-preset <name>` | Persisted in `Settings.json` | Override UA preset (`Chrome Windows Desktop`, etc.) |

## Build & Verification (Windows Example)

Ensure running `Ladybird`, `WebContent`, `RequestServer`, `ImageDecoder`, `AudioServer`, `WebRTCClient`, `Hub`, and `Compositor` processes are stopped before relinking on Windows:

```powershell
cmd /c "`"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat`" x64 && ninja -C .\Build\release -j 4 ladybird TestLibWebView TestUIQt TestLibWeb test-web"
.\Build\release\bin\TestLibWebView.exe
Push-Location Tests\UI\Qt; ..\..\Build\release\bin\TestUIQt.exe; Pop-Location
Push-Location Tests\LibWeb; ..\..\Build\release\bin\TestLibWeb.exe; Pop-Location
.\Build\release\bin\test-web.exe --results-dir .\Build\test-dumps --test-path .\Tests\LibWeb -f "web-compat-p0-apis"
node --test .\Tools\e2e-ladybird-engine\test\engine-protocol.test.mjs
```

See [Documentation/BuildInstructionsLadybird.md](Documentation/BuildInstructionsLadybird.md) for prerequisite toolchains.

## Upstream Sync Convention

Keep `upstream` pointed to `LadybirdBrowser/ladybird` (with pushes disabled) and `origin` pointed to your private fork:

```bash
git fetch upstream
git checkout master && git merge --ff-only upstream/master
git checkout main && git merge upstream/master
```
