/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from '../lib/i18n.js';
import { compactMoney, formatMoney, formatMonth } from '../lib/util.js';
import { el } from '../ui/dom.js';

const PLOT_H = 120;
const MINI_H = 72;

export function flowsChart(flows, currency = 'USD') {
  const max = Math.max(
    1,
    ...(flows || []).flatMap((r) => [r.added.value || 0, r.won.value || 0, r.lost.value || 0])
  );
  const px = (v) => (v > 0 ? Math.max(3, Math.round((v / max) * PLOT_H)) : 0);

  const scroller = el('div.flows-scroll', { role: 'img', 'aria-label': t('Monthly pipeline added, won, and lost') });
  for (const r of flows || []) {
    const tip = t('{month}: Added {added} ({addedCount}), Won {won} ({wonCount}), Lost {lost} ({lostCount})', {
      month: formatMonth(r.key),
      added: formatMoney(r.added.value, currency),
      addedCount: r.added.count,
      won: formatMoney(r.won.value, currency),
      wonCount: r.won.count,
      lost: formatMoney(r.lost.value, currency),
      lostCount: r.lost.count,
    });
    scroller.appendChild(
      el(
        'div.flow-group',
        { title: tip },
        el(
          'div.flow-bars',
          {},
          el('div.flow-bar', { style: `height:${px(r.added.value)}px;background:var(--viz-added)` }),
          el('div.flow-bar', { style: `height:${px(r.won.value)}px;background:var(--viz-won)` }),
          el('div.flow-bar', { style: `height:${px(r.lost.value)}px;background:var(--viz-lost)` })
        ),
        el('div.row-meta', {}, formatMonth(r.key, { year: 'auto' }))
      )
    );
  }
  requestAnimationFrame(() => {
    scroller.scrollLeft = scroller.scrollWidth;
  });

  const legend = el(
    'div.chip-row',
    { style: 'margin-top:8px' },
    el('span.pill', {}, el('span', { style: 'width:8px;height:8px;border-radius:2px;background:var(--viz-added)' }), t('Added')),
    el('span.pill', {}, el('span', { style: 'width:8px;height:8px;border-radius:2px;background:var(--viz-won)' }), t('Won')),
    el('span.pill', {}, el('span', { style: 'width:8px;height:8px;border-radius:2px;background:var(--viz-lost)' }), t('Lost')),
    flows?.length ? el('span.row-meta', {}, t('Peak {amount}', { amount: compactMoney(max, currency) })) : null
  );

  return el('div', {}, scroller, legend);
}

export function miniTrend(rows, { format = (v) => String(v), ariaLabel = null } = {}) {
  const max = Math.max(1, ...(rows || []).map((r) => r.value ?? 0));
  const scroller = el('div.mini-trend', { role: 'img', 'aria-label': ariaLabel || t('Trend') });

  for (const r of rows || []) {
    const val = r.value;
    const h = val != null ? Math.max(4, Math.round((val / max) * MINI_H)) : 0;
    const label = `${formatMonth(r.key)}: ${val != null ? format(val) : t('No data')}`;
    scroller.appendChild(
      el(
        'div',
        { style: 'display:flex;flex-direction:column;align-items:center;gap:3px;min-width:28px', title: label },
        val != null ? el('div.mini-bar', { style: `height:${h}px` }) : el('div.col-zero'),
        el('div.row-meta', { style: 'font-size:10px' }, r.key.slice(5))
      )
    );
  }
  requestAnimationFrame(() => {
    scroller.scrollLeft = scroller.scrollWidth;
  });
  return scroller;
}
