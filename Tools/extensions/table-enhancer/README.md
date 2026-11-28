# Table Filter & Sort Enhancer Extension

Automatically upgrades semantic HTML data `<table>` elements (`<thead>` or `<th>` header rows, 2+ body rows, and 2+ columns) with:
- Live substring and numeric comparator (`>`, `>=`, `<`, `<=`, `=`) row filtering with visible/total row counter.
- Clickable column headers with automatic numeric/date/text sorting (`▲` / `▼`).
- 1-click RFC-4180 **Export CSV** for currently filtered rows.

Tables without a semantic header row (either a `<thead>` first row with at least 2 non-empty cells — including `<td>` cells — or a first row where at least 2 cells and at least half of the cells are non-empty `<th>` elements), ARIA layout tables (`role="grid"`, `role="treegrid"`, `role="presentation"`), and webmail/app domains (`mail.google.com`, `studio.youtube.com`, `outlook.live.com`, etc. via `exclude_matches`) are skipped automatically.

Tip: Edit `matches` or `exclude_matches` in `manifest.json` if you want to scope it further to specific sites or internal web apps.
