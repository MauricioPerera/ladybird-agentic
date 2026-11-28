/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from '../lib/i18n.js';
import { compactMoney, formatMoney, formatMonth } from '../lib/util.js';
import { el } from '../ui/dom.js';

const PLOT_H = 170;
const LABEL_HEADROOM = 24;
const BOTTOM_LABEL_H = 18;

export function forecastChart(rows, currency = 'USD', { goal = 0 } = {}) {
  const max = Math.max(
    1,
    goal || 0,
    ...(rows || []).map((r) => (r.best || 0) + (r.won || 0))
  );
  const px = (v) => (v > 0 ? Math.max(3, Math.round((v / max) * (PLOT_H - LABEL_HEADROOM - BOTTOM_LABEL_H))) : 0);

  const plot = el('div.chart-plot', { role: 'img', 'aria-label': t('Monthly revenue forecast') });
  if (goal > 0) {
    const linePx = Math.round((goal / max) * (PLOT_H - LABEL_HEADROOM - BOTTOM_LABEL_H));
    plot.appendChild(
      el('div.chart-goal-line', {
        style: `top:${Math.max(4, PLOT_H - BOTTOM_LABEL_H - linePx)}px`,
        title: t('Monthly goal: {amount}', { amount: formatMoney(goal, currency) }),
      })
    );
  }

  for (const r of rows || []) {
    const total = (r.best || 0) + (r.won || 0);
    const upside = Math.max(0, (r.best || 0) - (r.weighted || 0));
    const stack = el('div.chart-stack', {
      title: t('{month}: Won {won}, Weighted {weighted}, Best case {best}', {
        month: formatMonth(r.key),
        won: formatMoney(r.won, currency),
        weighted: formatMoney(r.weighted, currency),
        best: formatMoney(r.best, currency),
      }),
    });
    if (r.won > 0) stack.appendChild(el('div.seg-won-bar', { style: `height:${px(r.won)}px` }));
    if (r.weighted > 0) stack.appendChild(el('div.seg-weighted-bar', { style: `height:${px(r.weighted)}px` }));
    if (upside > 0) stack.appendChild(el('div.seg-upside-bar', { style: `height:${px(upside)}px` }));

    plot.appendChild(
      el(
        'div.chart-col',
        {},
        el('div.row-meta', {}, total > 0 ? compactMoney(total, currency) : '0'),
        stack,
        el('div.row-meta', {}, formatMonth(r.key, { year: 'auto' }))
      )
    );
  }

  const legend = el(
    'div.chip-row',
    { style: 'margin-top:8px' },
    el('span.pill', {}, el('span', { style: 'width:8px;height:8px;border-radius:2px;background:var(--viz-won)' }), t('Won')),
    el('span.pill', {}, el('span', { style: 'width:8px;height:8px;border-radius:2px;background:var(--viz-weighted)' }), t('Weighted')),
    el('span.pill', {}, el('span', { style: 'width:8px;height:8px;border-radius:2px;background:var(--viz-upside)' }), t('Upside'))
  );

  return el('div', {}, plot, legend);
}
