# Table Filter & Sort Enhancer Extension

Automatically upgrades semantic HTML data `<table>` elements (`<thead>` or `<th>` header rows, 2+ body rows, and 2+ columns) with:
- Live substring row filtering and visible/total row counter.
- Clickable column headers with automatic numeric/date/text sorting (`▲` / `▼`).
- 1-click RFC-4180 **Export CSV** for currently filtered rows.

Layout/ARIA tables (`role="grid"`, `role="presentation"`, or tables without `<th>` headers) and webmail/app domains (`mail.google.com`, `studio.youtube.com`, `outlook.live.com`, etc. via `exclude_matches`) are skipped automatically.

Tip: Edit `matches` or `exclude_matches` in `manifest.json` if you want to scope it further to specific sites or internal web apps.
