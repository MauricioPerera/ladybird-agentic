/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { DEAL_ROLES, MAX_DEAL_CONTACTS, normalizeDealContacts } from '../lib/deal-contacts.js';
import { contactByEmail } from '../lib/dupes.js';
import { CONFIDENCE_LEVELS, isTerminalStage } from '../lib/forecast.js';
import { leadSourceLabel, stageLabel, t } from '../lib/i18n.js';
import { BILLING_MODES, DEFAULT_TERM } from '../lib/recurring.js';
import { REPEAT_UNITS } from '../lib/repeat.js';
import { ACTIVITY_TYPES, CONTACT_STATUSES } from '../lib/statuses.js';
import { LEAD_SOURCES, todayISO } from '../lib/util.js';
import { deletedToast } from './bits.js';
import { exportTaskIcs } from './calendar.js';
import { openCloseReasonForm } from './close-reason.js';
import { customFieldsInputs } from './custom-fields.js';
import { el } from './dom.js';
import { icon } from './icons.js';
import { confirmDialog, openModal } from './modal.js';
import { maybeNudgeNextStep } from './nudge.js';
import { maybeOfferPlaybook } from './playbooks.js';
import { tagInputControl } from './tag-input.js';
import { toast } from './toast.js';

let formFieldSeq = 0;

function fieldRow(labelText, control, full = false) {
  const wrapClass = full ? 'div.field.full' : 'div.field';
  let targetId = control?.id;
  if (!targetId && control && /^(INPUT|SELECT|TEXTAREA)$/.test(control.tagName || '')) {
    targetId = `crm-fld-${++formFieldSeq}`;
    control.id = targetId;
  }
  return el(wrapClass, {}, el('label.field-label', targetId ? { for: targetId } : {}, labelText), control);
}

function wireForm(form, onSave) {
  let busy = false;
  form.addEventListener('submit', async (e) => {
    e.preventDefault();
    if (busy || !form.reportValidity()) return;
    busy = true;
    try {
      await onSave();
    } catch (err) {
      toast(err?.message || t('Could not save'), { tone: 'danger' });
    } finally {
      busy = false;
    }
  });
}

function pasteExtractBox(kind, onApply) {
  const wrap = el('details', { style: 'border:1px dashed var(--hairline-strong);border-radius:var(--r-sm);padding:8px 10px' });
  const ta = el('textarea.textarea', { placeholder: t('Paste email signature or bio to extract name, email, phone, website…'), style: 'min-height:56px;margin-top:6px' });
  const btn = el('button.btn.btn-sm', {
    type: 'button',
    style: 'margin-top:6px',
    onclick: () => {
      const raw = ta.value.slice(0, 4000);
      const email = (raw.match(/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/) || [])[0] || '';
      const phone = (raw.match(/(?:\+?\d[\d\s().-]{6,}\d)/) || [])[0]?.trim() || '';
      const website = (raw.match(/https?:\/\/[^\s]+|(?:www\.)?[a-z0-9-]+\.[a-z]{2,}/i) || [])[0] || '';
      const lines = raw.split(/\r?\n/).map((l) => l.trim()).filter(Boolean);
      if (kind === 'company') {
        onApply({ name: lines[0] || '', website });
      } else {
        onApply({
          name: lines[0] && !lines[0].includes('@') ? lines[0] : '',
          title: lines[1] && !lines[1].includes('@') && lines[1] !== phone ? lines[1] : '',
          email,
          phone,
        });
      }
      wrap.open = false;
    },
  }, t('Extract into empty fields'));
  wrap.append(el('summary', { class: 'field-label', style: 'cursor:pointer' }, t('Paste details to auto-fill')), ta, btn);
  return wrap;
}

