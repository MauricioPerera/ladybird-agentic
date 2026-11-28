# Sales CRM (`Examples/SalesCRMApp`)

Full-featured, zero-dependency, `BSD-2-Clause` founder Sales CRM running natively on Ladybird App Mode (`AppStore.db` sharded KV + binary `/__ladybird_fs` + `--account` isolation + 3-tier `--webmcp-only` privacy boundary).

## Key Capabilities

- **Pipeline & Buying Committees**: Kanban drag-and-drop + accessible move sheet, sortable multi-select table, up to 8 customizable open stages + pinned `Won`/`Lost`, up to 8 buying-committee contacts per deal with 5 roles, `stageHistory` velocity with 30-second accidental flip undo, and won/lost close reasons.
- **Recurring Revenue & Forecast**: `one-time`, `monthly`, and `annual` billing (`term` 1–120 months), automatic MRR/ARR calculation, `billingSpread` prorated across months, per-deal confidence overrides, 6-month forecast, cohort conversion funnel, 12-month pipeline flows/win-rate/cycle trends, and going-cold/stalled/no-next-step attention queues.
- **Automation, Calendars & I/O**: Follow-up List/Week/Month calendar with drag/tap rescheduling, repeating tasks (`spawnedId`), playbooks (stage-triggered confirmation previews), saved views (with broken-filter tolerance), `Cmd/Ctrl+K` & `/` ARIA command palette, formula-safe CSV import/export, RFC 5545 `.ics` export, duplicate detection & merge, custom fields (5 types + retype guard), email templates, jailed binary attachments (`<= 25 MiB`), 30-day Trash (`200` cap), and rolling FS backups (`<= 128 MiB`).
- **Sharded Storage**: Splits large entity collections into `< 3 MiB` `AppStore.db` KV shards (`crm:<kind>:s:<NN>`) and writes full JSON snapshots/backups via `/__ladybird_fs` binary `PUT`/`GET` (`128 MiB` ceiling).

## Quick Start

```bash
# Interactive GUI window (isolated per account)
python ../../Meta/ladybird-app.py run . --account personal

# Headless strict privacy mode (WebMCP aggregates & opaque-ID actions only)
python ../../Meta/ladybird-app.py connect . --account personal --headless --webmcp-only --json
```

## WebMCP Tools (3-Tier Privacy Contract)

- **Tier A (Always active, zero PII)**: `get_crm_status`, `get_pipeline_summary`, `get_revenue_forecast`, `get_conversion_funnel`, `get_pipeline_trends`, `get_attention_summary`, `list_playbooks_and_views`.
- **Tier B (Always active, opaque IDs, zero PII echo)**: `move_deal_stage`, `apply_playbook`, `complete_followup`, `log_structured_touch`, `create_backup`, `export_ics_summary`.
- **Tier C (Full mode only; automatically disabled when `--webmcp-only`)**: `search_crm`, `get_record_detail`, `upsert_record`, `parse_contact_or_company_text`.
