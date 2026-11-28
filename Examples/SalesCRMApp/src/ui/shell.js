/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { getLanguage, t } from '../lib/i18n.js';
import { wipeSampleData } from '../lib/sample.js';
import { todayISO } from '../lib/util.js';
import { activityView } from '../views/activity.js';
import { companiesView } from '../views/companies.js';
import { dashboardView } from '../views/dashboard.js';
import { followupsView } from '../views/followups.js';
import { forecastView } from '../views/forecast.js';
import { leadsView } from '../views/leads.js';
import { pipelineView } from '../views/pipeline.js';
import { settingsView } from '../views/settings.js';
import { avatarEl } from './bits.js';
import { clear, el, keepingFocus } from './dom.js';
import { openActivityForm, openContactForm, openDealForm, openTaskForm } from './forms.js';
import { icon } from './icons.js';
import { closeAllModals, confirmDialog, openModal } from './modal.js';
import { openOnboarding } from './onboarding.js';
import { mountPalette } from './palette.js';
import { toast } from './toast.js';

export const ROUTES = [
  { hash: '#/', label: 'Home', icon: 'home', tab: true, view: dashboardView },
  { hash: '#/pipeline', label: 'Pipeline', icon: 'pipeline', tab: true, view: pipelineView },
  { hash: '#/leads', label: 'Leads', icon: 'users', tab: true, view: leadsView },
  { hash: '#/followups', label: 'Follow-ups', icon: 'checkSquare', tab: true, view: followupsView },
  { hash: '#/companies', label: 'Companies', icon: 'building', tab: false, sub: 'Accounts & linked teams', view: companiesView },
  { hash: '#/activity', label: 'Activity', icon: 'activity', tab: false, sub: 'Meetings, calls, emails & notes', view: activityView },
  { hash: '#/forecast', label: 'Forecast', icon: 'chart', tab: false, sub: 'Weighted revenue, funnel & trends', view: forecastView },
  { hash: '#/settings', label: 'Settings', icon: 'settings', tab: false, sub: 'Stages, playbooks, data & WebMCP', view: settingsView },
];

const SCROLL_MEMORY_MAX = 30;

function parseHash() {
  const raw = window.location.hash || '#/';
  const [pathPart, queryPart] = raw.split('?');
  const path = ROUTES.some((r) => r.hash === pathPart) ? pathPart : '#/';
  const params = new URLSearchParams(queryPart || '');
  return { raw, path, params };
}