export function openCompanyForm(store, existing = null, { onSaved = null } = {}) {
  const nameInput = el('input.input', { type: 'text', required: true, value: existing?.name || '' });
  const industryInput = el('input.input', { type: 'text', value: existing?.industry || '', placeholder: t('e.g. Software') });
  const websiteInput = el('input.input', { type: 'text', value: existing?.website || '', placeholder: 'company.com' });
  const sizeInput = el('input.input', { type: 'text', value: existing?.size || '', placeholder: t('e.g. 50-200') });
  const notesInput = el('textarea.textarea', { value: existing?.notes || '' });
  const tagsCtrl = tagInputControl(existing?.tags || [], store.allTags());
  const cf = customFieldsInputs(store.customFieldsFor('companies'), existing?.custom);

  const form = el('form');
  const grid = el(
    'div.form-grid',
    {},
    !existing
      ? el('div.field.full', {}, pasteExtractBox('company', (d) => {
          if (!nameInput.value && d.name) nameInput.value = d.name;
          if (!websiteInput.value && d.website) websiteInput.value = d.website;
        }))
      : null,
    fieldRow(t('Company name *'), nameInput, true),
    fieldRow(t('Industry'), industryInput),
    fieldRow(t('Size'), sizeInput),
    fieldRow(t('Website'), websiteInput, true),
    el('div.field.full', {}, el('span.field-label', {}, t('Tags')), tagsCtrl.el),
    ...cf.nodes,
    fieldRow(t('Notes'), notesInput, true)
  );
  form.appendChild(grid);

  const dlg = openModal({
    title: existing ? t('Edit {name}', { name: existing.name }) : t('New company'),
    wide: true,
    guardUnsaved: true,
    body: form,
    foot: el(
      'div.btn-group',
      { style: 'display:flex;gap:8px;width:100%;justify-content:space-between' },
      existing
        ? el('button.btn.btn-danger', {
            type: 'button',
            onclick: async () => {
              if (!(await confirmDialog(t('Delete "{name}"?', { name: existing.name }), { confirmLabel: t('Delete'), tone: 'danger' }))) return;
              const entry = await store.deleteCompany(existing.id);
              dlg.close();
              deletedToast(store, entry, t('Company'));
              if (window.location.hash.includes(existing.id)) window.location.hash = '#/companies';
            },
          }, t('Delete'))
        : el('span'),
      el(
        'div.chip-row',
        {},
        el('button.btn', { type: 'button', onclick: () => dlg.requestClose() }, t('Cancel')),
        el('button.btn.btn-primary', { type: 'button', onclick: () => form.requestSubmit() }, t('Save'))
      )
    ),
  });

  wireForm(form, async () => {
    const cleanName = nameInput.value.trim();
    if (!cleanName) {
      toast(t('Company name is required'), { tone: 'danger' });
      nameInput.focus();
      return;
    }
    const data = {
      name: cleanName,
      industry: industryInput.value.trim(),
      website: websiteInput.value.trim(),
      size: sizeInput.value.trim(),
      notes: notesInput.value,
      tags: tagsCtrl.getTags(),
      custom: cf.readValues(),
    };
    const saved = existing ? await store.updateCompany(existing.id, data) : await store.addCompany(data);
    dlg.close();
    onSaved?.(saved);
  });
}

