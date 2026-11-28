# Bash Workbench (`Examples/BashWorkbenchApp`)

Sandboxed UNIX shell and file-analysis workbench running [`vercel-labs/just-bash`](https://github.com/vercel-labs/just-bash) (`just-bash/browser`, bundled in [`vendor/just-bash.mjs`](vendor/just-bash.mjs)) inside Ladybird App Mode.

## Features

- **In-browser UNIX sandbox**: Runs `jq`, `awk`, `sed`, `grep`, `rg`, `xan`, `yq`, `sqlite3` inside LibJS without spawning OS shells.
- **Bidirectional `/workspace` disk sync**: Hydrates jailed files from `window.ladybirdApp.fs` at boot and syncs workspace writes/deletes back to per-account disk storage after each command.
- **Built-in Ladybird commands**:
  - `kv get|set|delete|list|clear` backed by per-account SQLite (`AppStore.db`)
  - `accounts current|list|create|launch`
  - `sysinfo`
- **WebMCP tools**: Exposes `run_bash` and `get_workbench_info` to external MCP agents.

## Quick Start

```bash
# Interactive GUI window (auto-starts MCP and binds WebMCP)
python ../../Meta/ladybird-app.py run . --account personal

# Headless on-demand with privacy boundary (--webmcp-only)
python ../../Meta/ladybird-app.py connect . --account personal --headless --webmcp-only --json

# Promote the background session to an interactive GUI window
python ../../Meta/ladybird-app.py connect . --account personal --gui

# Stop the instance cleanly
python ../../Meta/ladybird-app.py stop . --account personal
```
