/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { ACTIVITY_WINDOWS, matchesActivityWindow, touchProgress } from '../lib/activity.js';
import { dealContactIds } from '../lib/deal-contacts.js';
import { t } from '../lib/i18n.js';
import { ACTIVITY_TYPES } from '../lib/statuses.js';
import { fuzzyIncludes, todayISO } from '../lib/util.js';
import { activityRow, emptyState, pageHead, statTile } from '../ui/bits.js';
import { el, keepingFocus } from '../ui/dom.js';
import { openActivityForm } from '../ui/forms.js';

const PAGE = 50;

export function activityView(root, { store }) {
  let query = '';
  let windowId = 'all';
  let typeFilter = 'all';
  let linkFilter = '';
  let limit = PAGE;

  const render = () => {
    const today = todayISO();
    const targets = store.settings.touchTargets;
    const progress = touchProgress(store.activities, targets, today);

    const nodes = [
      pageHead(t('Activity'), {
        subtitle: t('{count} logged touchpoint(s)', { count: store.activities.length }),
        actions: [el('button.btn.btn-primary', { type: 'button', onclick: () => openActivityForm(store) }, t('+ Log activity'))],
      }),
    ];

    if (targets && store.activities.length > 0) {
      nodes.push(
        el(
          'div.stat-grid',
          {},
          statTile(t('Touches this week'), `${progress.logged}/${progress.target || 0}`, progress.pace === 'met' ? t('Target met') : progress.pace === 'on-track' ? t('On track') : t('{gap} behind', { gap: progress.gap }), {
            progress: progress.target ? progress.logged / progress.target : 0,
          }),
          ...progress.byType
            .filter((b) => b.target > 0 || b.logged > 0)
            .map((b) => statTile(t(b.label), b.target > 0 ? `${b.logged}/${b.target}` : String(b.logged), t('This week')))
        )
      );
    }

    const linkSel = el('select.select.select-sm', {
      'data-focus-key': 'act-link',
      onchange: (e) => {
        linkFilter = e.target.value;
        limit = PAGE;
        render();
      },
    }, el('option', { value: '' }, t('All records')));

    const addGroup = (label, prefix, list) => {
      if (!list.length) return;
      const grp = el('optgroup', { label });
      for (const item of [...list].sort((a, b) => a.name.localeCompare(b.name))) {
        const val = `${prefix}:${item.id}`;
        grp.appendChild(el('option', { value: val, selected: linkFilter === val }, item.name));
      }
      linkSel.appendChild(grp);
    };
    addGroup(t('Leads'), 'contact', store.contacts);
    addGroup(t('Deals'), 'deal', store.deals);
    addGroup(t('Companies'), 'company', store.companies);

    const toolbar = el(
      'div.filter-toolbar',
      {},
      el(
        'div.filter-line',
        {},
        el('input.input.input-sm', {
          type: 'search',
          placeholder: t('Search summary, notes, records…'),
          value: query,
          'data-focus-key': 'act-search',
          style: 'min-width:210px',
          oninput: (e) => {
            query = e.target.value;
            limit = PAGE;
            keepingFocus(render);
          },
        }),
        el(
          'select.select.select-sm',
          {
            'data-focus-key': 'act-window',
            onchange: (e) => {
              windowId = e.target.value;
              limit = PAGE;
              render();
            },
          },
          ...ACTIVITY_WINDOWS.map((w) => el('option', { value: w.id, selected: windowId === w.id }, t(w.label)))
        ),
        linkSel
      ),
      el(
        'div.chip-row',
        {},
        ...[{ id: 'all', label: 'All' }, ...ACTIVITY_TYPES].map((tp) =>
          el(
            'button',
            {
              type: 'button',
              class: `filter-chip${typeFilter === tp.id ? ' active' : ''}`,
              onclick: () => {
                typeFilter = tp.id;
                limit = PAGE;
                render();
              },
            },
            t(tp.label)
          )
        )
      )
    );
    nodes.push(toolbar);

    const filtered = store.activities
      .filter((a) => {
        if (typeFilter !== 'all' && a.type !== typeFilter) return false;
        if (!matchesActivityWindow(a, windowId, today)) return false;
        if (linkFilter) {
          const [lkKind, lkId] = linkFilter.split(':');
          if (lkKind === 'contact' && a.contactId !== lkId) return false;
          if (lkKind === 'company' && a.companyId !== lkId) return false;
          if (lkKind === 'deal') {
            const deal = store.deal(lkId);
            const cids = new Set(deal ? dealContactIds(deal) : []);
            if (a.dealId !== lkId && !( !a.dealId && a.contactId && cids.has(a.contactId))) return false;
          }
        }
        if (query) {
          const links = [store.contact(a.contactId)?.name, store.deal(a.dealId)?.name, store.company(a.companyId)?.name].filter(Boolean).join(' ');
          if (!fuzzyIncludes(`${a.summary} ${a.notes} ${links}`, query)) return false;
        }
        return true;
      })
      .sort((a, b) => String(b.date).localeCompare(String(a.date)) || b.createdAt - a.createdAt);

    if (filtered.length === 0) {
      nodes.push(emptyState('activity', t('No matching activity')));
    } else {
      const shown = filtered.slice(0, limit);
      const list = el('div.row-list', {}, ...shown.map((a) => activityRow(a, store, (x) => openActivityForm(store, x))));
      nodes.push(list);
      if (filtered.length > shown.length) {
        const rem = filtered.length - shown.length;
        nodes.push(
          el(
            'div',
            { style: 'margin-top:12px;text-align:center' },
            el('button.btn', { type: 'button', onclick: () => { limit += PAGE; render(); } }, t('Show {count} more ({rem} remaining)', { count: Math.min(PAGE, rem), rem }))
          )
        );
      }
    }

    root.replaceChildren(...nodes);
  };

  render();
  const onChange = () => keepingFocus(render);
  store.addEventListener('change', onChange);
  return () => store.removeEventListener('change', onChange);
}