export function mountShell(appRoot, store) {
  if ('scrollRestoration' in history) history.scrollRestoration = 'manual';

  const actions = {
    newDeal: () => openDealForm(store),
    newLead: () => openContactForm(store),
    newActivity: () => openActivityForm(store),
    newTask: () => openTaskForm(store),
  };

  const openQuickAdd = () => {
    const dlg = openModal({
      title: t('Quick create'),
      focusFirst: true,
      body: el(
        'div.row-list',
        {},
        el('button.list-row', { type: 'button', style: 'cursor:pointer', onclick: () => { dlg.close(); actions.newDeal(); } }, el('span.row-title', {}, t('New deal'))),
        el('button.list-row', { type: 'button', style: 'cursor:pointer', onclick: () => { dlg.close(); actions.newLead(); } }, el('span.row-title', {}, t('New lead / contact'))),
        el('button.list-row', { type: 'button', style: 'cursor:pointer', onclick: () => { dlg.close(); actions.newActivity(); } }, el('span.row-title', {}, t('Log activity'))),
        el('button.list-row', { type: 'button', style: 'cursor:pointer', onclick: () => { dlg.close(); actions.newTask(); } }, el('span.row-title', {}, t('New follow-up')))
      ),
    });
  };

  const sidebar = el('nav.sidebar', { 'aria-label': 'Primary' });
  const sampleBanner = el('div.sample-banner', { hidden: true });
  const mainView = el('main.main-view', { id: 'main', tabindex: '-1' });
  const tabbar = el('nav.tabbar', { 'aria-label': 'Mobile navigation' });
  const fab = el(
    'button.fab',
    {
      type: 'button',
      'aria-label': t('Quick create'),
      onclick: openQuickAdd,
    },
    icon('plus', 22)
  );

  appRoot.replaceChildren(
    el('div.shell', {}, sidebar, el('div.main-col', {}, sampleBanner, mainView)),
    tabbar,
    fab
  );

  mountPalette(store, actions);

  const badgeStats = () => {
    const today = todayISO();
    let overdue = 0;
    let dueToday = 0;
    for (const tsk of store.openTasks()) {
      if (tsk.due < today) overdue++;
      else if (tsk.due === today) dueToday++;
    }
    return { overdue, dueToday, sig: `${overdue}|${dueToday}|${getLanguage()}` };
  };

  const renderBadge = (stats) => {
    const count = stats.overdue > 0 ? stats.overdue : stats.dueToday;
    if (count <= 0) return null;
    const text = count > 9 ? '9+' : String(count);
    const title = stats.overdue > 0 ? `${stats.overdue} ${t('Overdue').toLowerCase()}` : `${stats.dueToday} ${t('Today').toLowerCase()}`;
    return el('span', { class: `nav-badge${stats.overdue > 0 ? '' : ' info'}`, title }, text);
  };

  let lastBadgeSig = '';
  const renderNav = () => {
    const { path } = parseHash();
    const isApple = typeof navigator !== 'undefined' && /Mac|iPhone|iPad/i.test(navigator.platform || '');
    const kbdLabel = isApple ? '⌘K' : 'Ctrl K';
    const bStats = badgeStats();
    lastBadgeSig = bStats.sig;
    fab.setAttribute('aria-label', t('Quick create'));

    const links = ROUTES.map((r) =>
      el(
        'a.nav-link',
        {
          href: r.hash,
          'data-focus-key': `nav-${r.hash}`,
          'aria-current': r.hash === path ? 'page' : undefined,
        },
        el('span.nav-link-main', {}, el('span', {}, icon(r.icon, 16)), t(r.label)),
        r.hash === '#/followups' ? renderBadge(bStats) : null
      )
    );

    const acctName = store.currentAccount?.label || store.currentAccount?.id || 'default';
    const modeLabel = store.systemInfo?.mcpAccess === 'webmcp-only' ? t('WebMCP-only') : t('App Mode');
    const svcHub = store.services;
    const svcAvailable = svcHub?.isAvailable();
    const svcStates = svcAvailable ? svcHub.getStates() : [];
    const unhandledNotifs = svcAvailable ? svcHub.getEvents().filter((e) => !e.handled).length : 0;

    const serviceItems = svcAvailable
      ? [
          { id: 'calendar', icon: 'calendar', label: 'Google Calendar' },
          { id: 'mail', icon: 'mail', label: 'Gmail' },
          { id: 'docs', icon: 'fileText', label: 'Drive / Docs' },
        ].map((svc) => {
          const st = svcStates.find((x) => x.id === svc.id);
          return el(
            'button.nav-link',
            {
              type: 'button',
              style: 'width:100%;text-align:left;background:transparent;border:0;cursor:pointer',
              onclick: () => svcHub.open(svc.id).catch(() => {}),
            },
            el('span.nav-link-main', {}, el('span', {}, icon(svc.icon, 15)), t(svc.label)),
            st?.open ? el('span.nav-badge.info', { title: t('Open') }, '•') : null
          );
        })
      : [];

    const servicesSection = svcAvailable
      ? el(
          'div',
          { style: 'margin:10px 0 6px;padding-top:8px;border-top:1px solid var(--hairline)' },
          el(
            'div',
            { style: 'display:flex;justify-content:space-between;align-items:center;padding:0 8px 4px;font-size:11px;font-weight:600;color:var(--ink-muted)' },
            el('span', {}, t('Connected Services')),
            unhandledNotifs ? el('span.nav-badge', {}, String(unhandledNotifs)) : null
          ),
          ...serviceItems,
          svcStates.find((x) => x.id === 'mail')?.open
            ? el(
                'button.btn.btn-sm.btn-ghost',
                {
                  type: 'button',
                  style: 'width:100%;margin-top:4px',
                  disabled: svcHub.isScanning(),
                  onclick: async () => {
                    const res = await svcHub.scanMailInbox();
                    if (!res.scanned) {
                      toast(res.error || t('Open the Gmail service tab first.'), { tone: 'warn' });
                    } else {
                      toast(t('Scanned {rows} emails · {count} calendar/CRM notices', { rows: res.totalRows || 0, count: res.added || 0 }), {
                        action: t('View'),
                        onAction: () => { location.hash = '#/settings'; },
                      });
                    }
                  },
                },
                icon('repeat', 13),
                ` ${svcHub.isScanning() ? t('Syncing…') : t('Sync inbox')}`
              )
            : null
        )
      : null;

    sidebar.replaceChildren(
      el(
        'div.brand',
        {},
        el('img', { src: '/icon.svg', width: 28, height: 28, alt: '' }),
        el('div', {}, el('div.brand-title', {}, 'Sales CRM'), el('div.brand-sub', {}, modeLabel))
      ),
      el(
        'button.search-trigger',
        { type: 'button', onclick: () => window.dispatchEvent(new CustomEvent('crm:search')) },
        el('span', { style: 'display:inline-flex;align-items:center;gap:6px' }, el('span', {}, icon('search', 14)), t('Search')),
        el('span.kbd', {}, kbdLabel)
      ),
      el('div.nav-list', {}, ...links),
      servicesSection,
      el(
        'div.sidebar-footer',
        {},
        el('button.btn.btn-primary', { type: 'button', onclick: openQuickAdd }, el('span', {}, icon('plus', 15)), t('New')),
        el(
          'div.account-chip',
          {},
          avatarEl(acctName),
          el('div', { style: 'min-width:0;flex:1' }, el('div.row-title.truncate', {}, acctName), el('div.row-meta.truncate', {}, t('Account: {id}', { id: store.currentAccount?.id || 'default' })))
        )
      )
    );

    const activeRoute = ROUTES.find((r) => r.hash === path) || ROUTES[0];
    const tabItems = ROUTES.filter((r) => r.tab).map((r) =>
      el(
        'a.tabbar-item',
        { href: r.hash, 'aria-current': r.hash === path ? 'page' : undefined },
        el('span', {}, icon(r.icon, 18)),
        t(r.label),
        r.hash === '#/followups' ? (() => {
          const b = renderBadge(bStats);
          if (b) b.classList.add('tabbar-badge');
          return b;
        })() : null
      )
    );

    const moreBtn = el(
      'button.tabbar-item',
      {
        type: 'button',
        className: activeRoute.tab ? '' : 'active',
        onclick: () => {
          const dlg = openModal({
            title: t('More'),
            focusFirst: true,
            body: el(
              'div.row-list',
              {},
              el('button.list-row', { type: 'button', style: 'cursor:pointer', onclick: () => { dlg.close(); window.dispatchEvent(new CustomEvent('crm:search')); } }, el('span.row-title', {}, t('Search')), el('span.kbd', {}, kbdLabel)),
              ...ROUTES.filter((r) => !r.tab).map((r) =>
                el(
                  'a.list-row',
                  { href: r.hash, onclick: () => dlg.close() },
                  el('div', {}, el('div.row-title', {}, t(r.label)), el('div.row-meta', {}, t(r.sub)))
                )
              )
            ),
          });
        },
      },
      el('span', {}, icon('more', 18)),
      t('More')
    );

    tabbar.replaceChildren(...tabItems, moreBtn);
  };

  const renderSampleBanner = () => {
    if (!store.settings.sampleData) {
      sampleBanner.hidden = true;
      sampleBanner.replaceChildren();
      return;
    }
    sampleBanner.hidden = false;
    sampleBanner.replaceChildren(
      el('span', {}, t('Exploring with sample data — changes are local to this account.')),
      el(
        'button.btn.btn-sm',
        {
          type: 'button',
          onclick: async () => {
            if (!(await confirmDialog(t('Clear sample data and start with a blank pipeline?'), { confirmLabel: t('Start fresh') }))) return;
            await wipeSampleData(store);
            openOnboarding(store, () => openContactForm(store));
          },
        },
        t('Start fresh')
      )
    );
  };

  const scrollMemory = new Map();
  let lastRawHash = parseHash().raw;
  if (!history.state?.crmVisited) {
    history.replaceState({ ...(history.state || {}), crmVisited: true }, '');
  }

  let viewCleanup = null;
  const syncTitle = () => {
    const { path } = parseHash();
    const route = ROUTES.find((r) => r.hash === path) || ROUTES[0];
    document.title = route.hash === '#/' ? 'Sales CRM' : `${t(route.label)} · Sales CRM`;
  };

  const renderRoute = (restoreScroll = false) => {
    const { raw, path, params } = parseHash();
    const route = ROUTES.find((r) => r.hash === path) || ROUTES[0];
    syncTitle();

    if (typeof viewCleanup === 'function') {
      viewCleanup();
      viewCleanup = null;
    }
    clear(mainView);
    const ctx = { store, params, actions, openQuickAdd };
    const maybeCleanup = route.view(mainView, ctx);
    if (typeof maybeCleanup === 'function') viewCleanup = maybeCleanup;

    renderNav();
    renderSampleBanner();
    if (restoreScroll && scrollMemory.has(raw)) {
      window.scrollTo(0, scrollMemory.get(raw));
    } else if (!restoreScroll) {
      window.scrollTo(0, 0);
    }
  };

  let badgeTimer = 0;
  store.services?.addEventListener('change', () => keepingFocus(renderNav));
  store.addEventListener('change', (ev) => {
    if (ev?.detail?.type === 'webmcp') return;
    renderSampleBanner();
    if (ev?.detail?.type === 'settings' || ev?.detail?.type === 'importAll') {
      syncTitle();
      if (badgeStats().sig !== lastBadgeSig) keepingFocus(renderNav);
    }
    clearTimeout(badgeTimer);
    badgeTimer = setTimeout(() => {
      const nextSig = badgeStats().sig;
      if (nextSig !== lastBadgeSig) {
        keepingFocus(renderNav);
      }
    }, 400);
  });

  window.addEventListener('hashchange', () => {
    closeAllModals();
    scrollMemory.set(lastRawHash, window.scrollY);
    if (scrollMemory.size > SCROLL_MEMORY_MAX) {
      const oldest = scrollMemory.keys().next().value;
      scrollMemory.delete(oldest);
    }
    const returnVisit = Boolean(history.state?.crmVisited);
    if (!history.state?.crmVisited) {
      history.replaceState({ ...(history.state || {}), crmVisited: true }, '');
    }
    lastRawHash = parseHash().raw;
    renderRoute(returnVisit);
    mainView.focus({ preventScroll: true });
  });

  renderRoute(false);
}