export function openContactForm(store, existing = null, { presetCompanyId = null, onSaved = null } = {}) {
  const nameInput = el('input.input', { type: 'text', required: true, value: existing?.name || '' });
  const titleInput = el('input.input', { type: 'text', value: existing?.title || '' });
  const emailInput = el('input.input', { type: 'email', value: existing?.email || '' });
  const phoneInput = el('input.input', { type: 'text', value: existing?.phone || '' });
  const sourceInput = el('input.input', { type: 'text', value: existing?.source || '', list: 'crm-lead-sources' });
  const statusSelect = el(
    'select.select',
    {},
    ...CONTACT_STATUSES.map((s) => el('option', { value: s.id, selected: (existing?.status || 'lead') === s.id }, t(s.label)))
  );

  let selectedCompanyId = existing?.companyId ?? presetCompanyId ?? '';
  const companySelect = el('select.select');
  const rebuildCompanies = () => {
    const sorted = [...store.companies].sort((a, b) => a.name.localeCompare(b.name));
    companySelect.replaceChildren(
      el('option', { value: '' }, t('No company')),
      ...sorted.map((c) => el('option', { value: c.id, selected: c.id === selectedCompanyId }, c.name)),
      el('option', { value: '__new__' }, t('+ New company…'))
    );
  };
  rebuildCompanies();
  companySelect.addEventListener('change', () => {
    if (companySelect.value === '__new__') {
      companySelect.value = selectedCompanyId;
      openCompanyForm(store, null, {
        onSaved: (co) => {
          selectedCompanyId = co.id;
          rebuildCompanies();
        },
      });
    } else {
      selectedCompanyId = companySelect.value;
    }
  });

  const dupeBanner = el('div.row-meta', { style: 'color:var(--warning-ink)' });
  const checkDupe = () => {
    const dup = contactByEmail(store.contacts, emailInput.value, existing?.id || null);
    if (!dup) {
      dupeBanner.replaceChildren();
    } else {
      dupeBanner.replaceChildren(
        `${t('{name} already uses this email.', { name: dup.name })} `,
        el('button.btn.btn-sm', {
          type: 'button',
          onclick: () => {
            dlg.close();
            window.location.hash = `#/leads?id=${dup.id}`;
          },
        }, t('Open instead?'))
      );
    }
  };
  emailInput.addEventListener('input', checkDupe);

  const tagsCtrl = tagInputControl(existing?.tags || [], store.allTags());
  const notesInput = el('textarea.textarea', { value: existing?.notes || '' });
  const cf = customFieldsInputs(store.customFieldsFor('contacts'), existing?.custom);
  const srcList = el('datalist', { id: 'crm-lead-sources' }, ...LEAD_SOURCES.map((s) => el('option', { value: s }, leadSourceLabel(s))));

  const form = el(
    'form',
    {},
    srcList,
    el(
      'div.form-grid',
      {},
      !existing
        ? el('div.field.full', {}, pasteExtractBox('contact', (d) => {
            if (!nameInput.value && d.name) nameInput.value = d.name;
            if (!titleInput.value && d.title) titleInput.value = d.title;
            if (!emailInput.value && d.email) emailInput.value = d.email;
            if (!phoneInput.value && d.phone) phoneInput.value = d.phone;
            checkDupe();
          }))
        : null,
      fieldRow(t('Full name *'), nameInput),
      fieldRow(t('Title'), titleInput),
      el('div.field', {}, el('label.field-label', { for: (emailInput.id = `crm-fld-${++formFieldSeq}`) }, t('Email')), emailInput, dupeBanner),
      fieldRow(t('Phone'), phoneInput),
      fieldRow(t('Company'), companySelect),
      fieldRow(t('Status'), statusSelect),
      fieldRow(t('Lead source'), sourceInput),
      el('div.field.full', {}, el('span.field-label', {}, t('Tags')), tagsCtrl.el),
      ...cf.nodes,
      fieldRow(t('Notes'), notesInput, true)
    )
  );

  const dlg = openModal({
    title: existing ? t('Edit {name}', { name: existing.name }) : t('New lead / contact'),
    wide: true,
    guardUnsaved: true,
    body: form,
    foot: el(
      'div.btn-group',
      { style: 'display:flex;gap:8px;width:100%;justify-content:space-between' },
      existing
        ? el('button.btn.btn-danger', {
            type: 'button',
            onclick: async () => {
              if (!(await confirmDialog(t('Delete "{name}"?', { name: existing.name }), { confirmLabel: t('Delete'), tone: 'danger' }))) return;
              const entry = await store.deleteContact(existing.id);
              dlg.close();
              deletedToast(store, entry, t('Lead'));
              if (window.location.hash.includes(existing.id)) window.location.hash = '#/leads';
            },
          }, t('Delete'))
        : el('span'),
      el(
        'div.chip-row',
        {},
        el('button.btn', { type: 'button', onclick: () => dlg.requestClose() }, t('Cancel')),
        el('button.btn.btn-primary', { type: 'button', onclick: () => form.requestSubmit() }, t('Save'))
      )
    ),
  });

  wireForm(form, async () => {
    const cleanName = nameInput.value.trim();
    if (!cleanName) {
      toast(t('Lead name is required'), { tone: 'danger' });
      nameInput.focus();
      return;
    }
    const data = {
      name: cleanName,
      title: titleInput.value.trim(),
      email: emailInput.value.trim(),
      phone: phoneInput.value.trim(),
      companyId: selectedCompanyId || null,
      source: sourceInput.value.trim(),
      status: statusSelect.value,
      tags: tagsCtrl.getTags(),
      custom: cf.readValues(),
      notes: notesInput.value,
    };
    const saved = existing ? await store.updateContact(existing.id, data) : await store.addContact(data);
    dlg.close();
    onSaved?.(saved);
  });
}

