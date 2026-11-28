/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { quietSince } from '../lib/attention.js';
import { leadSourceLabel, t } from '../lib/i18n.js';
import { contractValue } from '../lib/recurring.js';
import { CONTACT_STATUSES } from '../lib/statuses.js';
import { buildMailto, buildTel } from '../lib/templates.js';
import { formatMoney, fuzzyIncludes, todayISO } from '../lib/util.js';
import { attachmentsSection } from '../ui/attachments.js';
import { activityRow, avatarEl, deletedToast, duePill, emptyState, lastTouchChip, pageHead, stageBadge, statusPill, tagChips } from '../ui/bits.js';
import { bulkBar, confirmBulkDelete, createRowSelection, openBulkTagSheet, pickSheet, selectableRow } from '../ui/bulk.js';
import { exportSet } from '../ui/csv.js';
import { customFieldsDetail, customFilterControls } from '../ui/custom-fields.js';
import { el, keepingFocus } from '../ui/dom.js';
import { openEmailFlow } from '../ui/email.js';
import { openActivityForm, openContactForm, openDealForm, openTaskForm } from '../ui/forms.js';
import { confirmDialog } from '../ui/modal.js';
import { openDuplicatesReview } from '../ui/merge.js';
import { openPlaybookPicker } from '../ui/playbooks.js';
import { tagFilterControl } from '../ui/tag-input.js';
import { initialViewFilters, viewChips } from '../ui/views.js';

