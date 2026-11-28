/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from '../lib/i18n.js';
import { buildMailto, fillTemplate, MERGE_FIELDS, templateContext } from '../lib/templates.js';
import { todayISO } from '../lib/util.js';
import { el } from './dom.js';
import { openTaskForm } from './forms.js';
import { openModal } from './modal.js';
import { maybeNudgeNextStep } from './nudge.js';
import { toast } from './toast.js';

export function openCompose(store, { contact = null, company = null, deal = null, template = null } = {}) {
  const ctx = templateContext({ contact, company, deal, currency: store.settings.currency });
  const toInput = el('input.input', { id: 'crm-compose-to', type: 'email', required: true, value: contact?.email || '', placeholder: 'name@company.com' });
  const subjectInput = el('input.input', { id: 'crm-compose-subject', type: 'text', value: template ? fillTemplate(template.subject, ctx) : '' });
  const bodyInput = el('textarea.textarea', { id: 'crm-compose-body', style: 'min-height:150px', value: template ? fillTemplate(template.body, ctx) : '' });

  const logEmailAction = () => ({
    duration: 10000,
    action: t('Log it'),
    onAction: async () => {
      await store.addActivity({
        type: 'email',
        date: todayISO(),
        summary: subjectInput.value.trim() || t('Sent email'),
        notes: bodyInput.value.trim(),
        contactId: contact?.id || null,
        companyId: company?.id || contact?.companyId || deal?.companyId || null,
        dealId: deal?.id || null,
      });
      maybeNudgeNextStep(store, {
        dealId: deal?.id || null,
        contactId: contact?.id || null,
        openTaskForm: (tsk, o) => openTaskForm(store, tsk, o),
      });
    },
  });

  const gmailBtn = store.services?.isAvailable()
    ? el(
        'button.btn.btn-primary',
        {
          type: 'button',
          onclick: async () => {
            if (!toInput.value.trim() || !toInput.checkValidity()) {
              toInput.reportValidity();
              toInput.focus();
              return;
            }
            try {
              await store.services.composeInGmail({
                to: toInput.value.trim(),
                subject: subjectInput.value,
                body: bodyInput.value,
              });
              dlg.close();
              toast(t('Opened Gmail tab'), logEmailAction());
            } catch (err) {
              toast(err?.message || t('Could not open Gmail tab'), { tone: 'danger' });
            }
          },
        },
        t('Open in Gmail tab')
      )
    : null;

  const dlg = openModal({
    title: t('Compose email'),
    wide: true,
    guardUnsaved: true,
    focusFirst: true,
    body: el(
      'div.form-grid',
      {},
      el('div.field.full', {}, el('label.field-label', { for: 'crm-compose-to' }, t('To')), toInput),
      el('div.field.full', {}, el('label.field-label', { for: 'crm-compose-subject' }, t('Subject')), subjectInput),
      el('div.field.full', {}, el('label.field-label', { for: 'crm-compose-body' }, t('Message')), bodyInput)
    ),
    foot: el(
      'div.btn-group',
      { style: 'display:flex;gap:8px' },
      el('button.btn', { type: 'button', onclick: () => dlg.requestClose() }, t('Cancel')),
      el(
        gmailBtn ? 'button.btn' : 'button.btn.btn-primary',
        {
          type: 'button',
          onclick: () => {
            if (!toInput.value.trim() || !toInput.checkValidity()) {
              toInput.reportValidity();
              toInput.focus();
              return;
            }
            const href = buildMailto(toInput.value.trim(), subjectInput.value, bodyInput.value);
            window.location.href = href;
            dlg.close();
            toast(t('Opened mail app'), logEmailAction());
          },
        },
        t('Open in mail app')
      ),
      gmailBtn
    ),
  });
}

