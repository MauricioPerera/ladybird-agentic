/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from '../lib/i18n.js';
import { TRASH_MAX } from '../lib/store.js';
import { el, keepingFocus } from './dom.js';
import { confirmDialog, openModal } from './modal.js';
import { tagInputControl } from './tag-input.js';

export function createRowSelection(onChange) {
  const selected = new Set();
  let lastClickedId = null;

  return {
    has: (id) => selected.has(id),
    count: () => selected.size,
    ids: () => [...selected],
    clear: () => {
      selected.clear();
      lastClickedId = null;
      onChange?.();
    },
    selectAll: (visibleIds) => {
      for (const id of visibleIds) selected.add(id);
      onChange?.();
    },
    prune: (validIds) => {
      const valid = new Set(validIds);
      for (const id of [...selected]) {
        if (!valid.has(id)) selected.delete(id);
      }
    },
    toggle: (id, visibleOrder = [], shiftKey = false) => {
      if (shiftKey && lastClickedId && visibleOrder.includes(lastClickedId) && visibleOrder.includes(id)) {
        const a = visibleOrder.indexOf(lastClickedId);
        const b = visibleOrder.indexOf(id);
        const [start, end] = a < b ? [a, b] : [b, a];
        for (let i = start; i <= end; i++) selected.add(visibleOrder[i]);
      } else if (selected.has(id)) {
        selected.delete(id);
      } else {
        selected.add(id);
      }
      lastClickedId = id;
      onChange?.();
    },
  };
}

export function selectBox(selection, id, visibleOrder) {
  const checked = selection.has(id);
  return el('input', {
    type: 'checkbox',
    checked,
    'data-focus-key': `select-${id}`,
    'aria-label': t('Select row'),
    onclick: (e) => {
      e.stopPropagation();
      keepingFocus(() => selection.toggle(id, visibleOrder, e.shiftKey));
    },
  });
}

export function selectableRow(selection, id, visibleOrder, rowNode) {
  return el('div.select-row', {}, selectBox(selection, id, visibleOrder), rowNode);
}

export function bulkBar(selection, visibleIds, actions = []) {
  const count = selection.count();
  if (count === 0) return null;
  return el(
    'div.bulk-bar',
    { role: 'toolbar', 'aria-label': 'Bulk actions' },
    el(
      'div.chip-row',
      {},
      el('strong', {}, t('{count} selected', { count })),
      visibleIds.length > count
        ? el('button.btn.btn-sm', { type: 'button', onclick: () => selection.selectAll(visibleIds) }, t('Select all {count}', { count: visibleIds.length }))
        : null,
      el('button.btn.btn-sm.btn-ghost', { type: 'button', onclick: () => selection.clear() }, t('Clear'))
    ),
    el('div.chip-row', {}, ...actions)
  );
}

export function pickSheet(title, options, onPick) {
  const dlg = openModal({
    title,
    focusFirst: true,
    body: el(
      'div.row-list',
      {},
      ...options.map((opt) =>
        el(
          'button.list-row',
          {
            type: 'button',
            style: 'cursor:pointer;text-align:left',
            onclick: () => {
              dlg.close();
              onPick(opt.id);
            },
          },
          el('span.row-title', {}, opt.label),
          opt.meta ? el('span.row-meta', {}, opt.meta) : null
        )
      )
    ),
  });
}

export function openBulkTagSheet(store, kind, ids, onDone) {
  const ctrl = tagInputControl([], store.allTags());
  const dlg = openModal({
    title: t('Tag {count} item(s)', { count: ids.length }),
    focusFirst: true,
    body: el(
      'div',
      { style: 'display:flex;flex-direction:column;gap:10px' },
      el('p.muted', { style: 'margin:0' }, t('Add one or more tags to apply to the selected records.')),
      ctrl.el
    ),
    foot: el(
      'div.btn-group',
      { style: 'display:flex;gap:8px' },
      el('button.btn', { type: 'button', onclick: () => dlg.close() }, t('Cancel')),
      el(
        'button.btn.btn-primary',
        {
          type: 'button',
          onclick: async () => {
            for (const tag of ctrl.getTags()) {
              await store.bulkTag(kind, ids, tag, true);
            }
            dlg.close();
            onDone?.();
          },
        },
        t('Apply tags')
      )
    ),
  });
}

export async function confirmBulkDelete(count, label = 'records') {
  const extra = count > TRASH_MAX ? t(' Only the newest {max} fit in 30-day Trash; older ones will be permanently removed.', { max: TRASH_MAX }) : '';
  return confirmDialog(t('Delete {count} {label}?{extra}', { count, label: t(label), extra }), {
    title: t('Delete {count} {label}', { count, label: t(label) }),
    confirmLabel: t('Delete {count}', { count }),
    tone: 'danger',
  });
}