export function openDealForm(store, existing = null, { presetContactId = null, presetCompanyId = null, onSaved = null } = {}) {
  const stages = store.resolvedStages();
  const nameInput = el('input.input', { type: 'text', required: true, value: existing?.name || '' });
  const valueInput = el('input.input', { type: 'number', step: 'any', min: 0, required: true, value: existing?.value ?? 0 });
  const billingSelect = el(
    'select.select',
    {},
    ...BILLING_MODES.map((m) => el('option', { value: m.id, selected: (existing?.billing || 'one-time') === m.id }, t(m.label)))
  );
  const termInput = el('input.input', { type: 'number', min: 1, max: 120, value: existing?.term ?? DEFAULT_TERM });
  const termField = el('div.field', {}, el('label.field-label', {}, t('Term (months)')), termInput);
  const syncBillingUi = () => {
    const recurring = billingSelect.value !== 'one-time';
    termField.style.display = recurring ? '' : 'none';
    termInput.disabled = !recurring;
  };
  billingSelect.addEventListener('change', syncBillingUi);
  syncBillingUi();

  const stageSelect = el(
    'select.select',
    {},
    ...stages.map((s) => el('option', { value: s.id, selected: (existing?.stage || store.firstStage()) === s.id }, stageLabel(s, stages)))
  );
  const closeInput = el('input.input', { type: 'date', value: existing?.expectedClose || '' });

  let companyId = existing?.companyId ?? presetCompanyId ?? '';
  const companySelect = el(
    'select.select',
    { onchange: (e) => (companyId = e.target.value) },
    el('option', { value: '' }, t('No company')),
    ...[...store.companies].sort((a, b) => a.name.localeCompare(b.name)).map((c) => el('option', { value: c.id, selected: c.id === companyId }, c.name))
  );

  // Buying committee editor (primary first, up to 8)
  let committee = existing
    ? normalizeDealContacts(existing.contacts, existing.contactId)
    : presetContactId
      ? [{ contactId: presetContactId, role: '' }]
      : [];
  const committeeWrap = el('div.row-list');
  const sortedContacts = [...store.contacts].sort((a, b) => a.name.localeCompare(b.name));

  const renderCommittee = () => {
    const rows = committee.map((entry, idx) => {
      const cSel = el(
        'select.select.select-sm',
        {
          onchange: (e) => {
            entry.contactId = e.target.value;
            if (!companyId && idx === 0) {
              const ct = store.contact(entry.contactId);
              if (ct?.companyId) {
                companyId = ct.companyId;
                companySelect.value = companyId;
              }
            }
          },
        },
        ...sortedContacts.map((c) => el('option', { value: c.id, selected: c.id === entry.contactId }, c.name))
      );
      const rSel = el(
        'select.select.select-sm',
        { onchange: (e) => (entry.role = e.target.value) },
        el('option', { value: '' }, t('No role')),
        ...DEAL_ROLES.map((r) => el('option', { value: r.id, selected: entry.role === r.id }, t(r.label)))
      );
      return el(
        'div.list-row',
        { style: 'padding:6px 8px' },
        cSel,
        rSel,
        el('span.row-meta', {}, idx === 0 ? t('Primary') : ''),
        el(
          'button.icon-btn',
          {
            type: 'button',
            'aria-label': t('Remove contact'),
            onclick: () => {
              committee.splice(idx, 1);
              renderCommittee();
            },
          },
          icon('x', 13)
        )
      );
    });

    const addBtn =
      sortedContacts.length > 0 && committee.length < MAX_DEAL_CONTACTS
        ? el('button.btn.btn-sm', {
            type: 'button',
            onclick: () => {
              const unused = sortedContacts.find((c) => !committee.some((x) => x.contactId === c.id)) || sortedContacts[0];
              committee.push({ contactId: unused.id, role: '' });
              if (!companyId && committee.length === 1 && unused.companyId) {
                companyId = unused.companyId;
                companySelect.value = companyId;
              }
              renderCommittee();
            },
          }, t('+ Add person'))
        : null;

    committeeWrap.replaceChildren(...rows, addBtn);
  };
  renderCommittee();

  const initialConfMode =
    existing?.confidence == null
      ? 'stage'
      : CONFIDENCE_LEVELS.find((l) => l.value === existing.confidence)?.id || 'custom';
  const confSelect = el(
    'select.select',
    {},
    el('option', { value: 'stage', selected: initialConfMode === 'stage' }, t('Use stage weight')),
    ...CONFIDENCE_LEVELS.map((l) => el('option', { value: l.id, selected: initialConfMode === l.id }, `${t(l.label)} (${Math.round(l.value * 100)}%)`)),
    el('option', { value: 'custom', selected: initialConfMode === 'custom' }, t('Custom %'))
  );
  const confPctInput = el('input.input', {
    type: 'number',
    min: 0,
    max: 100,
    value: existing?.confidence != null ? Math.round(existing.confidence * 100) : 50,
  });
  const confPctField = el('div.field', {}, el('label.field-label', {}, t('Custom %')), confPctInput);
  const syncConfUi = () => {
    const custom = confSelect.value === 'custom';
    confPctField.style.display = custom ? '' : 'none';
    confPctInput.disabled = !custom;
  };
  confSelect.addEventListener('change', syncConfUi);
  syncConfUi();

  const tagsCtrl = tagInputControl(existing?.tags || [], store.allTags());
  const notesInput = el('textarea.textarea', { value: existing?.notes || '' });
  const cf = customFieldsInputs(store.customFieldsFor('deals'), existing?.custom);

  const form = el(
    'form',
    {},
    el(
      'div.form-grid',
      {},
      fieldRow(t('Deal name *'), nameInput, true),
      fieldRow(t('Value ({currency}) *', { currency: store.settings.currency }), valueInput),
      fieldRow(t('Billing'), billingSelect),
      termField,
      fieldRow(t('Stage'), stageSelect),
      fieldRow(t('Company'), companySelect),
      fieldRow(t('Expected close'), closeInput),
      el('div.field', {}, el('label.field-label', {}, t('Forecast confidence')), confSelect),
      confPctField,
      el('div.field.full', {}, el('label.field-label', {}, t('People & roles')), committeeWrap),
      el('div.field.full', {}, el('label.field-label', {}, t('Tags')), tagsCtrl.el),
      ...cf.nodes,
      el('div.field.full', {}, el('label.field-label', {}, t('Notes')), notesInput)
    )
  );

  const dlg = openModal({
    title: existing ? t('Edit {name}', { name: existing.name }) : t('New deal'),
    wide: true,
    guardUnsaved: true,
    body: form,
    foot: el(
      'div.btn-group',
      { style: 'display:flex;gap:8px;width:100%;justify-content:space-between' },
      existing
        ? el('button.btn.btn-danger', {
            type: 'button',
            onclick: async () => {
              if (!(await confirmDialog(t('Delete "{name}"?', { name: existing.name }), { confirmLabel: t('Delete'), tone: 'danger' }))) return;
              const entry = await store.deleteDeal(existing.id);
              dlg.close();
              deletedToast(store, entry, t('Deal'));
              if (window.location.hash.includes(existing.id)) window.location.hash = '#/pipeline';
            },
          }, t('Delete'))
        : el('span'),
      el(
        'div.chip-row',
        {},
        el('button.btn', { type: 'button', onclick: () => dlg.requestClose() }, t('Cancel')),
        el('button.btn.btn-primary', { type: 'button', onclick: () => form.requestSubmit() }, t('Save'))
      )
    ),
  });

  wireForm(form, async () => {
    const cleanName = nameInput.value.trim();
    if (!cleanName) {
      toast(t('Deal name is required'), { tone: 'danger' });
      nameInput.focus();
      return;
    }
    let confidence = null;
    if (confSelect.value === 'custom') confidence = Math.max(0, Math.min(100, Number(confPctInput.value) || 0)) / 100;
    else if (confSelect.value !== 'stage') confidence = CONFIDENCE_LEVELS.find((l) => l.id === confSelect.value)?.value ?? null;

    const data = {
      name: cleanName,
      value: Number(valueInput.value) || 0,
      billing: billingSelect.value,
      term: billingSelect.value === 'one-time' ? null : Number(termInput.value) || DEFAULT_TERM,
      stage: stageSelect.value,
      companyId: companyId || null,
      contacts: committee,
      expectedClose: closeInput.value || null,
      confidence,
      tags: tagsCtrl.getTags(),
      custom: cf.readValues(),
      notes: notesInput.value,
    };
    const prevStage = existing?.stage || null;
    const saved = existing ? await store.updateDeal(existing.id, data) : await store.addDeal(data);
    dlg.close();
    if ((!existing || saved.stage !== prevStage) && isTerminalStage(saved.stage)) {
      openCloseReasonForm(store, saved, () => onSaved?.(saved));
    } else if (existing && saved.stage !== prevStage) {
      if (!maybeOfferPlaybook(store, saved, saved.stage)) {
        maybeNudgeNextStep(store, { dealId: saved.id, contactId: saved.contactId, openTaskForm: (tsk, o) => openTaskForm(store, tsk, o) });
      }
      onSaved?.(saved);
    } else {
      onSaved?.(saved);
    }
  });
}

