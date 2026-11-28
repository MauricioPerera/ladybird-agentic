/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { getLanguagePreference, setLanguage, SUPPORTED_LANGUAGES, t } from '../lib/i18n.js';
import { sampleDataset } from '../lib/sample.js';
import { CURRENCIES } from '../lib/util.js';
import { el } from './dom.js';
import { openModal } from './modal.js';
import { toast } from './toast.js';

export function shouldOnboard(store) {
  return (
    !store.settings.onboarded &&
    !store.settings.sampleData &&
    store.contacts.length === 0 &&
    store.companies.length === 0 &&
    store.deals.length === 0
  );
}

export function openOnboarding(store, openContactForm = null) {
  let step = 0;
  let currency = store.settings.currency || 'USD';
  let language = store.settings.language || getLanguagePreference() || 'auto';
  const body = el('div', { style: 'display:flex;flex-direction:column;gap:12px' });
  const foot = el('div.btn-group', { style: 'display:flex;gap:8px' });
  let dlg = null;

  const markDone = () => {
    if (!store.settings.onboarded) {
      store.updateSettings({ onboarded: true }).catch(() => {});
    }
  };

  const render = () => {
    const titleEl = dlg?.card?.querySelector('.modal-title');
    if (titleEl) titleEl.textContent = t('Welcome to Sales CRM');
    if (step === 0) {
      const langRow = el(
        'div.field',
        {},
        el('span.field-label', {}, t('Language')),
        el(
          'div.segmented',
          {},
          ...SUPPORTED_LANGUAGES.map((l) =>
            el(
              'button',
              {
                type: 'button',
                class: `seg-btn${language === l.id ? ' active' : ''}`,
                'aria-pressed': String(language === l.id),
                onclick: () => {
                  language = l.id;
                  setLanguage(language);
                  store.updateSettings({ language }).catch(() => {});
                  render();
                },
              },
              l.labelKey ? t(l.labelKey) : l.label
            )
          )
        )
      );
      body.replaceChildren(
        el('p', { style: 'margin:0' }, t('Track leads, companies, deals, follow-ups, and revenue forecasts locally in isolated per-account storage.')),
        langRow
      );
      foot.replaceChildren(
        el('button.btn.btn-ghost', { type: 'button', onclick: () => dlg.close() }, t('Skip for now')),
        el('button.btn', {
          type: 'button',
          onclick: async () => {
            await store.importAll({ ...sampleDataset(store), settings: { ...(store.settings || {}), language } }, { reason: 'sample' });
            dlg.close();
            toast(t('Sample data loaded'));
          },
        }, t('Explore with sample data')),
        el('button.btn.btn-primary', { type: 'button', onclick: () => { step = 1; render(); } }, t('Set up my pipeline'))
      );
      return;
    }

    const sel = el(
      'select.select',
      { id: 'onboard-currency', onchange: (e) => (currency = e.target.value) },
      ...CURRENCIES.map((c) => el('option', { value: c, selected: c === currency }, c))
    );
    body.replaceChildren(
      el('div.field', {}, el('label.field-label', { for: 'onboard-currency' }, t('Preferred currency')), sel)
    );
    foot.replaceChildren(
      el('button.btn', { type: 'button', onclick: () => { step = 0; render(); } }, t('Back')),
      el('button.btn.btn-primary', {
        type: 'button',
        onclick: async () => {
          await store.updateSettings({ currency, language, onboarded: true });
          dlg.close();
          openContactForm?.(null);
        },
      }, t('Save & add first lead'))
    );
  };

  dlg = openModal({
    title: t('Welcome to Sales CRM'),
    focusFirst: true,
    body,
    foot,
    onClose: markDone,
  });
  render();
}
