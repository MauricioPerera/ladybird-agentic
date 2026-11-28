# Agentic Notes (`Examples/AgenticNotesApp`)

Multi-account Markdown notes app demonstrating Ladybird App Mode storage, jailed filesystem I/O, persistent SQLite KV state, multi-window account launching, and WebMCP tool registration.

## Capabilities

- **Jailed filesystem (`notes/`)**: Lists, reads, writes, and deletes `.md`/`.txt` notes isolated per account.
- **Persistent SQLite KV (`AppStore.db`)**: Tracks `meta:visits` and `ui:lastNote` across restarts.
- **Deterministic origin**: Keeps origin-scoped `localStorage` intact across restarts per `--account`.
- **WebMCP tools**: `get_account_info`, `kv_get`, `kv_set`, `kv_list`, `list_notes`, `read_note`, `create_note`, `delete_note`.

## Quick Start

```bash
# Open interactive GUI window for the 'personal' account
python ../../Meta/ladybird-app.py run . --account personal

# Open a separate 'work' account window concurrently
python ../../Meta/ladybird-app.py run . --account work

# List accounts registered for this app
python ../../Meta/ladybird-app.py accounts .
```
