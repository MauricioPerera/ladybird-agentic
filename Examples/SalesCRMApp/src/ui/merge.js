/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from '../lib/i18n.js';
import { deletedToast } from './bits.js';
import { el } from './dom.js';
import { openModal } from './modal.js';
import { toast } from './toast.js';

export function openMergeForm(store, kind, recA, recB) {
  let survivorId = recA.id;
  const picks = {};

  const fields =
    kind === 'contacts'
      ? [
          { key: 'name', label: t('Name') },
          { key: 'title', label: t('Title') },
          { key: 'email', label: t('Email') },
          { key: 'phone', label: t('Phone') },
          { key: 'companyId', label: t('Company'), format: (id) => store.company(id)?.name || '' },
          { key: 'source', label: t('Source') },
          { key: 'status', label: t('Status') },
          { key: 'notes', label: t('Notes') },
        ]
      : [
          { key: 'name', label: t('Name') },
          { key: 'industry', label: t('Industry') },
          { key: 'website', label: t('Website') },
          { key: 'size', label: t('Size') },
          { key: 'notes', label: t('Notes') },
        ];

  const body = el('div', { style: 'display:flex;flex-direction:column;gap:12px' });

  const render = () => {
    const survivor = survivorId === recA.id ? recA : recB;
    const other = survivorId === recA.id ? recB : recA;

    const chooser = el(
      'div.segmented',
      {},
      el('button', {
        type: 'button',
        class: `seg-btn${survivorId === recA.id ? ' active' : ''}`,
        'aria-pressed': String(survivorId === recA.id),
        onclick: () => {
          survivorId = recA.id;
          for (const k of Object.keys(picks)) delete picks[k];
          render();
        },
      }, t('Keep {name}', { name: recA.name })),
      el('button', {
        type: 'button',
        class: `seg-btn${survivorId === recB.id ? ' active' : ''}`,
        'aria-pressed': String(survivorId === recB.id),
        onclick: () => {
          survivorId = recB.id;
          for (const k of Object.keys(picks)) delete picks[k];
          render();
        },
      }, t('Keep {name}', { name: recB.name }))
    );

    const diffRows = [];
    for (const f of fields) {
      const va = String(survivor[f.key] ?? '').trim();
      const vb = String(other[f.key] ?? '').trim();
      if (!va && vb) {
        picks[f.key] = vb;
        continue;
      }
      if (!vb || va === vb) continue;
      const chosen = picks[f.key] ?? va;
      const dispA = f.format ? f.format(va) || va : va;
      const dispB = f.format ? f.format(vb) || vb : vb;
      diffRows.push(
        el(
          'div.card',
          { style: 'padding:10px' },
          el('div.field-label', {}, f.label),
          el(
            'div.chip-row',
            { style: 'margin-top:6px' },
            el('button', {
              type: 'button',
              class: `filter-chip${chosen === va ? ' active' : ''}`,
              'aria-pressed': String(chosen === va),
              onclick: () => {
                picks[f.key] = va;
                render();
              },
            }, dispA),
            el('button', {
              type: 'button',
              class: `filter-chip${chosen === vb ? ' active' : ''}`,
              'aria-pressed': String(chosen === vb),
              onclick: () => {
                picks[f.key] = vb;
                render();
              },
            }, dispB)
          )
        )
      );
    }

    body.replaceChildren(
      el('div.field-label', {}, t('Primary record to keep')),
      chooser,
      diffRows.length ? el('div.field-label', {}, t('Where they differ')) : el('p.muted', {}, t('All differing fields auto-fill cleanly without conflicts.')),
      ...diffRows
    );
  };
  render();

  const dlg = openModal({
    title: kind === 'contacts' ? t('Merge duplicate leads') : t('Merge duplicate companies'),
    wide: true,
    body,
    foot: el(
      'div.btn-group',
      { style: 'display:flex;gap:8px' },
      el('button.btn', { type: 'button', onclick: () => dlg.close() }, t('Cancel')),
      el('button.btn.btn-primary', {
        type: 'button',
        onclick: async () => {
          const mergedId = survivorId === recA.id ? recB.id : recA.id;
          try {
            if (kind === 'contacts') await store.mergeContacts(survivorId, mergedId, picks);
            else await store.mergeCompanies(survivorId, mergedId, picks);
            dlg.close();
            const entry = store.trash[0];
            deletedToast(store, entry, t('Duplicate'));
          } catch (err) {
            toast(err?.message || t('Merge failed'), { tone: 'danger' });
          }
        },
      }, t('Merge records'))
    ),
  });
}

export function openDuplicatesReview(store) {
  const wrap = el('div', { style: 'display:flex;flex-direction:column;gap:14px' });

  const render = () => {
    const dupes = store.duplicates();
    if (dupes.contacts.length === 0 && dupes.companies.length === 0) {
      wrap.replaceChildren(el('p.muted', {}, t('No duplicate leads or companies detected.')));
      return;
    }

    const section = (title, kind, pairs) =>
      pairs.length
        ? el(
            'div',
            { style: 'display:flex;flex-direction:column;gap:8px' },
            el('h3.card-title', {}, title),
            ...pairs.map((p) =>
              el(
                'div.list-row',
                {},
                el(
                  'div',
                  { style: 'min-width:0;flex:1' },
                  el('div.row-title', {}, `${p.a.name} ↔ ${p.b.name}`),
                  el('div.chip-row', { style: 'margin-top:4px' }, ...p.reasons.map((r) => el('span.pill.pill-warn', {}, t(r))))
                ),
                el('button.btn.btn-sm.btn-primary', {
                  type: 'button',
                  onclick: () => openMergeForm(store, kind, p.a, p.b),
                }, t('Merge…'))
              )
            )
          )
        : null;

    wrap.replaceChildren(
      section(t('Duplicate leads & contacts'), 'contacts', dupes.contacts),
      section(t('Duplicate companies'), 'companies', dupes.companies)
    );
  };

  render();
  const onChange = () => {
    if (!wrap.isConnected) {
      store.removeEventListener('change', onChange);
      return;
    }
    render();
  };
  store.addEventListener('change', onChange);

  openModal({
    title: t('Possible duplicates'),
    wide: true,
    body: wrap,
    onClose: () => store.removeEventListener('change', onChange),
  });
}
