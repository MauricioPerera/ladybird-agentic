# Ladybird Example Extension (`Page Inspector`)

Unpacked MV3 + Ladybird Flows reference extension:

- Injects `content.css` + `content.js` on `http://*/*` and `https://*/*` pages.
- Reads configurable `{ "labelPrefix": "Inspector" }` from isolated extension storage (`ladybirdExt.storage.local`).
- Registers `ext-page-inspector-smoke` automatically in `about:flows`.

## Load in UI

1. Open `about:extensions` (or `Inspect -> Extensions`).
2. Enter `D:\ladybird\Tools\example-extension` and click **Load Unpacked**.

## Load via MCP (`install_unpacked_extension`)

> **Prerequisites:** `install_unpacked_extension` is disabled by default over MCP. Launch Ladybird with `--mcp-allow-extension-install` and approve the native Qt confirmation prompt (or run in `--headless` mode with `--mcp-auto-approve`). See [Extensions Security Boundaries](../../Libraries/LibWebView/Extensions/README.md#4-security-boundaries-and-known-caveats).

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "install_unpacked_extension",
    "arguments": {
      "path": "D:\\ladybird\\Tools\\example-extension"
    }
  }
}
```