export function leadsView(root, { store, params }) {
  const initial = initialViewFilters('contacts', store, params.get('view'));
  let filters = initial || { query: '', status: 'all', tags: [], fields: {} };
  const selection = createRowSelection(() => keepingFocus(render));

  const renderDetail = (contactId) => {
    const contact = store.contact(contactId);
    if (!contact) {
      root.replaceChildren(
        pageHead(t('Lead not found'), { actions: [el('a.btn', { href: '#/leads' }, t('Back to leads'))] }),
        emptyState('users', t('This contact no longer exists'))
      );
      return;
    }

    const company = store.company(contact.companyId);
    const deals = store.dealsFor(contact.id);
    const tasks = store.tasksFor('contact', contact.id);
    const acts = store.activitiesFor('contact', contact.id);
    const stages = store.resolvedStages();
    const lastTouch = store.lastContactedAt(contact.id);
    const cfCard = customFieldsDetail(store.customFieldsFor('contacts'), contact.custom);

    root.replaceChildren(
      pageHead(contact.name, {
        subtitle: [contact.title, company?.name].filter(Boolean).join(' · ') || t('Independent contact'),
        actions: [
          el('a.btn', { href: '#/leads' }, t('All leads')),
          contact.email ? el('button.btn', { type: 'button', onclick: () => openEmailFlow(store, { contact, company }) }, t('Email…')) : null,
          store.services?.isAvailable() && (contact.email || contact.name)
            ? el('button.btn', { type: 'button', onclick: () => store.services.searchContactInGmail(contact.email || contact.name).catch(() => {}) }, t('Gmail history'))
            : null,
          el('button.btn', { type: 'button', onclick: () => openContactForm(store, contact) }, t('Edit')),
          el('button.btn.btn-danger', {
            type: 'button',
            onclick: async () => {
              if (!(await confirmDialog(t('Delete "{name}"?', { name: contact.name }), { confirmLabel: t('Delete'), tone: 'danger' }))) return;
              const entry = await store.deleteContact(contact.id);
              deletedToast(store, entry, t('Lead'));
              window.location.hash = '#/leads';
            },
          }, t('Delete')),
        ],
      }),
      el(
        'div.chip-row',
        { style: 'margin-bottom:14px' },
        statusPill(contact.status),
        company ? el('a.pill', { href: `#/companies?id=${company.id}` }, company.name) : null,
        contact.source ? el('span.pill', {}, leadSourceLabel(contact.source)) : null,
        lastTouchChip(lastTouch, contact.createdAt),
        tagChips(contact.tags)
      ),
      el(
        'div.detail-grid',
        {},
        el(
          'div',
          { style: 'display:flex;flex-direction:column;gap:14px' },
          el(
            'div.card',
            {},
            el('h3.card-title', {}, t('Contact info')),
            el(
              'div.row-list',
              { style: 'margin-top:8px' },
              contact.email ? el('a.list-row', { href: buildMailto(contact.email) }, el('span.row-meta', {}, t('Email')), el('span.row-title', {}, contact.email)) : null,
              contact.phone ? el('a.list-row', { href: buildTel(contact.phone) }, el('span.row-meta', {}, t('Phone')), el('span.row-title', {}, contact.phone)) : null,
              contact.notes ? el('p', { style: 'white-space:pre-wrap;margin:6px 0 0' }, contact.notes) : el('span.muted', {}, t('No notes yet.'))
            )
          ),
          cfCard,
          el(
            'div.card',
            {},
            el('div.card-head', {}, el('h3.card-title', {}, t('Deals')), el('button.btn.btn-sm', { type: 'button', onclick: () => openDealForm(store, null, { presetContactId: contact.id, presetCompanyId: contact.companyId }) }, t('+ Add'))),
            deals.length
              ? el('div.row-list', {}, ...deals.map((d) =>
                  el(
                    'div.list-row',
                    {},
                    el('a.row-title', { href: `#/pipeline?id=${d.id}` }, d.name),
                    el('div.chip-row', {}, stageBadge(d.stage, stages), el('strong', {}, formatMoney(contractValue(d), store.settings.currency)))
                  )
                ))
              : el('div.muted', {}, t('No deals linked.'))
          ),
          el(
            'div.card',
            {},
            el('div.card-head', {}, el('h3.card-title', {}, t('History')), el('button.btn.btn-sm', { type: 'button', onclick: () => openActivityForm(store, null, { presetContactId: contact.id, presetCompanyId: contact.companyId }) }, t('+ Log'))),
            acts.length ? el('div.row-list', {}, ...acts.map((a) => activityRow(a, store, (x) => openActivityForm(store, x)))) : el('div.muted', {}, t('No logged activity.'))
          )
        ),
        el(
          'div',
          { style: 'display:flex;flex-direction:column;gap:14px' },
          el(
            'div.card',
            {},
            el(
              'div.card-head',
              {},
              el('h3.card-title', {}, t('Follow-ups')),
              el(
                'div.chip-row',
                {},
                el('button.btn.btn-sm', { type: 'button', onclick: () => openPlaybookPicker(store, { contactId: contact.id }) }, t('Playbook')),
                el('button.btn.btn-sm', { type: 'button', onclick: () => openTaskForm(store, null, { presetContactId: contact.id }) }, t('+ Add'))
              )
            ),
            tasks.length
              ? el('div.row-list', {}, ...tasks.map((tsk) =>
                  el(
                    'div.list-row',
                    {},
                    el('div.row-main', {}, el('input', { type: 'checkbox', 'aria-label': t('Toggle {name}', { name: tsk.title }), checked: tsk.done, onchange: () => store.toggleTask(tsk.id) }), el('a.row-title', { href: `#/followups?task=${tsk.id}` }, tsk.title)),
                    tsk.done ? el('span.pill', {}, t('Done')) : duePill(tsk.due, tsk.dueTime)
                  )
                ))
              : el('div.muted', {}, t('No follow-ups.'))
          ),
          attachmentsSection(store, 'contact', contact.id)
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

    const today = todayISO();
    const coldDays = store.settings.coldDays;
    const dupes = store.duplicates();

    let rows = store.contacts.filter((c) => {
      if (filters.status === 'cold') {
        if (c.status !== 'lead' && c.status !== 'active') return false;
        const q = quietSince(store.lastContactedAt(c.id), c.createdAt, today);
        if (!coldDays || q.days < coldDays) return false;
      } else if (filters.status !== 'all' && c.status !== filters.status) {
        return false;
      }
      if (filters.tags.length && !filters.tags.every((tg) => (c.tags || []).some((x) => x.toLowerCase() === tg.toLowerCase()))) return false;
      for (const [k, v] of Object.entries(filters.fields || {})) {
        if (v === false) {
          if (Boolean(c.custom?.[k])) return false;
        } else if (c.custom?.[k] !== v) {
          return false;
        }
      }
      if (filters.query) {
        const comp = store.company(c.companyId)?.name || '';
        if (!fuzzyIncludes(`${c.name} ${c.email} ${c.title} ${comp}`, filters.query)) return false;
      }
      return true;
    });

    if (filters.status === 'cold') {
      rows.sort((a, b) => {
        const qa = quietSince(store.lastContactedAt(a.id), a.createdAt, today).days;
        const qb = quietSince(store.lastContactedAt(b.id), b.createdAt, today).days;
        return qb - qa || a.name.localeCompare(b.name);
      });
    } else {
      rows.sort((a, b) => a.name.localeCompare(b.name));
    }

    const visibleIds = rows.map((c) => c.id);
    selection.prune(visibleIds);

    const toolbar = el(
      'div.filter-toolbar',
      {},
      viewChips('contacts', store, filters, (next) => {
        filters = next;
        render();
      }),
      el(
        'div.filter-line',
        {},
        el('input.input.input-sm', {
          type: 'search',
          placeholder: t('Search name, email, company…'),
          value: filters.query,
          'data-focus-key': 'leads-search',
          style: 'min-width:210px',
          oninput: (e) => {
            filters.query = e.target.value;
            keepingFocus(render);
          },
        }),
        tagFilterControl(store.allTags(), filters.tags, (tags) => {
          filters.tags = tags;
          render();
        }),
        customFilterControls(store.customFieldsFor('contacts'), filters.fields, (fields) => {
          filters.fields = fields;
          render();
        })
      ),
      el(
        'div.chip-row',
        {},
        ...[{ id: 'all', label: 'All' }, ...CONTACT_STATUSES, { id: 'cold', label: 'Going cold' }].map((st) =>
          el(
            'button',
            {
              type: 'button',
              class: `filter-chip${filters.status === st.id ? ' active' : ''}`,
              onclick: () => {
                filters.status = st.id;
                render();
              },
            },
            t(st.label)
          )
        )
      )
    );

    const dupeHint =
      filters.status === 'all' && !filters.query && dupes.contacts.length > 0
        ? el(
            'div.dupe-hint',
            {},
            el('span', {}, t('{count} possible duplicate lead pair(s) detected', { count: dupes.contacts.length })),
            el('button.btn.btn-sm', { type: 'button', onclick: () => openDuplicatesReview(store) }, t('Review'))
          )
        : null;

    const bar = bulkBar(selection, visibleIds, [
      el('button.btn.btn-sm', {
        type: 'button',
        onclick: () =>
          pickSheet(
            t('Change status'),
            CONTACT_STATUSES.map((s) => ({ id: s.id, label: t(s.label) })),
            async (status) => {
              await store.bulkUpdateContacts(selection.ids(), { status });
              selection.clear();
            }
          ),
      }, t('Status')),
      el('button.btn.btn-sm', {
        type: 'button',
        onclick: () =>
          pickSheet(
            t('Assign company'),
            [{ id: '', label: t('No company') }, ...store.companies.map((co) => ({ id: co.id, label: co.name }))],
            async (companyId) => {
              await store.bulkUpdateContacts(selection.ids(), { companyId: companyId || null });
              selection.clear();
            }
          ),
      }, t('Company')),
      el('button.btn.btn-sm', { type: 'button', onclick: () => openBulkTagSheet(store, 'contacts', selection.ids(), () => selection.clear()) }, t('Tag')),
      el('button.btn.btn-sm', { type: 'button', onclick: () => exportSet('contacts', store, rows.filter((c) => selection.has(c.id))) }, t('Export CSV')),
      el('button.btn.btn-sm.btn-danger', {
        type: 'button',
        onclick: async () => {
          if (!(await confirmBulkDelete(selection.count(), 'leads'))) return;
          await store.bulkDeleteContacts(selection.ids());
          selection.clear();
        },
      }, t('Delete')),
    ]);

    const listEl = rows.length
      ? el(
          'div.row-list',
          {},
          ...rows.map((c) => {
            const comp = store.company(c.companyId);
            const openDealsCount = store.dealsFor(c.id).filter((d) => d.stage !== 'won' && d.stage !== 'lost').length;
            const lastAt = store.lastContactedAt(c.id);
            const q = quietSince(lastAt, c.createdAt, today);
            const isCold = (c.status === 'lead' || c.status === 'active') && coldDays > 0 && q.days >= coldDays;

            return selectableRow(
              selection,
              c.id,
              visibleIds,
              el(
                'a.list-row',
                { href: `#/leads?id=${c.id}` },
                el(
                  'div.row-main',
                  {},
                  avatarEl(c.name),
                  el(
                    'div',
                    { style: 'min-width:0;flex:1' },
                    el('div.row-title.truncate', {}, c.name),
                    el('div.row-meta.truncate', {}, [c.title, comp?.name, c.email].filter(Boolean).join(' · ') || t('No details'))
                  )
                ),
                el(
                  'div.chip-row',
                  {},
                  openDealsCount ? el('span.pill', {}, t('{count} deal(s)', { count: openDealsCount })) : null,
                  el('span', { class: `pill${isCold ? ' pill-warn' : ''}` }, lastAt ? t('{days}d ago', { days: q.days }) : t('Never')),
                  statusPill(c.status)
                )
              )
            );
          })
        )
      : emptyState('users', t('No matching leads'));

    root.replaceChildren(
      pageHead(t('Leads & Contacts'), {
        subtitle: t('{shown} of {total} contact(s)', { shown: rows.length, total: store.contacts.length }),
        actions: [el('button.btn.btn-primary', { type: 'button', onclick: () => openContactForm(store) }, t('+ New lead'))],
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
