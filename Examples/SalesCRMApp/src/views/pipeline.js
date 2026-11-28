/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { dealContactIds, hasDecisionMaker, roleInfo } from '../lib/deal-contacts.js';
import {
  CLOSE_WINDOWS,
  daysInStage,
  isOpen,
  isStalled,
  matchesCloseWindow,
  probabilityOf,
  stageInfo,
  weightedValue,
} from '../lib/forecast.js';
import { reasonLabel, stageLabel, t } from '../lib/i18n.js';
import { billingSummary, contractValue, isRecurring, rateText } from '../lib/recurring.js';
import { compactMoney, formatDate, formatMoney, fuzzyIncludes } from '../lib/util.js';
import { DEFAULT_DEAL_SORT } from '../lib/views.js';
import { attachmentsSection } from '../ui/attachments.js';
import { activityRow, deletedToast, duePill, emptyState, lastTouchChip, pageHead, stageBadge, tagChips } from '../ui/bits.js';
import { bulkBar, confirmBulkDelete, createRowSelection, openBulkTagSheet, pickSheet, selectBox } from '../ui/bulk.js';
import { openCloseReasonForm } from '../ui/close-reason.js';
import { exportSet } from '../ui/csv.js';
import { customFieldsDetail, customFilterControls } from '../ui/custom-fields.js';
import { el, keepingFocus } from '../ui/dom.js';
import { openEmailFlow } from '../ui/email.js';
import { openActivityForm, openDealForm, openTaskForm } from '../ui/forms.js';
import { icon } from '../ui/icons.js';
import { confirmDialog } from '../ui/modal.js';
import { maybeNudgeNextStep } from '../ui/nudge.js';
import { maybeOfferPlaybook, openPlaybookPicker } from '../ui/playbooks.js';
import { tagFilterControl } from '../ui/tag-input.js';
import { toast } from '../ui/toast.js';
import { initialViewFilters, viewChips } from '../ui/views.js';

