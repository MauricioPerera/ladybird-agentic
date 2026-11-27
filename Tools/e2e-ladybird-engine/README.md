# Ladybird Engine Protocol v1 Adapter (`e2e`)

Connects the [`e2e`](https://github.com/e2e-inc/e2e) agentic testing framework to a running Ladybird instance through Ladybird's embedded MCP server (`LibMCP`).

## 1. Start Ladybird with MCP enabled

```powershell
./Build/release/bin/Ladybird --mcp --mcp-token "my-local-dev-token"
```

Default MCP endpoint when port is omitted: `http://127.0.0.1:7331/mcp`.

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

Alternatively, without `e2e`, open `about:flows` directly inside Ladybird or call Ladybird's native MCP flow tools (`list_flows`, `save_flow`, `run_flow`, `list_flow_runs`, `get_flow_run`).

## 3. Verify the adapter

Use Node 24.16.0. From the repository root, install the locked `e2e` 0.24.0 package and run the two protocol cases and their regression checks.

```sh
npm ci --prefix Tools/e2e-ladybird-engine --ignore-scripts --engine-strict
node --test Tools/e2e-ladybird-engine/test/engine-protocol.test.mjs
node --test Tools/e2e-ladybird-engine/test/pinned-engine-oracle.test.mjs
```

The cases use a synthetic MCP server on loopback. The integration case imports the installed `e2e/engine` entry point. A missing package fails the run. See the [KDD contract](../../knowledge/contracts/ladybird-e2e-adapter-oracle.md) for the required evidence.
