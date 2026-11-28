/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { formatFieldValue, normalizeCustomValues } from '../lib/fields.js';
import { t } from '../lib/i18n.js';
import { el } from './dom.js';

let cfSeq = 0;

export function customFieldsInputs(fields, existingValues = {}) {
  if (!fields || fields.length === 0) return { nodes: [], readValues: () => ({ ...existingValues }) };
  const getters = [];
  const nodes = fields.map((f) => {
    const val = existingValues?.[f.id];
    const cid = `crm-cf-${f.id}-${++cfSeq}`;
    let control;
    if (f.type === 'select') {
      const opts = [...(f.options || [])];
      if (val && !opts.includes(String(val))) opts.push(String(val));
      control = el(
        'select.select',
        { id: cid, name: `custom_${f.id}` },
        el('option', { value: '' }, '—'),
        ...opts.map((o) => el('option', { value: o, selected: String(val ?? '') === o }, o))
      );
      getters.push(() => [f.id, control.value]);
    } else if (f.type === 'checkbox') {
      control = el('input', { id: cid, type: 'checkbox', name: `custom_${f.id}`, checked: Boolean(val) });
      getters.push(() => [f.id, control.checked ? true : null]);
    } else if (f.type === 'number') {
      control = el('input.input', { id: cid, type: 'number', step: 'any', name: `custom_${f.id}`, value: val ?? '' });
      getters.push(() => [f.id, control.value === '' ? null : Number(control.value)]);
    } else if (f.type === 'date') {
      control = el('input.input', { id: cid, type: 'date', name: `custom_${f.id}`, value: val ?? '' });
      getters.push(() => [f.id, control.value]);
    } else {
      control = el('input.input', { id: cid, type: 'text', name: `custom_${f.id}`, value: val ?? '' });
      getters.push(() => [f.id, control.value]);
    }
    return el('div.field', {}, el('label.field-label', { for: cid }, f.name), control);
  });

  const shownIds = new Set(fields.map((f) => f.id));
  const readValues = () => {
    const next = {};
    for (const [k, v] of Object.entries(existingValues || {})) {
      if (!shownIds.has(k)) next[k] = v;
    }
    for (const get of getters) {
      const [id, v] = get();
      if (v !== null && v !== undefined && v !== '') next[id] = v;
    }
    return normalizeCustomValues(next);
  };

  return { nodes, readValues };
}

export function customFieldsDetail(fields, values = {}) {
  const populated = (fields || []).filter((f) => values?.[f.id] !== undefined && values?.[f.id] !== null && values?.[f.id] !== '');
  if (!populated.length) return null;
  return el(
    'div.card',
    {},
    el('div.card-head', {}, el('h3.card-title', {}, t('Details'))),
    el(
      'dl',
      { style: 'display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;margin:0' },
      ...populated.map((f) =>
        el(
          'div',
          {},
          el('dt.row-meta', {}, f.name),
          el('dd', { style: 'margin:2px 0 0;font-weight:500' }, formatFieldValue(f, values[f.id]))
        )
      )
    )
  );
}

export function customFilterControls(fields, activeFields = {}, onChange) {
  const filterable = (fields || []).filter((f) => f.type === 'select' || f.type === 'checkbox');
  if (!filterable.length) return null;
  return el(
    'div.chip-row',
    {},
    ...filterable.map((f) => {
      const cur = activeFields?.[f.id];
      const options = f.type === 'checkbox' ? ['Yes', 'No'] : f.options || [];
      const sel = el(
        'select.select.select-sm',
        {
          'data-focus-key': `cf-${f.id}`,
          'aria-label': t('Filter {name}', { name: f.name }),
          onchange: () => {
            const next = { ...activeFields };
            if (!sel.value) delete next[f.id];
            else next[f.id] = f.type === 'checkbox' ? sel.value === 'Yes' : sel.value;
            onChange(next);
          },
        },
        el('option', { value: '' }, t('{name}: Any', { name: f.name })),
        ...options.map((o) => {
          const isSel = f.type === 'checkbox' ? cur === (o === 'Yes') : String(cur ?? '') === o;
          return el('option', { value: o, selected: isSel }, f.type === 'checkbox' ? t(o) : o);
        })
      );
      return sel;
    })
  );
}