export function pipelineView(root, { store, params }) {
  const viewId = params.get('view');
  const initial = initialViewFilters('deals', store, viewId);
  let filters = initial || { query: '', stage: 'open', closeWindow: 'any', tags: [], fields: {}, sort: null };
  const expanded = new Set();
  const selection = createRowSelection(() => keepingFocus(render));

  const moveDealTo = async (deal, nextStage) => {
    if (deal.stage === nextStage) return;
    const prevStage = deal.stage;
    const stages = store.resolvedStages();
    const label = stageLabel(stageInfo(nextStage, stages), stages);
    const moved = await store.moveDeal(deal.id, nextStage);
    if (!moved) return;

    if (nextStage === 'won' || nextStage === 'lost') {
      openCloseReasonForm(store, moved, () => {
        toast(t('Marked {stage}: {name}', { stage: label, name: moved.name }), {
          duration: 10000,
          action: t('Undo'),
          onAction: () => store.moveDeal(moved.id, prevStage),
        });
      });
    } else if (!maybeOfferPlaybook(store, moved, nextStage)) {
      if (!maybeNudgeNextStep(store, { dealId: moved.id, contactId: moved.contactId, openTaskForm: (tsk, o) => openTaskForm(store, tsk, o) })) {
        toast(t('Moved to {stage}', { stage: label }), {
          duration: 10000,
          action: t('Undo'),
          onAction: () => store.moveDeal(moved.id, prevStage),
        });
      }
    }
  };

  const openMoveSheet = (deal) => {
    const stages = store.resolvedStages();
    pickSheet(
      t('Move "{name}"', { name: deal.name }),
      stages.map((s) => ({ id: s.id, label: stageLabel(s, stages), meta: `${Math.round(s.probability * 100)}%` })),
      (stageId) => moveDealTo(deal, stageId)
    );
  };

  const renderDetail = (dealId) => {
    const deal = store.deal(dealId);
    if (!deal) {
      root.replaceChildren(
        pageHead(t('Deal not found'), { actions: [el('a.btn', { href: '#/pipeline' }, t('Back to pipeline'))] }),
        emptyState('pipeline', t('This deal no longer exists'))
      );
      return;
    }

    const stages = store.resolvedStages();
    const cur = store.settings.currency;
    const company = store.company(deal.companyId);
    const primaryContact = store.contact(deal.contactId);
    const prob = probabilityOf(deal, store.settings.stageProbabilities, stages);
    const wVal = weightedValue(deal, store.settings.stageProbabilities, stages);
    const lastTouch = store.lastTouchedDeal(deal);
    const acts = store.dealActivities(deal);
    const tasks = store.tasksFor('deal', deal.id);
    const cfCard = customFieldsDetail(store.customFieldsFor('deals'), deal.custom);

    const stepper = el(
      'div.stage-stepper',
      {},
      ...stages.map((s) =>
        el(
          'button',
          {
            type: 'button',
            class: `step-btn${deal.stage === s.id ? ' active' : ''}`,
            onclick: () => moveDealTo(deal, s.id),
          },
          stageLabel(s, stages)
        )
      )
    );

    const peopleRows = (deal.contacts || []).map((entry, idx) => {
      const c = store.contact(entry.contactId);
      if (!c) return null;
      const role = roleInfo(entry.role);
      return el(
        'div.list-row',
        {},
        el('a.row-title', { href: `#/leads?id=${c.id}` }, c.name),
        el('div.chip-row', {}, role ? el('span.pill', {}, t(role.label)) : null, idx === 0 ? el('span.pill.pill-accent', {}, t('Primary')) : null)
      );
    }).filter(Boolean);

    const historyRows = [...(deal.stageHistory || [])].reverse().map((h) =>
      el('div.list-row', {}, stageBadge(h.stage, stages), el('span.row-meta', {}, formatDate(new Date(h.at).toISOString().slice(0, 10), { year: 'always' })))
    );

    const docsActions = store.services?.isAvailable()
      ? [
          el('button.btn', {
            type: 'button',
            onclick: () => store.services.scheduleFollowUpInCalendar({ title: deal.name, due: deal.expectedClose || '', dealId: deal.id, contactId: deal.contactId }).catch((err) => toast(err?.message || t('Could not open Calendar tab'), { tone: 'danger' })),
          }, t('Google Calendar')),
          el('button.btn', {
            type: 'button',
            onclick: () => store.services.openNewGoogleDoc().catch((err) => toast(err?.message || t('Could not open Docs tab'), { tone: 'danger' })),
          }, t('New Google Doc')),
          el('button.btn', {
            type: 'button',
            onclick: async () => {
              const res = await store.services.linkOpenDocToDeal(deal.id);
              if (res.ok) toast(t('Linked Google Doc: {title}', { title: res.title }));
              else toast(t('Open a Drive or Google Doc tab first.'), { tone: 'warn' });
            },
          }, t('Link open Doc')),
        ]
      : [];

    root.replaceChildren(
      pageHead(deal.name, {
        subtitle: `${billingSummary(deal, cur)} · ${t('Weighted {amount} ({pct}%)', { amount: compactMoney(wVal, cur), pct: Math.round(prob * 100) })}`,
        actions: [
          el('a.btn', { href: '#/pipeline' }, t('All deals')),
          primaryContact?.email
            ? el('button.btn', { type: 'button', onclick: () => openEmailFlow(store, { contact: primaryContact, company, deal }) }, t('Email…'))
            : null,
          ...docsActions,
          el('button.btn', { type: 'button', onclick: () => openDealForm(store, deal) }, t('Edit')),
          el('button.btn.btn-danger', {
            type: 'button',
            onclick: async () => {
              if (!(await confirmDialog(t('Delete "{name}"?', { name: deal.name }), { confirmLabel: t('Delete'), tone: 'danger' }))) return;
              const entry = await store.deleteDeal(deal.id);
              deletedToast(store, entry, t('Deal'));
              window.location.hash = '#/pipeline';
            },
          }, t('Delete')),
        ],
      }),
      el(
        'div.chip-row',
        { style: 'margin-bottom:12px' },
        stageBadge(deal.stage, stages),
        company ? el('a.pill', { href: `#/companies?id=${company.id}` }, company.name) : null,
        deal.expectedClose ? el('span.pill', {}, t('Close {date}', { date: formatDate(deal.expectedClose) })) : null,
        isOpen(deal)
          ? el('span.pill', {}, t('In {stage} for {days}d', { stage: stageLabel(stageInfo(deal.stage, stages), stages), days: daysInStage(deal) }))
          : el('span.pill', {}, deal.closedAt ? t('Closed {date}', { date: formatDate(new Date(deal.closedAt).toISOString().slice(0, 10), { year: 'always' }) }) : t('Closed')),
        lastTouchChip(lastTouch, deal.createdAt),
        tagChips(deal.tags)
      ),
      stepper,
      el(
        'div.detail-grid',
        {},
        el(
          'div',
          { style: 'display:flex;flex-direction:column;gap:14px' },
          deal.notes ? el('div.card', {}, el('h3.card-title', {}, t('Notes')), el('p', { style: 'white-space:pre-wrap;margin:8px 0 0' }, deal.notes)) : null,
          cfCard,
          (deal.stage === 'won' || deal.stage === 'lost')
            ? el(
                'div.card',
                {},
                el('div.card-head', {}, el('h3.card-title', {}, t('Why it closed')), el('button.btn.btn-sm', { type: 'button', onclick: () => openCloseReasonForm(store, deal) }, t('Edit reason'))),
                el('div.row-title', {}, deal.closeReason ? reasonLabel(deal.closeReason) : t('No reason recorded')),
                deal.closeReasonNote ? el('p.muted', { style: 'margin:6px 0 0' }, deal.closeReasonNote) : null
              )
            : null,
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
                el('button.btn.btn-sm', { type: 'button', onclick: () => openPlaybookPicker(store, { dealId: deal.id, contactId: deal.contactId }) }, t('Playbook')),
                el('button.btn.btn-sm', { type: 'button', onclick: () => openTaskForm(store, null, { presetDealId: deal.id, presetContactId: deal.contactId }) }, t('+ Add'))
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
              : el('div.muted', {}, t('No follow-ups on this deal.'))
          ),
          el(
            'div.card',
            {},
            el('div.card-head', {}, el('h3.card-title', {}, t('History')), el('button.btn.btn-sm', { type: 'button', onclick: () => openActivityForm(store, null, { presetDealId: deal.id, presetContactId: deal.contactId }) }, t('+ Log'))),
            acts.length
              ? el('div.row-list', {}, ...acts.map((a) => activityRow(a, store, (x) => openActivityForm(store, x))))
              : el('div.muted', {}, t('No activity logged yet.'))
          )
        ),
        el(
          'div',
          { style: 'display:flex;flex-direction:column;gap:14px' },
          el(
            'div.card',
            {},
            el('div.card-head', {}, el('h3.card-title', {}, t('People')), !hasDecisionMaker(deal) && isOpen(deal) ? el('span.pill.pill-warn', {}, t('No decision maker')) : null),
            peopleRows.length ? el('div.row-list', {}, ...peopleRows) : el('div.muted', {}, t('No people linked.'))
          ),
          deal.stageHistory?.length > 1
            ? el('div.card', {}, el('div.card-head', {}, el('h3.card-title', {}, t('Stage history'))), el('div.row-list', {}, ...historyRows))
            : null,
          attachmentsSection(store, 'deal', deal.id)
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

    const stages = store.resolvedStages();
    const cur = store.settings.currency;
    const mode = store.settings.dealView === 'list' ? 'list' : 'board';

    const matches = store.deals.filter((d) => {
      if (filters.stage === 'open' && !isOpen(d)) return false;
      if (filters.stage !== 'open' && filters.stage !== 'all' && d.stage !== filters.stage) return false;
      if (!matchesCloseWindow(d, filters.closeWindow)) return false;
      if (filters.tags.length && !filters.tags.every((tg) => (d.tags || []).some((x) => x.toLowerCase() === tg.toLowerCase()))) return false;
      for (const [k, v] of Object.entries(filters.fields || {})) {
        if (v === false) {
          if (Boolean(d.custom?.[k])) return false;
        } else if (d.custom?.[k] !== v) {
          return false;
        }
      }
      if (filters.query) {
        const company = store.company(d.companyId)?.name || '';
        const contacts = dealContactIds(d).map((cid) => store.contact(cid)?.name || '').join(' ');
        if (!fuzzyIncludes(`${d.name} ${company} ${contacts} ${d.notes}`, filters.query)) return false;
      }
      return true;
    });

    const sortSpec = filters.sort || DEFAULT_DEAL_SORT;
    const sorted = [...matches].sort((a, b) => {
      const dir = sortSpec.dir === 'asc' ? 1 : -1;
      const getVal = (d) => {
        if (sortSpec.key === 'name') return d.name;
        if (sortSpec.key === 'link') return store.company(d.companyId)?.name || store.contact(d.contactId)?.name || '';
        if (sortSpec.key === 'stage') return stages.findIndex((s) => s.id === d.stage);
        if (sortSpec.key === 'value') return contractValue(d);
        if (sortSpec.key === 'expectedClose') return d.expectedClose || '';
        return d.updatedAt || 0;
      };
      const va = getVal(a);
      const vb = getVal(b);
      if (va === '' && vb !== '') return 1;
      if (vb === '' && va !== '') return -1;
      if (typeof va === 'number' && typeof vb === 'number') {
        if (va !== vb) return (va - vb) * dir;
      } else {
        const cmp = String(va).localeCompare(String(vb));
        if (cmp !== 0) return cmp * dir;
      }
      return String(a.name).localeCompare(String(b.name));
    });

    const visibleIds = sorted.map((d) => d.id);
    selection.prune(visibleIds);

    const searchInp = el('input.input.input-sm', {
      type: 'search',
      placeholder: t('Search deals, companies, people…'),
      value: filters.query,
      'data-focus-key': 'pipeline-search',
      style: 'min-width:210px',
      oninput: (e) => {
        filters.query = e.target.value;
        keepingFocus(render);
      },
    });

    const closeSel = el(
      'select.select.select-sm',
      {
        'data-focus-key': 'pipeline-close-window',
        onchange: (e) => {
          filters.closeWindow = e.target.value;
          keepingFocus(render);
        },
      },
      ...CLOSE_WINDOWS.map((w) => el('option', { value: w.id, selected: filters.closeWindow === w.id }, t(w.label)))
    );

    const stageChipRow = el(
      'div.chip-row',
      {},
      ...[{ id: 'open', label: t('Open') }, { id: 'all', label: t('All') }, ...stages.map((s) => ({ id: s.id, label: stageLabel(s, stages) }))].map((s) =>
        el(
          'button',
          {
            type: 'button',
            class: `filter-chip${filters.stage === s.id ? ' active' : ''}`,
            onclick: () => {
              filters.stage = s.id;
              render();
            },
          },
          s.label
        )
      )
    );

    const toolbar = el(
      'div.filter-toolbar',
      {},
      viewChips('deals', store, filters, (next) => {
        filters = next;
        render();
      }),
      el(
        'div.filter-line',
        {},
        searchInp,
        closeSel,
        tagFilterControl(store.allTags(), filters.tags, (tags) => {
          filters.tags = tags;
          render();
        }),
        customFilterControls(store.customFieldsFor('deals'), filters.fields, (fields) => {
          filters.fields = fields;
          render();
        })
      ),
      stageChipRow
    );

    let mainArea;
    if (mode === 'board') {
      const cols = stages
        .filter((s) => filters.stage === 'open' || filters.stage === 'all' || filters.stage === s.id)
        .map((s) => {
          const colDeals = sorted.filter((d) => d.stage === s.id);
          const colTotal = colDeals.reduce((acc, d) => acc + contractValue(d), 0);
          const isTerminal = s.id === 'won' || s.id === 'lost';
          const collapsed = isTerminal && filters.stage === 'open' && !expanded.has(s.id);

          const colEl = el('div', {
            class: `board-col${collapsed ? ' collapsed' : ''}`,
            ondragover: (e) => {
              e.preventDefault();
              colEl.classList.add('drop-target');
            },
            ondragleave: () => colEl.classList.remove('drop-target'),
            ondrop: (e) => {
              e.preventDefault();
              colEl.classList.remove('drop-target');
              const did = e.dataTransfer.getData('text/deal-id');
              const deal = store.deal(did);
              if (deal) moveDealTo(deal, s.id);
            },
          });

          if (collapsed) {
            colEl.appendChild(
              el(
                'button.board-col-expand',
                {
                  type: 'button',
                  'aria-expanded': 'false',
                  'aria-label': t('Expand {stage} column', { stage: stageLabel(s, stages) }),
                  onclick: () => {
                    expanded.add(s.id);
                    render();
                  },
                },
                stageBadge(s.id, stages),
                el('span.row-meta', {}, `${colDeals.length} · ${compactMoney(colTotal, cur)}`)
              )
            );
          } else {
            colEl.appendChild(
              el(
                'div.board-col-head',
                {},
                stageBadge(s.id, stages),
                el('span.row-meta', {}, `${colDeals.length} · ${compactMoney(colTotal, cur)}`)
              )
            );
          }

          if (!collapsed) {
            for (const d of colDeals) {
              const company = store.company(d.companyId);
              const contact = store.contact(d.contactId);
              const stalled = isStalled(d, store.settings.staleDays);
              const card = el(
                'div.deal-card',
                {
                  draggable: 'true',
                  ondragstart: (e) => e.dataTransfer.setData('text/deal-id', d.id),
                },
                el(
                  'div.panel-row',
                  { style: 'display:flex;justify-content:space-between;align-items:flex-start;gap:6px' },
                  el('a.row-title', { href: `#/pipeline?id=${d.id}` }, d.name),
                  el(
                    'button.icon-btn',
                    {
                      type: 'button',
                      'aria-haspopup': 'dialog',
                      'aria-label': t('Move {name}', { name: d.name }),
                      onclick: (e) => {
                        e.stopPropagation();
                        openMoveSheet(d);
                      },
                    },
                    icon('more', 14)
                  )
                ),
                el('div', { style: 'font-weight:600;color:var(--accent-strong)' }, rateText(d, cur), isRecurring(d) ? el('span.row-meta', {}, ` (${t('{amount} total', { amount: compactMoney(contractValue(d), cur) })})`) : null),
                el('div.row-meta.truncate', {}, [company?.name, contact?.name].filter(Boolean).join(' · ') || t('Unlinked')),
                el(
                  'div.chip-row',
                  {},
                  d.expectedClose ? el('span.pill', {}, formatDate(d.expectedClose)) : null,
                  stalled ? el('span.pill.pill-warn', {}, t('{days}d stalled', { days: daysInStage(d) })) : null,
                  d.confidence != null ? el('span.pill', {}, t('{pct}% conf', { pct: Math.round(d.confidence * 100) })) : null
                )
              );
              colEl.appendChild(card);
            }
          }
          return colEl;
        });

      mainArea = sorted.length
        ? el('div.board', {}, ...cols)
        : emptyState('pipeline', t('No matching deals'), t('Add a new deal or clear your current filters.'), [
            el('button.btn.btn-primary', { type: 'button', onclick: () => openDealForm(store) }, t('+ New deal')),
          ]);
    } else {
      const sortBtn = (key, label) =>
        el(
          'button.sort-btn',
          {
            type: 'button',
            onclick: () => {
              const dir = sortSpec.key === key && sortSpec.dir === 'desc' ? 'asc' : 'desc';
              filters.sort = { key, dir };
              render();
            },
          },
          label,
          el('span', {}, icon('arrowUpDown', 12))
        );

      const bar = bulkBar(selection, visibleIds, [
        el('button.btn.btn-sm', {
          type: 'button',
          onclick: () =>
            pickSheet(
              t('Move {count} deal(s) to stage', { count: selection.count() }),
              stages.map((s) => ({ id: s.id, label: stageLabel(s, stages) })),
              async (stageId) => {
                const count = await store.bulkMoveDeals(selection.ids(), stageId);
                selection.clear();
                if (count > 0) toast(t('Moved {count} deal(s) to {stage}', { count, stage: stageLabel(stageInfo(stageId, stages), stages) }));
              }
            ),
        }, t('Stage')),
        el('button.btn.btn-sm', { type: 'button', onclick: () => openBulkTagSheet(store, 'deals', selection.ids(), () => selection.clear()) }, t('Tag')),
        el('button.btn.btn-sm', { type: 'button', onclick: () => exportSet('deals', store, sorted.filter((d) => selection.has(d.id))) }, t('Export CSV')),
        el('button.btn.btn-sm.btn-danger', {
          type: 'button',
          onclick: async () => {
            if (!(await confirmBulkDelete(selection.count(), 'deals'))) return;
            await store.bulkDeleteDeals(selection.ids());
            selection.clear();
          },
        }, t('Delete')),
      ]);

      const table = el(
        'div.data-table-wrap',
        {},
        el(
          'table.data-table',
          {},
          el(
            'thead',
            {},
            el(
              'tr',
              {},
              el('th', { style: 'width:32px' }),
              el('th', {}, sortBtn('name', t('Deal'))),
              el('th', {}, sortBtn('link', t('Company / Lead'))),
              el('th', {}, sortBtn('stage', t('Stage'))),
              el('th', {}, sortBtn('value', t('Value'))),
              el('th', {}, sortBtn('expectedClose', t('Expected close'))),
              el('th', {}, sortBtn('updatedAt', t('Updated')))
            )
          ),
          el(
            'tbody',
            {},
            ...sorted.map((d) =>
              el(
                'tr',
                {},
                el('td', {}, selectBox(selection, d.id, visibleIds)),
                el('td', {}, el('a.row-title', { href: `#/pipeline?id=${d.id}` }, d.name)),
                el('td', {}, [store.company(d.companyId)?.name, store.contact(d.contactId)?.name].filter(Boolean).join(' · ')),
                el('td', {}, stageBadge(d.stage, stages)),
                el('td', {}, formatMoney(contractValue(d), cur)),
                el('td', {}, d.expectedClose ? formatDate(d.expectedClose) : '—'),
                el('td', {}, d.updatedAt ? formatDate(new Date(d.updatedAt).toISOString().slice(0, 10), { year: 'always' }) : '—')
              )
            )
          )
        )
      );

      mainArea = el('div', {}, bar, sorted.length ? table : emptyState('pipeline', t('No matching deals')));
    }

    const toggleMode = el(
      'div.segmented',
      {},
      el('button', {
        type: 'button',
        class: `seg-btn${mode === 'board' ? ' active' : ''}`,
        'aria-pressed': String(mode === 'board'),
        onclick: () => {
          selection.clear();
          store.updateSettings({ dealView: 'board' });
        },
      }, t('Board')),
      el('button', {
        type: 'button',
        class: `seg-btn${mode === 'list' ? ' active' : ''}`,
        'aria-pressed': String(mode === 'list'),
        onclick: () => {
          selection.clear();
          store.updateSettings({ dealView: 'list' });
        },
      }, t('List'))
    );

    root.replaceChildren(
      pageHead(t('Pipeline'), {
        subtitle: `${t('{count} deal(s)', { count: sorted.length })} · ${formatMoney(sorted.reduce((a, d) => a + contractValue(d), 0), cur)}`,
        actions: [toggleMode, el('button.btn.btn-primary', { type: 'button', onclick: () => openDealForm(store) }, t('+ New deal'))],
      }),
      toolbar,
      mainArea
    );
  };

  render();
  const onChange = () => keepingFocus(render);
  store.addEventListener('change', onChange);
  return () => store.removeEventListener('change', onChange);
}
