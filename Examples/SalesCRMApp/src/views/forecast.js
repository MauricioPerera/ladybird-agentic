/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { forecastChart } from '../charts/forecast-chart.js';
import { funnelChart } from '../charts/funnel-chart.js';
import { flowsChart, miniTrend } from '../charts/trends-chart.js';
import {
  avgClosedValue,
  avgSalesCycle,
  closeReasonBreakdown,
  forecastByMonth,
  goalProgress,
  pipelineCoverage,
  pipelineValue,
  stageSummary,
  stageVelocity,
  weightedPipeline,
  winRate,
  winRateBy,
} from '../lib/forecast.js';
import { FUNNEL_WINDOWS, funnelReport, matchesFunnelWindow } from '../lib/funnel.js';
import { leadSourceLabel, reasonLabel, stageLabel, t } from '../lib/i18n.js';
import { isRecurring, pipelineMRR, recurringSummary } from '../lib/recurring.js';
import { monthlyFlows, salesCycleTrend, winRateTrend } from '../lib/trends.js';
import { compactMoney, formatMoney, formatMonth, monthKey } from '../lib/util.js';
import { emptyState, pageHead, stageBadge, statTile } from '../ui/bits.js';
import { el, keepingFocus } from '../ui/dom.js';

export function forecastView(root, { store }) {
  let funnelWindow = 'all';

  const render = () => {
    const cur = store.settings.currency;
    const stages = store.resolvedStages();
    const ov = store.settings.stageProbabilities;
    const key = monthKey();

    if (store.deals.length === 0) {
      root.replaceChildren(
        pageHead(t('Forecast')),
        emptyState('chart', t('No deals to forecast yet'), t('Add deals to see weighted forecasts, recurring MRR, conversion funnels, and pipeline trends.'))
      );
      return;
    }

    const weighted = weightedPipeline(store.deals, ov, stages);
    const bestCase = pipelineValue(store.deals);
    const goal = goalProgress(store.deals, store.settings.revenueGoal, key);
    const cov = pipelineCoverage(weighted, goal.remaining);
    const wr = winRate(store.deals);
    const avgCycle = avgSalesCycle(store.deals);
    const recWon = recurringSummary(store.deals, key);
    const openMrr = pipelineMRR(store.deals);
    const hasRecurring = openMrr > 0 || recWon.mrr > 0 || store.deals.some(isRecurring);

    const tiles = [
      statTile(t('Weighted pipeline'), compactMoney(weighted, cur), t('Probability adjusted')),
      cov != null ? statTile(t('Goal coverage'), `${cov}×`, t('vs {amount} remaining', { amount: compactMoney(goal.remaining, cur) })) : null,
      statTile(t('Best case'), compactMoney(bestCase, cur), t('All open deals')),
      statTile(t('Win rate'), wr.rate != null ? `${Math.round(wr.rate * 100)}%` : '—', t('{won} won · {lost} lost', { won: wr.won, lost: wr.lost })),
      statTile(t('Sales cycle'), avgCycle != null ? t('{days}d', { days: avgCycle }) : '—', t('Avg days to win')),
    ].filter(Boolean);

    const monthsRows = forecastByMonth(store.deals, { months: 6, overrides: ov, stages });
    const monthTable = el(
      'div.data-table-wrap',
      {},
      el(
        'table.data-table',
        {},
        el('thead', {}, el('tr', {}, el('th', {}, t('Month')), el('th', {}, t('Deals')), el('th', {}, t('Best case')), el('th', {}, t('Weighted')), el('th', {}, t('Won')))),
        el(
          'tbody',
          {},
          ...monthsRows.map((r) =>
            el('tr', {}, el('td', {}, formatMonth(r.key)), el('td', {}, String(r.count)), el('td', {}, formatMoney(r.best, cur)), el('td', {}, formatMoney(r.weighted, cur)), el('td', {}, formatMoney(r.won, cur)))
          )
        )
      )
    );

    const flows = monthlyFlows(store.deals, { months: 12 });
    const wrSeries = winRateTrend(flows).map((r) => ({ key: r.key, value: r.rate != null ? Math.round(r.rate * 100) : null }));
    const cycSeries = salesCycleTrend(store.deals, { months: 12 }).map((r) => ({ key: r.key, value: r.days }));

    const cohortDeals = store.deals.filter((d) => matchesFunnelWindow(d, funnelWindow));
    const funnel = funnelReport(cohortDeals, stages);
    const mostLosses = [...funnel.rows].sort((a, b) => b.lostHere - a.lostHere)[0];
    const vel = stageVelocity(store.deals, stages);
    const velMax = Math.max(1, ...vel.map((v) => v.days ?? 0));

    const wonReasons = closeReasonBreakdown(store.deals, 'won');
    const lostReasons = closeReasonBreakdown(store.deals, 'lost');
    const avgVals = avgClosedValue(store.deals);

    const bySource = winRateBy(store.deals, (d) => store.contact(d.contactId)?.source || '');
    const byIndustry = winRateBy(store.deals, (d) => store.company(d.companyId)?.industry || '');

    const reasonCard = (title, data, avgVal) =>
      el(
        'div.card',
        {},
        el('div.card-head', {}, el('h3.card-title', {}, title), avgVal != null ? el('span.pill', {}, t('Avg {amount}', { amount: compactMoney(avgVal, cur) })) : null),
        data.rows.length
          ? el(
              'div.row-list',
              {},
              ...data.rows.map((r) =>
                el(
                  'div.list-row',
                  {},
                  el('span.row-title', {}, reasonLabel(r.reason)),
                  el('span.row-meta', {}, `${r.count} · ${compactMoney(r.value, cur)}`)
                )
              ),
              data.unrecorded ? el('div.row-meta', {}, t('+{count} without recorded reason', { count: data.unrecorded })) : null
            )
          : el('div.muted', {}, t('No reasons recorded yet.'))
      );

    const sourceTable = (title, list, formatKey = (k) => k) =>
      el(
        'div.card',
        {},
        el('div.card-head', {}, el('h3.card-title', {}, title)),
        list.length
          ? el(
              'div.row-list',
              {},
              ...list.map((r) =>
                el(
                  'div.list-row',
                  {},
                  el('span.row-title', {}, formatKey(r.key)),
                  el('span.row-meta', {}, `${r.won}/${r.total} (${r.rate != null ? Math.round(r.rate * 100) : 0}%) · ${compactMoney(r.value, cur)}`)
                )
              )
            )
          : el('div.muted', {}, t('No closed deals tagged yet.'))
      );

    const stageBars = stageSummary(store.deals, ov, stages)
      .filter((s) => s.stage !== 'won' && s.stage !== 'lost')
      .map((s) =>
        el(
          'div.list-row',
          {},
          stageBadge(s.stage, stages),
          el('span.row-meta', {}, t('{count} deal(s) · Total {amount}', { count: s.count, amount: formatMoney(s.value, cur) })),
          el('strong', {}, t('{amount} weighted', { amount: formatMoney(s.weighted, cur) }))
        )
      );

    root.replaceChildren(
      pageHead(t('Forecast & Analytics'), {
        subtitle: hasRecurring ? `MRR ${formatMoney(recWon.mrr, cur)} (${formatMoney(recWon.arr, cur)} ARR)` : t('Pipeline projection'),
      }),
      el('div.stat-grid', {}, ...tiles),
      el(
        'div.two-col',
        {},
        el(
          'div.card',
          {},
          el('div.card-head', {}, el('h2.card-title', {}, t('Next 6 months')), hasRecurring ? el('span.pill', {}, t('Recurring spread active')) : null),
          forecastChart(monthsRows, cur, { goal: store.settings.revenueGoal })
        ),
        el('div.card', {}, el('div.card-head', {}, el('h2.card-title', {}, t('Month by month'))), monthTable)
      ),
      el(
        'div.card',
        { style: 'margin-bottom:16px' },
        el('div.card-head', {}, el('h2.card-title', {}, t('Pipeline trends · last 12 months'))),
        flowsChart(flows, cur),
        el(
          'div.two-col',
          { style: 'margin-top:14px;margin-bottom:0' },
          el('div', {}, el('div.field-label', {}, t('Win rate %')), miniTrend(wrSeries, { format: (v) => `${v}%`, ariaLabel: t('Win rate trend') })),
          el('div', {}, el('div.field-label', {}, t('Sales cycle (days)')), miniTrend(cycSeries, { format: (v) => `${v}d`, ariaLabel: t('Sales cycle trend') }))
        )
      ),
      el(
        'div.two-col',
        {},
        el(
          'div.card',
          {},
          el(
            'div.card-head',
            {},
            el('h2.card-title', {}, t('Conversion funnel')),
            el(
              'select.select.select-sm',
              {
                'data-focus-key': 'funnel-window',
                onchange: (e) => {
                  funnelWindow = e.target.value;
                  render();
                },
              },
              ...FUNNEL_WINDOWS.map((w) => el('option', { value: w.id, selected: funnelWindow === w.id }, t(w.label)))
            )
          ),
          el(
            'div.chip-row',
            { style: 'margin-bottom:10px' },
            el('span.pill', {}, t('Entry to won: {pct}%', { pct: funnel.totals.wonRate != null ? Math.round(funnel.totals.wonRate * 100) : 0 })),
            mostLosses?.lostHere ? el('span.pill.pill-warn', {}, t('Most losses: {stage} ({count})', { stage: stageLabel({ id: mostLosses.stage, label: mostLosses.label }, stages), count: mostLosses.lostHere })) : null,
            funnel.untracked ? el('span.row-meta', {}, t('{count} pre-tracking deal(s) omitted', { count: funnel.untracked })) : null
          ),
          funnelChart(funnel.rows, stages)
        ),
        el(
          'div.card',
          {},
          el('div.card-head', {}, el('h2.card-title', {}, t('Time in each stage'))),
          el(
            'div.row-list',
            {},
            ...vel.map((v) =>
              el(
                'div',
                { style: 'display:flex;flex-direction:column;gap:4px' },
                el('div', { style: 'display:flex;justify-content:space-between;font-size:12px' }, el('span.row-title', {}, stageLabel({ id: v.stage, label: v.label }, stages)), el('span.row-meta', {}, v.days != null ? t('{days}d avg ({count})', { days: v.days, count: v.count }) : '—')),
                el('div.progress-track', {}, el('div.progress-fill', { style: `width:${v.days != null ? Math.round((v.days / velMax) * 100) : 0}%` }))
              )
            )
          )
        )
      ),
      el(
        'div.two-col',
        {},
        reasonCard(t('Why deals win'), wonReasons, avgVals.won),
        reasonCard(t('Why deals lose'), lostReasons, avgVals.lost)
      ),
      el(
        'div.two-col',
        {},
        sourceTable(t('Wins by lead source'), bySource, leadSourceLabel),
        sourceTable(t('Wins by industry'), byIndustry)
      ),
      el('div.card', {}, el('div.card-head', {}, el('h2.card-title', {}, t('Where the weighted number comes from'))), el('div.row-list', {}, ...stageBars))
    );
  };

  render();
  const onChange = () => keepingFocus(render);
  store.addEventListener('change', onChange);
  return () => store.removeEventListener('change', onChange);
}
