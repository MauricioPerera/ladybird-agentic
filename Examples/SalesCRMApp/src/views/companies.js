/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { isOpen } from '../lib/forecast.js';
import { t } from '../lib/i18n.js';
import { contractValue } from '../lib/recurring.js';
import { formatMoney, fuzzyIncludes } from '../lib/util.js';
import { attachmentsSection } from '../ui/attachments.js';
import { activityRow, avatarEl, deletedToast, emptyState, pageHead, stageBadge, statusPill, tagChips } from '../ui/bits.js';
import { bulkBar, confirmBulkDelete, createRowSelection, openBulkTagSheet, selectableRow } from '../ui/bulk.js';
import { exportSet } from '../ui/csv.js';
import { customFieldsDetail, customFilterControls } from '../ui/custom-fields.js';
import { el, keepingFocus } from '../ui/dom.js';
import { openActivityForm, openCompanyForm, openContactForm, openDealForm } from '../ui/forms.js';
import { icon } from '../ui/icons.js';
import { confirmDialog } from '../ui/modal.js';
import { openDuplicatesReview } from '../ui/merge.js';
import { tagFilterControl } from '../ui/tag-input.js';
import { initialViewFilters, viewChips } from '../ui/views.js';

export function companiesView(root, { store, params }) {
  const initial = initialViewFilters('companies', store, params.get('view'));
  let filters = initial || { query: '', tags: [], fields: {} };
  const selection = createRowSelection(() => keepingFocus(render));

  const renderDetail = (companyId) => {
    const company = store.company(companyId);
    if (!company) {
      root.replaceChildren(
        pageHead(t('Company not found'), { actions: [el('a.btn', { href: '#/companies' }, t('Back to companies'))] }),
        emptyState('building', t('This company no longer exists'))
      );
      return;
    }

    const contacts = store.contactsFor(company.id);
    const deals = store.dealsForCompany(company.id);
    const openVal = deals.filter(isOpen).reduce((acc, d) => acc + contractValue(d), 0);
    const acts = store.activitiesFor('company', company.id);
    const stages = store.resolvedStages();
    const cur = store.settings.currency;
    const cfCard = customFieldsDetail(store.customFieldsFor('companies'), company.custom);
    const siteHref = company.website ? (/^https?:\/\//i.test(company.website) ? company.website : `https://${company.website}`) : '';

    root.replaceChildren(
      pageHead(company.name, {
        subtitle: [company.industry, company.size].filter(Boolean).join(' · ') || t('Company account'),
        actions: [
          el('a.btn', { href: '#/companies' }, t('All companies')),
          el('button.btn', { type: 'button', onclick: () => openCompanyForm(store, company) }, t('Edit')),
          el('button.btn.btn-danger', {
            type: 'button',
            onclick: async () => {
              if (!(await confirmDialog(t('Delete "{name}"?', { name: company.name }), { confirmLabel: t('Delete'), tone: 'danger' }))) return;
              const entry = await store.deleteCompany(company.id);
              deletedToast(store, entry, t('Company'));
              window.location.hash = '#/companies';
            },
          }, t('Delete')),
        ],
      }),
      el(
        'div.chip-row',
        { style: 'margin-bottom:14px' },
        company.website ? el('a.pill', { href: siteHref, target: '_blank', rel: 'noopener noreferrer' }, company.website) : null,
        openVal > 0 ? el('span.pill.pill-accent', {}, t('{amount} open pipeline', { amount: formatMoney(openVal, cur) })) : null,
        tagChips(company.tags)
      ),
      el(
        'div.detail-grid',
        {},
        el(
          'div',
          { style: 'display:flex;flex-direction:column;gap:14px' },
          cfCard,
          company.notes ? el('div.card', {}, el('h3.card-title', {}, t('Notes')), el('p', { style: 'white-space:pre-wrap;margin:8px 0 0' }, company.notes)) : null,
          el(
            'div.card',
            {},
            el('div.card-head', {}, el('h3.card-title', {}, t('Deals')), el('button.btn.btn-sm', { type: 'button', onclick: () => openDealForm(store, null, { presetCompanyId: company.id }) }, t('+ Add'))),
            deals.length
              ? el('div.row-list', {}, ...deals.map((d) =>
                  el('div.list-row', {}, el('a.row-title', { href: `#/pipeline?id=${d.id}` }, d.name), el('div.chip-row', {}, stageBadge(d.stage, stages), el('strong', {}, formatMoney(contractValue(d), cur))))
                ))
              : el('div.muted', {}, t('No deals yet.'))
          ),
          el(
            'div.card',
            {},
            el('div.card-head', {}, el('h3.card-title', {}, t('History')), el('button.btn.btn-sm', { type: 'button', onclick: () => openActivityForm(store, null, { presetCompanyId: company.id }) }, t('+ Log'))),
            acts.length ? el('div.row-list', {}, ...acts.map((a) => activityRow(a, store, (x) => openActivityForm(store, x)))) : el('div.muted', {}, t('No logged activity.'))
          )
        ),
        el(
          'div',
          { style: 'display:flex;flex-direction:column;gap:14px' },
          el(
            'div.card',
            {},
            el('div.card-head', {}, el('h3.card-title', {}, t('People')), el('button.btn.btn-sm', { type: 'button', onclick: () => openContactForm(store, null, { presetCompanyId: company.id }) }, t('+ Add'))),
            contacts.length
              ? el('div.row-list', {}, ...contacts.map((c) =>
                  el('a.list-row', { href: `#/leads?id=${c.id}` }, el('div.row-main', {}, avatarEl(c.name), el('div', {}, el('div.row-title', {}, c.name), el('div.row-meta', {}, c.title || c.email || ''))), statusPill(c.status))
                ))
              : el('div.muted', {}, t('No contacts linked.'))
          ),
          attachmentsSection(store, 'company', company.id)
        )
      )
    );
  };

  const render = () => {
    const id = params.get('id');
    if (id) {
      renderDetail(id);
      return;
    }

    const dupes = store.duplicates();
    const cur = store.settings.currency;
    const rows = store.companies
      .filter((c) => {
        if (filters.tags.length && !filters.tags.every((tg) => (c.tags || []).some((x) => x.toLowerCase() === tg.toLowerCase()))) return false;
        for (const [k, v] of Object.entries(filters.fields || {})) {
          if (v === false) {
            if (Boolean(c.custom?.[k])) return false;
          } else if (c.custom?.[k] !== v) {
            return false;
          }
        }
        if (filters.query && !fuzzyIncludes(`${c.name} ${c.industry}`, filters.query)) return false;
        return true;
      })
      .sort((a, b) => a.name.localeCompare(b.name));

    const visibleIds = rows.map((c) => c.id);
    selection.prune(visibleIds);

    const extraFilters = el(
      'div.chip-row',
      {},
      tagFilterControl(store.allTags(), filters.tags, (tags) => {
        filters.tags = tags;
        render();
      }),
      customFilterControls(store.customFieldsFor('companies'), filters.fields, (fields) => {
        filters.fields = fields;
        render();
      })
    );

    const toolbar = el(
      'div.filter-toolbar',
      {},
      viewChips('companies', store, filters, (next) => {
        filters = next;
        render();
      }),
      el(
        'div.filter-line',
        {},
        el('input.input.input-sm', {
          type: 'search',
          placeholder: t('Search company or industry…'),
          value: filters.query,
          'data-focus-key': 'companies-search',
          style: 'min-width:220px',
          oninput: (e) => {
            filters.query = e.target.value;
            keepingFocus(render);
          },
        }),
        extraFilters.childElementCount ? extraFilters : null
      )
    );

    const dupeHint =
      !filters.query && dupes.companies.length > 0
        ? el(
            'div.dupe-hint',
            {},
            el('span', {}, t('{count} possible duplicate company pair(s)', { count: dupes.companies.length })),
            el('button.btn.btn-sm', { type: 'button', onclick: () => openDuplicatesReview(store) }, t('Review'))
          )
        : null;

    const bar = bulkBar(selection, visibleIds, [
      el('button.btn.btn-sm', { type: 'button', onclick: () => openBulkTagSheet(store, 'companies', selection.ids(), () => selection.clear()) }, t('Tag')),
      el('button.btn.btn-sm', { type: 'button', onclick: () => exportSet('companies', store, rows.filter((c) => selection.has(c.id))) }, t('Export CSV')),
      el('button.btn.btn-sm.btn-danger', {
        type: 'button',
        onclick: async () => {
          if (!(await confirmBulkDelete(selection.count(), 'companies'))) return;
          await store.bulkDeleteCompanies(selection.ids());
          selection.clear();
        },
      }, t('Delete')),
    ]);

    const listEl = rows.length
      ? el(
          'div.row-list',
          {},
          ...rows.map((c) => {
            const people = store.contactsFor(c.id).length;
            const openVal = store.dealsForCompany(c.id).filter(isOpen).reduce((acc, d) => acc + contractValue(d), 0);
            return selectableRow(
              selection,
              c.id,
              visibleIds,
              el(
                'a.list-row',
                { href: `#/companies?id=${c.id}` },
                el(
                  'div.row-main',
                  {},
                  el('span', {}, icon('building', 16)),
                  el('div', { style: 'min-width:0;flex:1' }, el('div.row-title.truncate', {}, c.name), el('div.row-meta.truncate', {}, `${c.industry || t('No industry')} · ${t('{count} people', { count: people })}`))
                ),
                openVal > 0 ? el('strong', {}, formatMoney(openVal, cur)) : el('span.row-meta', {}, '—')
              )
            );
          })
        )
      : emptyState('building', t('No matching companies'));

    root.replaceChildren(
      pageHead(t('Companies'), {
        subtitle: t('{shown} of {total} account(s)', { shown: rows.length, total: store.companies.length }),
        actions: [el('button.btn.btn-primary', { type: 'button', onclick: () => openCompanyForm(store) }, t('+ New company'))],
      }),
      toolbar,
      dupeHint,
      bar,
      listEl
    );
  };

  render();
  const onChange = () => keepingFocus(render);
  store.addEventListener('change', onChange);
  return () => store.removeEventListener('change', onChange);
}
