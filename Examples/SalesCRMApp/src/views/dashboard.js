/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { touchProgress } from '../lib/activity.js';
import { goingCold, noDecisionMaker, noNextStep } from '../lib/attention.js';
import {
  daysInStage,
  goalProgress,
  pipelineValue,
  stalledDeals,
  stageSummary,
  weightedPipeline,
  winRate,
  wonInMonth,
} from '../lib/forecast.js';
import { t } from '../lib/i18n.js';
import { contractValue, pipelineMRR, recurringSummary } from '../lib/recurring.js';
import { compactMoney, formatDate, formatMoney, formatMonth, monthKey, shiftMonthKey, todayISO } from '../lib/util.js';
import { activityRow, duePill, emptyState, pageHead, stageBadge, statTile } from '../ui/bits.js';
import { el, keepingFocus } from '../ui/dom.js';
import { openActivityForm, openContactForm, openDealForm, openTaskForm } from '../ui/forms.js';
import { icon } from '../ui/icons.js';
import { openOnboarding } from '../ui/onboarding.js';

const ATTENTION_MAX = 6;

function greetingText(hour, name) {
  const part = hour < 5 ? t('Working late') : hour < 12 ? t('Good morning') : hour < 17 ? t('Good afternoon') : t('Good evening');
  return name && name !== 'Default' ? `${part}, ${name}` : part;
}

