# Ladybird Engine Protocol v1 Adapter (`e2e`)

Connects the [`e2e`](https://github.com/e2e-inc/e2e) agentic testing framework to a running Ladybird instance through Ladybird's embedded MCP server (`LibMCP`).

## 1. Start Ladybird with MCP enabled

```powershell
./Build/release/bin/Ladybird --mcp --mcp-allow-eval-js --mcp-token "my-local-dev-token"
```

Default MCP endpoint when port is omitted: `http://127.0.0.1:7331/mcp`. Note that `eval_js` starts **disabled by default** in Ladybird for prompt-injection hardening; `--mcp-allow-eval-js` is required because this adapter currently uses `eval_js` for `data-e2e-ref` stamping and DOM/action helpers.

## 2. Configure `e2e.config.ts`

```ts
import type { E2EConfig } from 'e2e';
import { connectEngine, spawnTransport } from 'e2e/engine';
import { createLadybirdEngine } from './Tools/e2e-ladybird-engine/index.mjs';

export default {
  targets: [
    {
      name: 'ladybird-desktop',
      app: { url: 'http://localhost:3000' },
      engine: createLadybirdEngine({
        connectEngine,
        spawnTransport,
        mcpUrl: 'http://127.0.0.1:7331/mcp',
        mcpToken: process.env.LADYBIRD_MCP_TOKEN,
      }),
    },
  ],
} satisfies E2EConfig;
```

## 3. Running Adapter Tests

```powershell
node --test Tools/e2e-ladybird-engine/test/engine-protocol.test.mjs
```

The protocol & mocked-MCP unit test runs out of the box; the second optional integration test activates when `E2E_REPO_PATH` points to a built `e2e` checkout containing its compiled client/transport bundles, exercising the real `e2e` client/transport against the adapter's mocked MCP server (`startMockMcpServer()`) rather than a live Ladybird browser process.

Alternatively, without `e2e`, open `about:flows` directly inside Ladybird or call Ladybird's native MCP flow tools (`list_flows`, `save_flow`, `run_flow`, `list_flow_runs`, `get_flow_run`).
