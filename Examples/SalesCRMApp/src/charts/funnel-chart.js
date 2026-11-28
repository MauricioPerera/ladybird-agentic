/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { stageRampColor, STAGES } from '../lib/forecast.js';
import { stageLabel, t } from '../lib/i18n.js';
import { el } from '../ui/dom.js';

export function funnelChart(rows, stages = STAGES) {
  const max = Math.max(1, ...(rows || []).map((r) => r.entered || 0));
  const wrap = el('div.row-list', { role: 'img', 'aria-label': t('Pipeline conversion funnel') });

  for (const r of rows || []) {
    const outerPct = r.entered ? Math.max((r.entered / max) * 100, 3) : 0;
    const innerPct = r.entered ? (r.advanced / r.entered) * 100 : 0;
    const rateTxt = r.advanceRate != null ? `${Math.round(r.advanceRate * 100)}%` : '—';
    const color = stageRampColor(r.stage, stages);

    wrap.appendChild(
      el(
        'div.funnel-row',
        {},
        el('div', {}, el('span.row-title', {}, stageLabel({ id: r.stage, label: r.label }, stages)), r.gone ? el('span.row-meta', {}, ` ${t('(removed)')}`) : null),
        el(
          'div.funnel-track',
          { title: t('{entered} entered · {advanced} advanced ({rate}) · {lost} lost here', { entered: r.entered, advanced: r.advanced, rate: rateTxt, lost: r.lostHere }) },
          el(
            'div.funnel-entered',
            { style: `width:${outerPct.toFixed(1)}%` },
            el('div.funnel-advanced', { style: `width:${innerPct.toFixed(1)}%;background:${color}` })
          )
        ),
        el('div.row-meta', { style: 'text-align:right' }, `${r.advanced}/${r.entered} (${rateTxt})`)
      )
    );
  }
  return wrap;
}
