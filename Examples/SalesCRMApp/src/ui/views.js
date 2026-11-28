/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from '../lib/i18n.js';
import {
  applyFilters,
  DEAL_SORT_COLUMNS,
  defaultView,
  hasActiveFilters,
  matchingView,
  MAX_VIEW_NAME,
  MAX_VIEWS,
} from '../lib/views.js';
import { el } from './dom.js';
import { icon } from './icons.js';
import { confirmDialog, openModal } from './modal.js';
import { toast } from './toast.js';

export function useView(kind, rawFilters, store) {
  const ctx = {
    tags: store.allTags(),
    fields: store.customFieldsFor(kind),
    stages: store.resolvedStages(),
    sortKeys: DEAL_SORT_COLUMNS,
  };
  const { filters, dropped } = applyFilters(kind, rawFilters, ctx);
  if (dropped.length > 0) {
    toast(t('Ignored missing view filter(s): {items}', { items: dropped.join(', ') }), { tone: 'warn', duration: 7000 });
  }
  return filters;
}

export function initialViewFilters(kind, store, queryViewId = null) {
  const list = store.settings.views?.[kind] || [];
  const chosen = (queryViewId && list.find((v) => v.id === queryViewId)) || defaultView(store.settings.views, kind);
  return chosen ? useView(kind, chosen.filters, store) : null;
}

export function viewChips(kind, store, currentFilters, onApply) {
  const list = store.settings.views?.[kind] || [];
  const active = matchingView(store.settings.views, kind, currentFilters);
  const canSave = hasActiveFilters(kind, currentFilters) && !active && list.length < MAX_VIEWS;

  const openChipMenu = (view, idx) => {
    const dlg = openModal({
      title: view.name,
      focusFirst: true,
      body: el(
        'div.row-list',
        {},
        el('button.list-row', {
          type: 'button',
          style: 'cursor:pointer',
          onclick: async () => {
            dlg.close();
            const next = list.map((v) => ({ ...v, default: v.id === view.id ? !v.default : false }));
            await store.setViews(kind, next);
          },
        }, el('span.row-title', {}, view.default ? t('Unset default view') : t('Set as default view'))),
        el('button.list-row', {
          type: 'button',
          style: 'cursor:pointer',
          onclick: () => {
            dlg.close();
            const inp = el('input.input', { type: 'text', maxlength: MAX_VIEW_NAME, value: view.name });
            const rDlg = openModal({
              title: t('Rename view'),
              focusFirst: true,
              body: inp,
              foot: el(
                'div.btn-group',
                { style: 'display:flex;gap:8px' },
                el('button.btn', { type: 'button', onclick: () => rDlg.close() }, t('Cancel')),
                el('button.btn.btn-primary', {
                  type: 'button',
                  onclick: async () => {
                    const name = inp.value.trim().slice(0, MAX_VIEW_NAME);
                    if (!name) {
                      toast(t('Enter a view name'), { tone: 'danger' });
                      inp.focus();
                      return;
                    }
                    await store.setViews(
                      kind,
                      list.map((v) => (v.id === view.id ? { ...v, name } : v))
                    );
                    rDlg.close();
                  },
                }, t('Save'))
              ),
            });
          },
        }, el('span.row-title', {}, t('Rename'))),
        idx > 0
          ? el('button.list-row', {
              type: 'button',
              style: 'cursor:pointer',
              onclick: async () => {
                dlg.close();
                const next = [...list];
                [next[idx - 1], next[idx]] = [next[idx], next[idx - 1]];
                await store.setViews(kind, next);
              },
            }, el('span.row-title', {}, t('Move left')))
          : null,
        idx < list.length - 1
          ? el('button.list-row', {
              type: 'button',
              style: 'cursor:pointer',
              onclick: async () => {
                dlg.close();
                const next = [...list];
                [next[idx + 1], next[idx]] = [next[idx], next[idx + 1]];
                await store.setViews(kind, next);
              },
            }, el('span.row-title', {}, t('Move right')))
          : null,
        el('button.list-row', {
          type: 'button',
          style: 'cursor:pointer;color:var(--critical-ink)',
          onclick: async () => {
            dlg.close();
            if (!(await confirmDialog(t('Delete saved view "{name}"?', { name: view.name }), { confirmLabel: t('Delete'), tone: 'danger' }))) return;
            await store.setViews(
              kind,
              list.filter((v) => v.id !== view.id)
            );
          },
        }, el('span.row-title', {}, t('Delete view')))
      ),
    });
  };

  if (list.length === 0 && !canSave) return null;

  return el(
    'div.chip-row',
    {},
    ...list.map((v, idx) =>
      el(
        'div',
        { style: 'display:inline-flex;align-items:center;gap:2px' },
        el(
          'button',
          {
            type: 'button',
            class: `filter-chip${active?.id === v.id ? ' active' : ''}`,
            onclick: () => onApply(useView(kind, v.filters, store)),
          },
          `${v.name}${v.default ? ' ★' : ''}`
        ),
        el(
          'button.icon-btn',
          {
            type: 'button',
            style: 'width:24px;height:24px',
            'aria-label': t('View options for {name}', { name: v.name }),
            onclick: () => openChipMenu(v, idx),
          },
          icon('more', 13)
        )
      )
    ),
    canSave
      ? el(
          'button.btn.btn-sm',
          {
            type: 'button',
            onclick: () => {
              const inp = el('input.input', { type: 'text', maxlength: MAX_VIEW_NAME, placeholder: t('View name (e.g. Q4 renewals)') });
              const sDlg = openModal({
                title: t('Save current view'),
                focusFirst: true,
                body: inp,
                foot: el(
                  'div.btn-group',
                  { style: 'display:flex;gap:8px' },
                  el('button.btn', { type: 'button', onclick: () => sDlg.close() }, t('Cancel')),
                  el('button.btn.btn-primary', {
                    type: 'button',
                    onclick: async () => {
                      const name = inp.value.trim().slice(0, MAX_VIEW_NAME);
                      if (!name) {
                        toast(t('Enter a view name'), { tone: 'danger' });
                        inp.focus();
                        return;
                      }
                      await store.setViews(kind, [...list, { name, default: false, filters: currentFilters }]);
                      sDlg.close();
                      toast(t('Saved view "{name}"', { name }));
                    },
                  }, t('Save'))
                ),
              });
            },
          },
          t('+ Save view')
        )
      : null
  );
}
