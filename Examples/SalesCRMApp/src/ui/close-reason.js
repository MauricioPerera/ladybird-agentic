/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { reasonsForStage } from '../lib/forecast.js';
import { t, reasonLabel } from '../lib/i18n.js';
import { el } from './dom.js';
import { openModal } from './modal.js';

export function openCloseReasonForm(store, deal, onDone = null) {
  let chosen = deal.closeReason || '';
  const options = reasonsForStage(deal.stage, store.deals);
  const noteInput = el('textarea.textarea', {
    maxlength: 500,
    placeholder: t('Optional context (what won it or what stopped it)…'),
    value: deal.closeReasonNote || '',
  });
  const chipsWrap = el('div.chip-row');

  const renderChips = () => {
    chipsWrap.replaceChildren(
      ...options.map((r) =>
        el(
          'button',
          {
            type: 'button',
            class: `filter-chip${chosen === r ? ' active' : ''}`,
            onclick: () => {
              chosen = chosen === r ? '' : r;
              renderChips();
            },
          },
          reasonLabel(r)
        )
      )
    );
  };
  renderChips();

  let doneFired = false;
  const finish = () => {
    if (doneFired) return;
    doneFired = true;
    onDone?.();
  };

  const dlg = openModal({
    title: deal.stage === 'won' ? t('Why did this deal win?') : t('Why was this deal lost?'),
    focusFirst: true,
    body: el('div', { style: 'display:flex;flex-direction:column;gap:12px' }, chipsWrap, noteInput),
    foot: el(
      'div.btn-group',
      { style: 'display:flex;gap:8px' },
      el(
        'button.btn.btn-ghost',
        {
          type: 'button',
          onclick: () => {
            dlg.close();
          },
        },
        t('Skip')
      ),
      el(
        'button.btn.btn-primary',
        {
          type: 'button',
          onclick: async () => {
            await store.setCloseReason(deal.id, chosen, noteInput.value);
            dlg.close();
          },
        },
        t('Save')
      )
    ),
    onClose: finish,
  });
}
