/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { dealContactIds } from '../lib/deal-contacts.js';
import { stageLabel, t } from '../lib/i18n.js';
import { formatDate, formatMoney, matchScore } from '../lib/util.js';
import { VIEW_KINDS } from '../lib/views.js';
import { el } from './dom.js';
import { icon } from './icons.js';
import { hasOpenModal, openModal } from './modal.js';

const MAX_PER_GROUP = 5;
const MAX_RECENT = 6;

export function mountPalette(store, actions) {
  let activeModal = null;

  const openPalette = () => {
    if (activeModal) {
      activeModal.close();
      activeModal = null;
      return;
    }

    const input = el('input.palette-input', {
      type: 'text',
      placeholder: t('Search leads, companies, deals, views, follow-ups, or type an action…'),
      role: 'combobox',
      'aria-expanded': 'true',
      'aria-autocomplete': 'list',
      'aria-controls': 'palette-results',
    });
    const resultsEl = el('div.palette-results', { id: 'palette-results', role: 'listbox' });

    let items = [];
    let selectedIndex = 0;
    let pointerAt = '';
    let keyboardMoved = false;

    const quickActions = [
      { id: 'act-deal', title: t('New deal'), meta: t('Action'), run: () => actions.newDeal() },
      { id: 'act-lead', title: t('New lead'), meta: t('Action'), run: () => actions.newLead() },
      { id: 'act-act', title: t('Log activity'), meta: t('Action'), run: () => actions.newActivity() },
      { id: 'act-task', title: t('New follow-up'), meta: t('Action'), run: () => actions.newTask() },
      { id: 'go-pipe', title: t('Go to Pipeline'), meta: t('Route'), run: () => (window.location.hash = '#/pipeline') },
      { id: 'go-fc', title: t('Go to Forecast'), meta: t('Route'), run: () => (window.location.hash = '#/forecast') },
      { id: 'go-fu', title: t('Go to Follow-ups'), meta: t('Route'), run: () => (window.location.hash = '#/followups') },
      { id: 'go-set', title: t('Go to Settings'), meta: t('Route'), run: () => (window.location.hash = '#/settings') },
    ];

    const buildGroups = (query) => {
      const q = query.trim();
      const groups = [];
      const stages = store.resolvedStages();
      if (!q) {
        groups.push({ label: t('Actions'), items: quickActions.slice(0, 4) });
        const recentDeals = [...store.deals].sort((a, b) => (b.updatedAt || 0) - (a.updatedAt || 0)).slice(0, 3);
        const recentContacts = [...store.contacts].sort((a, b) => (b.createdAt || 0) - (a.createdAt || 0)).slice(0, 3);
        const recents = [
          ...recentDeals.map((d) => ({
            id: `d-${d.id}`,
            title: d.name,
            meta: `${formatMoney(d.value, store.settings.currency)} · ${t('Deal')}`,
            run: () => (window.location.hash = `#/pipeline?id=${d.id}`),
          })),
          ...recentContacts.map((c) => ({
            id: `c-${c.id}`,
            title: c.name,
            meta: `${store.company(c.companyId)?.name || c.email || t('Lead')}`,
            run: () => (window.location.hash = `#/leads?id=${c.id}`),
          })),
        ].slice(0, MAX_RECENT);
        if (recents.length) groups.push({ label: t('Recent'), items: recents });
        return groups;
      }

      const rankGroup = (label, list, toItem) => {
        const hits = [];
        for (const rec of list) {
          const mapped = toItem(rec);
          const best = Math.max(...mapped.searchText.map((s) => matchScore(s, q)));
          if (best >= 0) hits.push({ ...mapped, score: best });
        }
        hits.sort((a, b) => b.score - a.score || a.title.localeCompare(b.title));
        if (hits.length) groups.push({ label, items: hits.slice(0, MAX_PER_GROUP) });
      };

      rankGroup(t('Leads'), store.contacts, (c) => ({
        id: `c-${c.id}`,
        title: c.name,
        meta: store.company(c.companyId)?.name || c.email || c.status,
        searchText: [c.name, c.email, c.phone, c.title, store.company(c.companyId)?.name || ''],
        run: () => (window.location.hash = `#/leads?id=${c.id}`),
      }));

      rankGroup(t('Companies'), store.companies, (c) => ({
        id: `co-${c.id}`,
        title: c.name,
        meta: c.industry || c.website || t('Company'),
        searchText: [c.name, c.industry, c.website],
        run: () => (window.location.hash = `#/companies?id=${c.id}`),
      }));

      rankGroup(t('Deals'), store.deals, (d) => ({
        id: `d-${d.id}`,
        title: d.name,
        meta: `${formatMoney(d.value, store.settings.currency)} · ${stageLabel(d.stage, stages)}`,
        searchText: [
          d.name,
          store.company(d.companyId)?.name || '',
          ...dealContactIds(d).map((cid) => store.contact(cid)?.name || ''),
        ],
        run: () => (window.location.hash = `#/pipeline?id=${d.id}`),
      }));

      const viewEntries = [];
      for (const kind of VIEW_KINDS) {
        const route = kind === 'contacts' ? 'leads' : kind === 'deals' ? 'pipeline' : 'companies';
        for (const v of store.settings.views?.[kind] || []) {
          viewEntries.push({
            id: `v-${kind}-${v.id}`,
            title: v.name,
            meta: t('{kind} view', { kind: t(kind === 'contacts' ? 'Leads' : kind === 'deals' ? 'Deals' : 'Companies') }),
            searchText: [v.name, kind],
            run: () => (window.location.hash = `#/${route}?view=${v.id}`),
          });
        }
      }
      rankGroup(t('Saved views'), viewEntries, (v) => v);

      rankGroup(t('Follow-ups'), store.tasks, (tsk) => ({
        id: `t-${tsk.id}`,
        title: tsk.title,
        meta: formatDate(tsk.due),
        searchText: [tsk.title, store.contact(tsk.contactId)?.name || '', store.deal(tsk.dealId)?.name || ''],
        run: () => (window.location.hash = `#/followups?task=${tsk.id}`),
      }));

      rankGroup(t('Activity'), store.activities, (a) => ({
        id: `a-${a.id}`,
        title: a.summary || t('(Activity)'),
        meta: `${t(a.type)} · ${formatDate(a.date)}`,
        searchText: [a.summary, a.notes],
        run: () => (window.location.hash = '#/activity'),
      }));

      const matchedActions = quickActions
        .map((a) => ({ ...a, score: matchScore(a.title, q) }))
        .filter((a) => a.score >= 0)
        .sort((a, b) => b.score - a.score)
        .slice(0, 4);
      if (matchedActions.length) groups.push({ label: t('Actions'), items: matchedActions });

      return groups;
    };

    const choose = (item) => {
      if (!item) return;
      activeModal?.close();
      activeModal = null;
      item.run();
    };

    const renderList = () => {
      const groups = buildGroups(input.value);
      items = [];
      const nodes = [];
      for (const g of groups) {
        const grpEl = el('div', { role: 'group' }, el('div.palette-group-title', {}, g.label));
        for (const item of g.items) {
          const idx = items.length;
          items.push(item);
          const optId = `pal-opt-${idx}`;
          const opt = el(
            'div.palette-item',
            {
              id: optId,
              role: 'option',
              tabindex: '-1',
              'aria-selected': String(idx === selectedIndex),
              onmousemove: (e) => {
                const pos = `${e.clientX},${e.clientY}`;
                if (keyboardMoved && pos === pointerAt) return;
                pointerAt = pos;
                keyboardMoved = false;
                if (selectedIndex !== idx) {
                  selectedIndex = idx;
                  syncSelection();
                }
              },
              onclick: () => choose(item),
            },
            el('span.row-title', {}, item.title),
            el('span.row-meta', {}, item.meta || '')
          );
          grpEl.appendChild(opt);
        }
        nodes.push(grpEl);
      }
      if (items.length === 0) {
        nodes.push(el('div.empty-state', { style: 'padding:24px' }, t('No matches found.')));
      }
      resultsEl.replaceChildren(...nodes);
      if (selectedIndex >= items.length) selectedIndex = Math.max(0, items.length - 1);
      syncSelection();
    };

    const syncSelection = () => {
      const opts = resultsEl.querySelectorAll('.palette-item');
      opts.forEach((o, i) => o.setAttribute('aria-selected', String(i === selectedIndex)));
      const active = opts[selectedIndex];
      if (active) {
        input.setAttribute('aria-activedescendant', active.id);
        active.scrollIntoView({ block: 'nearest' });
      } else {
        input.removeAttribute('aria-activedescendant');
      }
    };

    input.addEventListener('input', () => {
      selectedIndex = 0;
      renderList();
    });

    input.addEventListener('keydown', (e) => {
      if (e.isComposing) return;
      if (e.key === 'ArrowDown' && items.length) {
        e.preventDefault();
        keyboardMoved = true;
        selectedIndex = (selectedIndex + 1) % items.length;
        syncSelection();
      } else if (e.key === 'ArrowUp' && items.length) {
        e.preventDefault();
        keyboardMoved = true;
        selectedIndex = (selectedIndex - 1 + items.length) % items.length;
        syncSelection();
      } else if (e.key === 'Home' && !input.value && items.length) {
        e.preventDefault();
        selectedIndex = 0;
        syncSelection();
      } else if (e.key === 'End' && !input.value && items.length) {
        e.preventDefault();
        selectedIndex = items.length - 1;
        syncSelection();
      } else if (e.key === 'Enter' && items.length) {
        e.preventDefault();
        choose(items[selectedIndex]);
      }
    });

    const box = el(
      'div.palette-box',
      {},
      el('div.palette-input-wrap', {}, el('span', {}, icon('search', 16)), input),
      resultsEl
    );
    activeModal = openModal({
      title: t('Search & Quick Actions'),
      wide: true,
      focusFirst: true,
      body: box,
      onClose: () => {
        activeModal = null;
      },
    });
    renderList();
    requestAnimationFrame(() => input.focus());
  };

  window.addEventListener('crm:search', openPalette);
  document.addEventListener('keydown', (e) => {
    const isModK = (e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'k';
    if (isModK) {
      if (hasOpenModal() && !activeModal) return;
      e.preventDefault();
      openPalette();
      return;
    }
    if (e.key === '/' && !e.metaKey && !e.ctrlKey && !e.altKey) {
      const tgt = e.target;
      const inField = tgt && (tgt.tagName === 'INPUT' || tgt.tagName === 'TEXTAREA' || tgt.tagName === 'SELECT' || tgt.isContentEditable);
      if (inField || hasOpenModal()) return;
      e.preventDefault();
      openPalette();
    }
  });
}