function buildContactSelect(store, selectedId, selectedDealId) {
  const deal = selectedDealId ? store.deal(selectedDealId) : null;
  const dealIds = new Set((deal?.contacts || []).map((x) => x.contactId));
  const sorted = [...store.contacts].sort((a, b) => a.name.localeCompare(b.name));
  const inDeal = sorted.filter((c) => dealIds.has(c.id));
  const rest = sorted.filter((c) => !dealIds.has(c.id));

  const sel = el('select.select', {}, el('option', { value: '' }, t('No lead')));
  if (inDeal.length) {
    const grp = el('optgroup', { label: t('On this deal') }, ...inDeal.map((c) => el('option', { value: c.id, selected: c.id === selectedId }, c.name)));
    sel.appendChild(grp);
  }
  for (const c of rest) {
    sel.appendChild(el('option', { value: c.id, selected: c.id === selectedId }, c.name));
  }
  return sel;
}

export function openActivityForm(store, existing = null, { presetContactId = null, presetDealId = null, presetCompanyId = null, onSaved = null } = {}) {
  const typeSelect = el(
    'select.select',
    {},
    ...ACTIVITY_TYPES.map((tp) => el('option', { value: tp.id, selected: (existing?.type || 'note') === tp.id }, t(tp.label)))
  );
  const dateInput = el('input.input', { type: 'date', required: true, value: existing?.date || todayISO() });
  const summaryInput = el('input.input', { type: 'text', required: true, value: existing?.summary || '', placeholder: t('What happened?') });
  const notesInput = el('textarea.textarea', { value: existing?.notes || '' });

  let dealId = existing?.dealId ?? presetDealId ?? '';
  let contactId = existing?.contactId ?? presetContactId ?? '';
  const dealSelect = el(
    'select.select',
    {},
    el('option', { value: '' }, t('No deal')),
    ...[...store.deals].sort((a, b) => a.name.localeCompare(b.name)).map((d) => el('option', { value: d.id, selected: d.id === dealId }, d.name))
  );
  let contactSelect = buildContactSelect(store, contactId, dealId);
  const contactHolder = el('div.field', {}, el('label.field-label', {}, t('Lead / Contact')), contactSelect);

  dealSelect.addEventListener('change', () => {
    dealId = dealSelect.value;
    contactSelect = buildContactSelect(store, contactId, dealId);
    contactSelect.addEventListener('change', () => (contactId = contactSelect.value));
    contactHolder.replaceChildren(el('label.field-label', {}, t('Lead / Contact')), contactSelect);
  });
  contactSelect.addEventListener('change', () => (contactId = contactSelect.value));

  const form = el(
    'form',
    {},
    el(
      'div.form-grid',
      {},
      fieldRow(t('Type'), typeSelect),
      fieldRow(t('Date *'), dateInput),
      fieldRow(t('Summary *'), summaryInput, true),
      fieldRow(t('Deal'), dealSelect),
      contactHolder,
      fieldRow(t('Notes'), notesInput, true)
    )
  );

  const dlg = openModal({
    title: existing ? t('Edit activity') : t('Log activity'),
    wide: true,
    guardUnsaved: true,
    body: form,
    foot: el(
      'div.btn-group',
      { style: 'display:flex;gap:8px;width:100%;justify-content:space-between' },
      existing
        ? el('button.btn.btn-danger', {
            type: 'button',
            onclick: async () => {
              if (!(await confirmDialog(t('Delete this activity?'), { confirmLabel: t('Delete'), tone: 'danger' }))) return;
              const entry = await store.deleteActivity(existing.id);
              dlg.close();
              deletedToast(store, entry, t('Activity_rec'));
            },
          }, t('Delete'))
        : el('span'),
      el(
        'div.chip-row',
        {},
        el('button.btn', { type: 'button', onclick: () => dlg.requestClose() }, t('Cancel')),
        el('button.btn.btn-primary', { type: 'button', onclick: () => form.requestSubmit() }, t('Save'))
      )
    ),
  });

  wireForm(form, async () => {
    const cleanSummary = summaryInput.value.trim();
    if (!cleanSummary) {
      toast(t('Summary is required'), { tone: 'danger' });
      summaryInput.focus();
      return;
    }
    const data = {
      type: typeSelect.value,
      date: dateInput.value,
      summary: cleanSummary,
      notes: notesInput.value,
      dealId: dealId || null,
      contactId: contactSelect.value || null,
      companyId: existing?.companyId ?? presetCompanyId ?? null,
    };
    const saved = existing ? await store.updateActivity(existing.id, data) : await store.addActivity(data);
    dlg.close();
    if (!existing) {
      maybeNudgeNextStep(store, { dealId: saved.dealId, contactId: saved.contactId, openTaskForm: (tsk, o) => openTaskForm(store, tsk, o) });
    }
    onSaved?.(saved);
  });
}

