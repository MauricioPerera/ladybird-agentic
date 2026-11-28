/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { runBackupIfDue } from './lib/backups.js';
import { setLanguage, t } from './lib/i18n.js';
import { startReminders } from './lib/reminders.js';
import { createServiceHub } from './lib/services.js';
import { store } from './lib/store.js';
import { registerSalesCRMWebMCP } from './lib/webmcp.js';
import { el } from './ui/dom.js';
import { openContactForm } from './ui/forms.js';
import { openOnboarding, shouldOnboard } from './ui/onboarding.js';
import { mountShell } from './ui/shell.js';
import { applyTheme } from './ui/theme.js';

const appRoot = document.getElementById('app');

async function boot() {
  try {
    await store.init();
  } catch (err) {
    console.error('Sales CRM boot error:', err);
    appRoot.replaceChildren(
      el(
        'div.empty-state',
        {},
        el('h2', {}, t('Could not load Sales CRM storage')),
        el('p.muted', {}, err?.message || String(err)),
        el('button.btn.btn-primary', { type: 'button', onclick: () => location.reload() }, t('Retry'))
      )
    );
    return;
  }

  applyTheme(store.settings.theme || 'system');
  setLanguage(store.settings.language || 'auto');
  store.addEventListener('change', (ev) => {
    const kind = ev?.detail?.type;
    if (kind === 'importAll') {
      applyTheme(store.settings.theme || 'system');
      setLanguage(store.settings.language || 'auto');
    } else if (kind === 'settings') {
      setLanguage(store.settings.language || 'auto');
    }
  });
  registerSalesCRMWebMCP(store);
  store.services = createServiceHub(store);
  store.services.start();
  mountShell(appRoot, store);
  startReminders(store);
  runBackupIfDue(store).catch(() => {});

  if (shouldOnboard(store)) {
    openOnboarding(store, () => openContactForm(store));
  }
}

boot();