export function dashboardView(root, { store }) {
  const render = () => {
    const today = todayISO();
    const now = new Date();
    const curMonth = monthKey(now);
    const prevMonth = shiftMonthKey(curMonth, -1);
    const cur = store.settings.currency;
    const stages = store.resolvedStages();
    const ov = store.settings.stageProbabilities;
    const acctName = store.currentAccount?.label || store.currentAccount?.id || '';

    const nodes = [
      pageHead(greetingText(now.getHours(), acctName), {
        subtitle: t('Today · {date}', { date: formatDate(today, { year: 'always' }) }),
        actions: [
          el('button.btn', { type: 'button', onclick: () => openActivityForm(store) }, t('+ Log activity')),
          el('button.btn.btn-primary', { type: 'button', onclick: () => openDealForm(store) }, t('+ New deal')),
        ],
      }),
    ];

    if (store.deals.length === 0 && store.contacts.length === 0 && store.companies.length === 0) {
      nodes.push(
        emptyState(
          'pipeline',
          t('Your pipeline is empty'),
          t('Add your first lead, import a CSV, or walk through the setup wizard with optional sample data.'),
          [
            el('button.btn.btn-primary', { type: 'button', onclick: () => openContactForm(store) }, t('Add a lead')),
            el('button.btn', { type: 'button', onclick: () => openOnboarding(store, () => openContactForm(store)) }, t('Show me around')),
          ]
        )
      );
      root.replaceChildren(...nodes);
      return;
    }

    const openTotal = pipelineValue(store.deals);
    const weightedTotal = weightedPipeline(store.deals, ov, stages);
    const openMrr = pipelineMRR(store.deals);
    const recWon = recurringSummary(store.deals, curMonth, now);
    const wonCur = wonInMonth(store.deals, curMonth, now);
    const wonPrev = wonInMonth(store.deals, prevMonth, now);
    const goal = goalProgress(store.deals, store.settings.revenueGoal, curMonth, now);
    const wr = winRate(store.deals);
    const touches = touchProgress(store.activities, store.settings.touchTargets, today);

    const tiles = [
      statTile(t('Open pipeline'), compactMoney(openTotal, cur), t('{count} open deal(s)', { count: store.deals.filter((d) => d.stage !== 'won' && d.stage !== 'lost').length })),
      openMrr > 0 || recWon.mrr > 0
        ? statTile('MRR', compactMoney(recWon.mrr, cur), openMrr > 0 ? t('{amount} in open pipeline', { amount: compactMoney(openMrr, cur) }) : `${compactMoney(recWon.arr, cur)} ARR`)
        : null,
      statTile(t('Weighted forecast'), compactMoney(weightedTotal, cur), t('Stage & confidence adjusted')),
      goal.goal > 0
        ? statTile(
            t('Won in {month}', { month: formatMonth(curMonth, { year: 'auto' }) }),
            compactMoney(wonCur, cur),
            `${goal.pace === 'met' ? t('Goal met') : goal.pace === 'on-track' ? t('On track') : t('Behind pace')} · ${t('goal {amount}', { amount: compactMoney(goal.goal, cur) })}`,
            { progress: goal.pct }
          )
        : statTile(
            t('Won in {month}', { month: formatMonth(curMonth, { year: 'auto' }) }),
            compactMoney(wonCur, cur),
            wonPrev > 0 ? t('vs {amount} in {month}', { amount: compactMoney(wonPrev, cur), month: formatMonth(prevMonth, { year: 'auto' }) }) : t('Closed won revenue')
          ),
      statTile(t('Win rate'), wr.rate != null ? `${Math.round(wr.rate * 100)}%` : '—', t('{won} won · {lost} lost', { won: wr.won, lost: wr.lost })),
      touches.target > 0
        ? statTile(
            t('Touches this week'),
            `${touches.logged}/${touches.target}`,
            touches.pace === 'met' ? t('Weekly target met') : touches.pace === 'on-track' ? t('On track') : t('{gap} behind pace', { gap: touches.gap }),
            { progress: touches.logged / touches.target }
          )
        : null,
    ].filter(Boolean);
    nodes.push(el('div.stat-grid', {}, ...tiles));

    const openTasks = store.openTasks();
    const dueNow = openTasks.filter((tsk) => tsk.due <= today);
    const upNext = openTasks.filter((tsk) => tsk.due > today).slice(0, 3);
    const stalled = stalledDeals(store.deals, store.settings.staleDays);
    const usesRoles = store.deals.some((d) => d.contacts?.some((c) => Boolean(c.role)));
    const missingDecider = usesRoles ? noDecisionMaker(store.deals).slice(0, 3) : [];

    const attentionBody = el('div.row-list');
    if (dueNow.length === 0) {
      attentionBody.appendChild(el('div.muted', {}, t('No overdue or due-today follow-ups.')));
    } else {
      for (const tsk of dueNow.slice(0, ATTENTION_MAX)) {
        const contact = store.contact(tsk.contactId);
        const deal = store.deal(tsk.dealId);
        attentionBody.appendChild(
          el(
            'div.list-row',
            {},
            el(
              'div.row-main',
              {},
              el('input', {
                type: 'checkbox',
                checked: false,
                'aria-label': t('Complete {name}', { name: tsk.title }),
                onchange: () => store.toggleTask(tsk.id, true),
              }),
              el(
                'div',
                { style: 'min-width:0;flex:1' },
                el('a.row-title.truncate', { href: `#/followups?task=${tsk.id}` }, tsk.title),
                el('div.row-meta.truncate', {}, [contact?.name, deal?.name].filter(Boolean).join(' · '))
              )
            ),
            duePill(tsk.due, tsk.dueTime)
          )
        );
      }
      if (dueNow.length > ATTENTION_MAX) {
        attentionBody.appendChild(
          el('a.row-meta', { href: '#/followups', style: 'text-decoration:underline' }, t('+{count} more in Follow-ups', { count: dueNow.length - ATTENTION_MAX }))
        );
      }
    }

    if (upNext.length) {
      attentionBody.appendChild(el('div.field-label', { style: 'margin-top:8px' }, t('Up next')));
      for (const tsk of upNext) {
        attentionBody.appendChild(
          el('a.list-row', { href: `#/followups?task=${tsk.id}` }, el('span.row-title.truncate', {}, tsk.title), duePill(tsk.due, tsk.dueTime))
        );
      }
    }
    if (missingDecider.length) {
      attentionBody.appendChild(el('div.field-label', { style: 'margin-top:8px' }, t('No decision maker named')));
      for (const d of missingDecider) {
        attentionBody.appendChild(
          el('a.list-row', { href: `#/pipeline?id=${d.id}` }, el('span.row-title.truncate', {}, d.name), el('span.pill.pill-warn', {}, formatMoney(contractValue(d), cur)))
        );
      }
    }
    if (stalled.length) {
      attentionBody.appendChild(el('div.field-label', { style: 'margin-top:8px' }, t('Stalled deals')));
      for (const d of stalled.slice(0, 3)) {
        attentionBody.appendChild(
          el('a.list-row', { href: `#/pipeline?id=${d.id}` }, el('span.row-title.truncate', {}, d.name), el('span.pill.pill-warn', {}, t('{days}d in stage', { days: daysInStage(d) })))
        );
      }
    }

    const cold = goingCold(store.contacts, (id) => store.lastContactedAt(id), { coldDays: store.settings.coldDays, today }).slice(0, 4);
    const noStep = noNextStep(store.deals, {
      lastTouchOf: (d) => store.lastTouchedDeal(d),
      hasOpenTask: (id) => store.hasOpenTask(id),
      coldDays: store.settings.coldDays,
      today,
    }).slice(0, 3);

    const coldBody = el('div.row-list');
    if (cold.length === 0 && noStep.length === 0) {
      coldBody.appendChild(el('div.muted', {}, t('Everyone active has recent touches and next steps.')));
    } else {
      for (const c of cold) {
        coldBody.appendChild(
          el(
            'div.list-row',
            {},
            el(
              'div',
              { style: 'min-width:0;flex:1' },
              el('a.row-title.truncate', { href: `#/leads?id=${c.contact.id}` }, c.contact.name),
              el('div.row-meta', {}, c.never ? t('No activity ({days}d)', { days: c.days }) : t('Quiet {days}d', { days: c.days }))
            ),
            el(
              'div.chip-row',
              {},
              el('button.icon-btn', { type: 'button', title: t('Log activity'), 'aria-label': t('Log activity for {name}', { name: c.contact.name }), onclick: () => openActivityForm(store, null, { presetContactId: c.contact.id }) }, icon('activity', 14)),
              el('button.icon-btn', { type: 'button', title: t('Add follow-up'), 'aria-label': t('Add follow-up for {name}', { name: c.contact.name }), onclick: () => openTaskForm(store, null, { presetContactId: c.contact.id }) }, icon('calendar', 14))
            )
          )
        );
      }
      if (noStep.length) {
        coldBody.appendChild(el('div.field-label', { style: 'margin-top:8px' }, t('Open deals with no next step')));
        for (const item of noStep) {
          coldBody.appendChild(
            el(
              'div.list-row',
              {},
              el('a.row-title.truncate', { href: `#/pipeline?id=${item.deal.id}` }, item.deal.name),
              el('button.btn.btn-sm', { type: 'button', onclick: () => openTaskForm(store, null, { presetDealId: item.deal.id, presetContactId: item.deal.contactId }) }, t('Follow up'))
            )
          );
        }
      }
    }

    nodes.push(
      el(
        'div.two-col',
        {},
        el('div.card', {}, el('div.card-head', {}, el('h2.card-title', {}, t('Needs attention'))), attentionBody),
        el('div.card', {}, el('div.card-head', {}, el('h2.card-title', {}, t('Going cold'))), coldBody)
      )
    );

    const glanceRows = stageSummary(store.deals, ov, stages)
      .filter((s) => s.stage !== 'won' && s.stage !== 'lost')
      .map((s) =>
        el(
          'div.list-row',
          {},
          stageBadge(s.stage, stages),
          el('span.row-meta', {}, t('{count} deal(s)', { count: s.count })),
          el('strong', {}, formatMoney(s.value, cur))
        )
      );

    const recentActs = [...store.activities]
      .sort((a, b) => String(b.date).localeCompare(String(a.date)) || b.createdAt - a.createdAt)
      .slice(0, 5);

    nodes.push(
      el(
        'div.two-col',
        {},
        el('div.card', {}, el('div.card-head', {}, el('h2.card-title', {}, t('Pipeline at a glance')), el('a.btn.btn-sm', { href: '#/pipeline' }, t('Board'))), el('div.row-list', {}, ...glanceRows)),
        el(
          'div.card',
          {},
          el('div.card-head', {}, el('h2.card-title', {}, t('Recent activity')), el('button.btn.btn-sm', { type: 'button', onclick: () => openActivityForm(store) }, t('+ Log'))),
          recentActs.length
            ? el('div.row-list', {}, ...recentActs.map((a) => activityRow(a, store, (x) => openActivityForm(store, x))))
            : el('div.muted', {}, t('No logged calls, meetings, or notes yet.'))
        )
      )
    );

    root.replaceChildren(...nodes);
  };

  render();
  const onChange = () => keepingFocus(render);
  store.addEventListener('change', onChange);
  return () => store.removeEventListener('change', onChange);
}