export function openEmailFlow(store, { contact = null, company = null, deal = null, template = null } = {}) {
  if (template || store.templates.length === 0) {
    openCompose(store, { contact, company, deal, template });
    return;
  }

  const ctx = templateContext({ contact, company, deal, currency: store.settings.currency });
  const dlg = openModal({
    title: t('Choose an email template'),
    focusFirst: true,
    body: el(
      'div.row-list',
      {},
      el(
        'button.list-row',
        {
          type: 'button',
          style: 'cursor:pointer;text-align:left',
          onclick: () => {
            dlg.close();
            openCompose(store, { contact, company, deal });
          },
        },
        el('div', {}, el('div.row-title', {}, t('Blank email')), el('div.row-meta', {}, t('Start with an empty draft')))
      ),
      ...store.templates.map((tpl) =>
        el(
          'button.list-row',
          {
            type: 'button',
            style: 'cursor:pointer;text-align:left',
            onclick: () => {
              dlg.close();
              openCompose(store, { contact, company, deal, template: tpl });
            },
          },
          el('div', {}, el('div.row-title', {}, tpl.name), el('div.row-meta.truncate', {}, fillTemplate(tpl.subject, ctx)))
        )
      )
    ),
  });
}

export function openTemplateForm(store, existing = null) {
  const nameInput = el('input.input', { id: 'crm-tpl-name', type: 'text', required: true, value: existing?.name || '' });
  const subjectInput = el('input.input', { id: 'crm-tpl-subject', type: 'text', required: true, value: existing?.subject || '' });
  const bodyInput = el('textarea.textarea', { id: 'crm-tpl-body', style: 'min-height:140px', value: existing?.body || '' });

  let lastTarget = bodyInput;
  subjectInput.addEventListener('focus', () => (lastTarget = subjectInput));
  bodyInput.addEventListener('focus', () => (lastTarget = bodyInput));

  const insertToken = (token) => {
    const start = lastTarget.selectionStart ?? lastTarget.value.length;
    const end = lastTarget.selectionEnd ?? start;
    lastTarget.value = lastTarget.value.slice(0, start) + token + lastTarget.value.slice(end);
    lastTarget.focus();
    lastTarget.setSelectionRange(start + token.length, start + token.length);
  };

  const form = el(
    'form',
    {
      onsubmit: async (e) => {
        e.preventDefault();
        const name = nameInput.value.trim();
        const subject = subjectInput.value.trim();
        if (!name || !subject) {
          toast(t('Template name and subject are required'), { tone: 'danger' });
          return;
        }
        try {
          const data = { name, subject, body: bodyInput.value };
          if (existing) await store.updateTemplate(existing.id, data);
          else await store.addTemplate(data);
          dlg.close();
        } catch (err) {
          toast(err?.message || t('Could not save template'), { tone: 'danger' });
        }
      },
    },
    el(
      'div.form-grid',
      {},
      el('div.field.full', {}, el('label.field-label', { for: 'crm-tpl-name' }, t('Template name')), nameInput),
      el('div.field.full', {}, el('label.field-label', { for: 'crm-tpl-subject' }, t('Subject')), subjectInput),
      el(
        'div.field.full',
        {},
        el('span.field-label', {}, t('Merge fields')),
        el('div.chip-row', {}, ...MERGE_FIELDS.map((mf) => el('button.pill', { type: 'button', onclick: () => insertToken(mf.token) }, t(mf.label))))
      ),
      el('div.field.full', {}, el('label.field-label', { for: 'crm-tpl-body' }, t('Body')), bodyInput)
    )
  );

  const dlg = openModal({
    title: existing ? t('Edit email template') : t('New email template'),
    wide: true,
    guardUnsaved: true,
    body: form,
    foot: el(
      'div.btn-group',
      { style: 'display:flex;gap:8px' },
      el('button.btn', { type: 'button', onclick: () => dlg.requestClose() }, t('Cancel')),
      el('button.btn.btn-primary', { type: 'button', onclick: () => form.requestSubmit() }, t('Save template'))
    ),
  });
}