export function openTaskForm(store, existing = null, { presetContactId = null, presetDealId = null, title = '', onSaved = null } = {}) {
  const titleInput = el('input.input', { type: 'text', required: true, value: existing?.title || title || '', placeholder: t('Follow-up action…') });
  const dueInput = el('input.input', { type: 'date', required: true, value: existing?.due || todayISO() });
  const timeInput = el('input.input', { type: 'time', value: existing?.dueTime || '' });

  const repUnitSel = el(
    'select.select',
    {},
    el('option', { value: '' }, t('No repeat')),
    ...REPEAT_UNITS.map((u) => el('option', { value: u, selected: existing?.repeat?.unit === u }, t('Every N {unit}(s)', { unit: t(u) })))
  );
  const repEveryInp = el('input.input', { type: 'number', min: 1, max: 365, value: existing?.repeat?.every ?? 1 });
  const repEveryField = el('div.field', {}, el('label.field-label', {}, t('Interval')), repEveryInp);
  const syncRep = () => {
    const on = Boolean(repUnitSel.value);
    repEveryField.style.display = on ? '' : 'none';
    repEveryInp.disabled = !on;
  };
  repUnitSel.addEventListener('change', syncRep);
  syncRep();

  let dealId = existing?.dealId ?? presetDealId ?? '';
  let contactId = existing?.contactId ?? presetContactId ?? '';
  const dealSelect = el(
    'select.select',
    {},
    el('option', { value: '' }, t('No deal')),
    ...[...store.deals].sort((a, b) => a.name.localeCompare(b.name)).map((d) => el('option', { value: d.id, selected: d.id === dealId }, d.name))
  );
  let contactSelect = buildContactSelect(store, contactId, dealId);
  const contactHolder = el('div.field', {}, el('label.field-label', {}, t('Lead / Contact')), contactSelect);

  dealSelect.addEventListener('change', () => {
    dealId = dealSelect.value;
    contactSelect = buildContactSelect(store, contactId, dealId);
    contactSelect.addEventListener('change', () => (contactId = contactSelect.value));
    contactHolder.replaceChildren(el('label.field-label', {}, t('Lead / Contact')), contactSelect);
  });
  contactSelect.addEventListener('change', () => (contactId = contactSelect.value));

  const form = el(
    'form',
    {},
    el(
      'div.form-grid',
      {},
      fieldRow(t('Title *'), titleInput, true),
      fieldRow(t('Due date *'), dueInput),
      fieldRow(t('Time (optional)'), timeInput),
      fieldRow(t('Repeat'), repUnitSel),
      repEveryField,
      fieldRow(t('Deal'), dealSelect),
      contactHolder
    )
  );

  const dlg = openModal({
    title: existing ? t('Edit follow-up') : t('New follow-up'),
    wide: true,
    guardUnsaved: true,
    body: form,
    foot: el(
      'div.btn-group',
      { style: 'display:flex;gap:8px;width:100%;justify-content:space-between' },
      el(
        'div.chip-row',
        {},
        existing
          ? el('button.btn.btn-danger', {
              type: 'button',
              onclick: async () => {
                if (!(await confirmDialog(t('Delete follow-up "{name}"?', { name: existing.title }), { confirmLabel: t('Delete'), tone: 'danger' }))) return;
                const entry = await store.deleteTask(existing.id);
                dlg.close();
                deletedToast(store, entry, t('Follow-up'));
              },
            }, t('Delete'))
          : null,
        existing
          ? el('button.btn.btn-sm', { type: 'button', onclick: () => exportTaskIcs(existing, store) }, '.ics')
          : null,
        store.services?.isAvailable()
          ? el('button.btn.btn-sm', {
              type: 'button',
              onclick: () => {
                const draft = {
                  id: existing?.id || null,
                  title: titleInput.value.trim() || t('Follow up'),
                  due: dueInput.value || todayISO(),
                  dueTime: timeInput.value || '',
                  dealId: dealId || null,
                  contactId: contactSelect.value || null,
                };
                store.services.scheduleFollowUpInCalendar(draft).catch((err) => toast(err?.message || t('Could not open Calendar tab'), { tone: 'danger' }));
              },
            }, t('Google Calendar'))
          : null
      ),
      el(
        'div.chip-row',
        {},
        el('button.btn', { type: 'button', onclick: () => dlg.requestClose() }, t('Cancel')),
        el('button.btn.btn-primary', { type: 'button', onclick: () => form.requestSubmit() }, t('Save'))
      )
    ),
  });

  wireForm(form, async () => {
    const cleanTitle = titleInput.value.trim();
    if (!cleanTitle) {
      toast(t('Follow-up title is required'), { tone: 'danger' });
      titleInput.focus();
      return;
    }
    const repeat = repUnitSel.value ? { every: Math.max(1, Number(repEveryInp.value) || 1), unit: repUnitSel.value } : null;
    const data = {
      title: cleanTitle,
      due: dueInput.value,
      dueTime: timeInput.value || '',
      repeat,
      dealId: dealId || null,
      contactId: contactSelect.value || null,
    };
    const saved = existing ? await store.updateTask(existing.id, data) : await store.addTask(data);
    dlg.close();
    onSaved?.(saved);
  });
}
