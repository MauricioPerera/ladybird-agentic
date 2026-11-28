/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from '../lib/i18n.js';
import { addDays, todayISO } from '../lib/util.js';
import { el } from './dom.js';
import { openModal } from './modal.js';
import { toast } from './toast.js';

export function maybeNudgeNextStep(store, { dealId = null, contactId = null, defaultTitle = null, openTaskForm = null } = {}) {
  if (store.settings.nextStepNudges === false) return false;
  if (store.hasOpenFollowUp(contactId, dealId)) return false;

  toast(t('No next follow-up scheduled'), {
    duration: 8000,
    action: t('Schedule'),
    onAction: () => openNextStepSheet(store, { dealId, contactId, defaultTitle: defaultTitle || t('Follow up'), openTaskForm }),
  });
  return true;
}

export function openNextStepSheet(store, { dealId = null, contactId = null, defaultTitle = null, openTaskForm = null } = {}) {
  const titleInput = el('input.input', { type: 'text', value: defaultTitle || t('Follow up') });
  const today = todayISO();

  const pickOffset = async (days) => {
    const due = addDays(today, days);
    await store.addTask({ title: titleInput.value.trim() || t('Follow up'), due, dealId, contactId });
    dlg.close();
    toast(t('Scheduled for {date}', { date: due }));
  };

  const dlg = openModal({
    title: t('Schedule next follow-up'),
    focusFirst: true,
    body: el(
      'div',
      { style: 'display:flex;flex-direction:column;gap:12px' },
      el('div.field', {}, el('label.field-label', {}, t('Title')), titleInput),
      el(
        'div.chip-row',
        {},
        el('button.btn', { type: 'button', onclick: () => pickOffset(3) }, t('In 3 days')),
        el('button.btn', { type: 'button', onclick: () => pickOffset(7) }, t('In a week')),
        el('button.btn', { type: 'button', onclick: () => pickOffset(30) }, t('In a month'))
      )
    ),
    foot: openTaskForm
      ? el(
          'button.btn.btn-ghost',
          {
            type: 'button',
            onclick: () => {
              dlg.close();
              openTaskForm(null, { dealId, contactId, title: titleInput.value.trim() });
            },
          },
          t('Pick a date and time…')
        )
      : null,
  });
}
