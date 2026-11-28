/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { contractValue } from './recurring.js';
import { monthKey, round2, shiftMonthKey } from './util.js';

export function monthSpan(months = 12, to = monthKey()) {
  const out = [];
  for (let i = months - 1; i >= 0; i--) {
    out.push(shiftMonthKey(to, -i));
  }
  return out;
}

export function monthlyFlows(deals, { months = 12, to = monthKey() } = {}) {
  const keys = monthSpan(months, to);
  const rows = keys.map((key) => ({
    key,
    added: { count: 0, value: 0 },
    won: { count: 0, value: 0 },
    lost: { count: 0, value: 0 },
  }));
  const byKey = new Map(rows.map((r) => [r.key, r]));

  for (const d of deals || []) {
    const val = contractValue(d);
    if (Number.isFinite(d.createdAt)) {
      const k = monthKey(new Date(d.createdAt));
      const row = byKey.get(k);
      if (row) {
        row.added.count += 1;
        row.added.value = round2(row.added.value + val);
      }
    }
    if ((d.stage === 'won' || d.stage === 'lost') && Number.isFinite(d.closedAt)) {
      const k = monthKey(new Date(d.closedAt));
      const row = byKey.get(k);
      if (row) {
        row[d.stage].count += 1;
        row[d.stage].value = round2(row[d.stage].value + val);
      }
    }
  }
  return rows;
}

export function winRateTrend(flows) {
  return (flows || []).map((r) => {
    const closed = r.won.count + r.lost.count;
    return {
      key: r.key,
      rate: closed ? round2(r.won.count / closed) : null,
      closed,
    };
  });
}

export function salesCycleTrend(deals, { months = 12, to = monthKey() } = {}) {
  const keys = monthSpan(months, to);
  const buckets = new Map(keys.map((k) => [k, { days: 0, count: 0 }]));
  for (const d of deals || []) {
    if (d.stage !== 'won' || !Number.isFinite(d.createdAt) || !Number.isFinite(d.closedAt)) continue;
    const k = monthKey(new Date(d.closedAt));
    const t = buckets.get(k);
    if (!t) continue;
    t.days += Math.max(0, (d.closedAt - d.createdAt) / 86400000);
    t.count += 1;
  }
  return keys.map((key) => {
    const t = buckets.get(key);
    return {
      key,
      days: t && t.count ? Math.round(t.days / t.count) : null,
      count: t ? t.count : 0,
    };
  });
}
