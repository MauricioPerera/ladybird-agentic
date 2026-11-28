/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { autoMap, exportRecords, importFieldsFor, mapRows, parseCSV, unguardFormula } from '../lib/csv.js';
import { t } from '../lib/i18n.js';
import { downloadTextFile } from './calendar.js';
import { toggleSwitch } from './bits.js';
import { el } from './dom.js';
import { openModal } from './modal.js';
import { toast } from './toast.js';

export function exportSet(kind, store, subset = null) {
  const csv = `\uFEFF${exportRecords(kind, store, subset)}`;
  downloadTextFile(`crm-${kind}.csv`, csv, 'text/csv;charset=utf-8');
  toast(t('Exported crm-{kind}.csv', { kind }));
}

export function openCSVImport(store, defaultKind = 'contacts') {
  let kind = defaultKind;
  let rows = [];
  let headers = [];
  let mapping = {};
  let updateExisting = true;
  let step = 1;

  const body = el('div', { style: 'display:flex;flex-direction:column;gap:12px' });
  const foot = el('div.btn-group', { style: 'display:flex;gap:8px' });
  let dlg = null;

  const render = () => {
    foot.replaceChildren();
    if (step === 1) {
      const kindSel = el(
        'select.select',
        { onchange: (e) => (kind = e.target.value) },
        el('option', { value: 'contacts', selected: kind === 'contacts' }, t('Leads & contacts')),
        el('option', { value: 'companies', selected: kind === 'companies' }, t('Companies')),
        el('option', { value: 'deals', selected: kind === 'deals' }, t('Deals'))
      );
      const fileInp = el('input.input', { id: 'csv-import-file', type: 'file', accept: '.csv,text/csv' });
      const kindSelId = 'csv-import-kind';
      kindSel.id = kindSelId;
      fileInp.addEventListener('change', async () => {
        const file = fileInp.files?.[0];
        if (!file) return;
        try {
          const parsed = parseCSV(await file.text());
          if (parsed.length < 2) {
            toast(t('CSV needs a header row and at least one data row'), { tone: 'warn' });
            return;
          }
          headers = parsed[0];
          rows = parsed.slice(1);
          const fields = importFieldsFor(kind, store.customFieldsFor(kind));
          mapping = autoMap(headers, fields);
          step = 2;
          render();
        } catch (err) {
          toast(err?.message || t('Could not read CSV file'), { tone: 'danger' });
        }
      });
      body.replaceChildren(
        el('div.field', {}, el('label.field-label', { for: kindSelId }, t('Record type')), kindSel),
        el('div.field', {}, el('label.field-label', { for: 'csv-import-file' }, t('Choose CSV file')), fileInp)
      );
      foot.appendChild(el('button.btn', { type: 'button', onclick: () => dlg.close() }, t('Cancel')));
      return;
    }

    if (step === 2) {
      const fields = importFieldsFor(kind, store.customFieldsFor(kind));
      const preview = rows.slice(0, 4);
      const mapRowsUi = fields.map((f) => {
        const sel = el(
          'select.select.select-sm',
          { onchange: (e) => (mapping[f.key] = Number(e.target.value)) },
          el('option', { value: -1 }, t('— Ignore —')),
          ...headers.map((h, i) =>
            el('option', { value: i, selected: mapping[f.key] === i }, unguardFormula(h) || t('Column {num}', { num: i + 1 }))
          )
        );
        return el('div.list-row', {}, el('span.row-title', {}, `${f.field ? f.label : t(f.label)}${f.required ? ' *' : ''}`), sel);
      });

      body.replaceChildren(
        el('div.panel-row', { style: 'display:flex;justify-content:space-between;align-items:center' }, el('span.field-label', {}, t('Update existing matches')), toggleSwitch(updateExisting, (v) => (updateExisting = v))),
        el('div.row-list', {}, ...mapRowsUi),
        el('div.field-label', {}, t('Preview ({count} row(s) total)', { count: rows.length })),
        el(
          'div.data-table-wrap',
          {},
          el(
            'table.data-table',
            {},
            el('thead', {}, el('tr', {}, ...headers.map((h) => el('th', {}, unguardFormula(h))))),
            el('tbody', {}, ...preview.map((r) => el('tr', {}, ...headers.map((_, i) => el('td', {}, r[i] || '')))))
          )
        )
      );
      foot.append(
        el('button.btn', { type: 'button', onclick: () => { step = 1; render(); } }, t('Back')),
        el('button.btn.btn-primary', {
          type: 'button',
          onclick: async () => {
            const missingReq = fields.filter((f) => f.required && !(Number(mapping[f.key]) >= 0));
            if (missingReq.length > 0) {
              toast(t('Map required column(s): {cols}', { cols: missingReq.map((f) => (f.field ? f.label : t(f.label))).join(', ') }), { tone: 'danger' });
              return;
            }
            try {
              await store.saveSnapshot('csv');
              const mapped = mapRows(rows, kind, mapping, {
                stages: store.resolvedStages(),
                customFields: store.customFieldsFor(kind),
              });
              const res = await store.importRecords(kind, mapped.records, { update: updateExisting });
              step = 3;
              body.replaceChildren(
                el('p', {}, t('Added {added}, updated {updated}, skipped {skipped}.', { added: res.added, updated: res.updated, skipped: mapped.skipped })),
                res.companiesCreated ? el('p.muted', {}, t('Created {count} missing company record(s).', { count: res.companiesCreated })) : null
              );
              foot.replaceChildren(
                el('button.btn', {
                  type: 'button',
                  onclick: async () => {
                    try {
                      if (await store.restoreBackup()) toast(t('Import undone'));
                      dlg.close();
                    } catch (err) {
                      toast(err?.message || t('Could not undo import'), { tone: 'danger' });
                    }
                  },
                }, t('Undo import')),
                el('button.btn.btn-primary', { type: 'button', onclick: () => dlg.close() }, t('Done'))
              );
            } catch (err) {
              toast(err?.message || t('CSV import failed'), { tone: 'danger' });
            }
          },
        }, t('Import {count} row(s)', { count: rows.length }))
      );
    }
  };

  dlg = openModal({ title: t('Import CSV'), wide: true, body, foot });
  render();
}
