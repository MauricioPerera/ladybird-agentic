/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { openStages } from '../lib/forecast.js';
import { stageLabel, t } from '../lib/i18n.js';
import {
  MAX_PLAYBOOK_NAME,
  MAX_PLAYBOOK_STEPS,
  MAX_PLAYBOOKS,
  MAX_STEP_TITLE,
  offsetLabel,
  playbookForStage,
  playbookTasks,
} from '../lib/playbooks.js';
import { formatDate, todayISO } from '../lib/util.js';
import { el } from './dom.js';
import { icon } from './icons.js';
import { openModal } from './modal.js';
import { toast } from './toast.js';

export function openPlaybookPreview(store, playbook, { dealId = null, contactId = null } = {}) {
  const today = todayISO();
  const tasks = playbookTasks(playbook, today);
  const dlg = openModal({
    title: t('Apply playbook: {name}', { name: playbook.name }),
    focusFirst: true,
    body: el(
      'div',
      { style: 'display:flex;flex-direction:column;gap:8px' },
      el('p.muted', { style: 'margin:0' }, t('{count} follow-up(s) will be created from {date}:', { count: tasks.length, date: formatDate(today) })),
      el(
        'div.row-list',
        {},
        ...tasks.map((tsk, i) =>
          el(
            'div.list-row',
            {},
            el('div.row-title', {}, tsk.title),
            el('span.pill', {}, `${offsetLabel(playbook.steps[i]?.offset || 0)} · ${formatDate(tsk.due)}`)
          )
        )
      )
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
            const created = await store.applyPlaybook(playbook, { dealId, contactId, from: today });
            dlg.close();
            toast(t('Added {count} follow-up(s)', { count: created.length }), {
              duration: 7000,
              action: t('Undo'),
              onAction: () => store.undoAddedTasks(created.map((tsk) => tsk.id)),
            });
          },
        },
        t('Apply {count} step(s)', { count: tasks.length })
      )
    ),
  });
}

export function openPlaybookPicker(store, { dealId = null, contactId = null } = {}) {
  const list = store.settings.playbooks || [];
  if (list.length === 0) {
    toast(t('Create a playbook in Settings first.'));
    return;
  }
  const dlg = openModal({
    title: t('Run a follow-up playbook'),
    focusFirst: true,
    body: el(
      'div.row-list',
      {},
      ...list.map((pb) =>
        el(
          'button.list-row',
          {
            type: 'button',
            style: 'cursor:pointer;text-align:left',
            onclick: () => {
              dlg.close();
              openPlaybookPreview(store, pb, { dealId, contactId });
            },
          },
          el('span.row-title', {}, pb.name),
          el('span.pill', {}, t('{count} steps', { count: pb.steps.length }))
        )
      )
    ),
  });
}

export function maybeOfferPlaybook(store, deal, stageId) {
  const book = playbookForStage(store.settings, stageId);
  if (!book || book.steps.length === 0) return false;
  if (store.hasOpenTask(deal.id)) return false;
  openPlaybookPreview(store, book, { dealId: deal.id, contactId: deal.contactId });
  return true;
}

export function openPlaybookForm(store, existing = null) {
  const stages = openStages(store.resolvedStages());
  const nameInput = el('input.input', { type: 'text', maxlength: MAX_PLAYBOOK_NAME, value: existing?.name || '' });
  const triggerSelect = el(
    'select.select',
    {},
    el('option', { value: '' }, t('Only when I ask')),
    ...stages.map((s) => el('option', { value: s.id, selected: existing?.trigger === s.id }, t('Offer on reaching {stage}', { stage: stageLabel(s, stages) })))
  );

  let steps = existing?.steps?.length
    ? existing.steps.map((s) => ({ ...s }))
    : [{ title: t('Follow up with recap'), offset: 0, templateId: '' }];
  const stepsWrap = el('div.row-list');

  const renderSteps = () => {
    stepsWrap.replaceChildren(
      ...steps.map((st, idx) => {
        const titleEl = el('input.input', {
          type: 'text',
          maxlength: MAX_STEP_TITLE,
          value: st.title,
          placeholder: t('Follow-up title'),
          oninput: (e) => (st.title = e.target.value),
        });
        const offEl = el('input.input.input-sm', {
          type: 'number',
          min: 0,
          max: 365,
          value: st.offset,
          title: t('Day offset'),
          oninput: (e) => (st.offset = Number(e.target.value) || 0),
        });
        const tplEl = el(
          'select.select.select-sm',
          { onchange: (e) => (st.templateId = e.target.value) },
          el('option', { value: '' }, t('No template')),
          ...store.templates.map((tpl) => el('option', { value: tpl.id, selected: st.templateId === tpl.id }, tpl.name))
        );
        return el(
          'div.list-row',
          { style: 'flex-wrap:wrap;gap:6px' },
          el('div', { style: 'flex:1;min-width:180px' }, titleEl),
          el('span.muted', {}, '+d'),
          offEl,
          tplEl,
          steps.length > 1
            ? el(
                'button.icon-btn',
                {
                  type: 'button',
                  'aria-label': t('Remove step'),
                  onclick: () => {
                    steps.splice(idx, 1);
                    renderSteps();
                  },
                },
                icon('x', 14)
              )
            : null
        );
      })
    );
  };
  renderSteps();

  const dlg = openModal({
    title: existing ? t('Edit playbook') : t('New playbook'),
    wide: true,
    guardUnsaved: true,
    body: el(
      'div',
      { style: 'display:flex;flex-direction:column;gap:12px' },
      el(
        'div.form-grid',
        {},
        el('div.field', {}, el('label.field-label', {}, t('Name')), nameInput),
        el('div.field', {}, el('label.field-label', {}, t('Stage trigger')), triggerSelect)
      ),
      el(
        'div.panel-row',
        { style: 'display:flex;justify-content:space-between;align-items:center' },
        el('span.field-label', {}, t('Steps')),
        el('button.btn.btn-sm', {
          type: 'button',
          onclick: () => {
            if (steps.length >= MAX_PLAYBOOK_STEPS) return;
            steps.push({ title: '', offset: (steps[steps.length - 1]?.offset || 0) + 3, templateId: '' });
            renderSteps();
          },
        }, t('+ Add step'))
      ),
      stepsWrap
    ),
    foot: el(
      'div.btn-group',
      { style: 'display:flex;gap:8px' },
      el('button.btn', { type: 'button', onclick: () => dlg.requestClose() }, t('Cancel')),
      el('button.btn.btn-primary', {
        type: 'button',
        onclick: async () => {
          const name = nameInput.value.trim();
          if (!name) {
            toast(t('Playbook name is required'), { tone: 'danger' });
            nameInput.focus();
            return;
          }
          const validSteps = steps.map((s) => ({ ...s, title: String(s.title || '').trim() })).filter((s) => s.title);
          if (validSteps.length === 0) {
            toast(t('Add at least one step with a title'), { tone: 'danger' });
            return;
          }
          const cur = [...(store.settings.playbooks || [])];
          const entry = { id: existing?.id, name, trigger: triggerSelect.value, steps: validSteps };
          if (existing) {
            const i = cur.findIndex((p) => p.id === existing.id);
            if (i >= 0) cur[i] = entry;
          } else if (cur.length < MAX_PLAYBOOKS) {
            cur.push(entry);
          }
          try {
            await store.setPlaybooks(cur);
            dlg.close();
          } catch (err) {
            toast(err?.message || t('Could not save playbook'), { tone: 'danger' });
          }
        },
      }, t('Save playbook'))
    ),
  });
}
