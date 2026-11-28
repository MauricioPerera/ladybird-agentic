/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { isTerminalStage, openStages, STAGES } from './forecast.js';
import { round2 } from './util.js';

export const FUNNEL_WINDOWS = [
  { id: 'all', label: 'All time' },
  { id: 'quarter', label: 'This quarter' },
  { id: 'last-quarter', label: 'Last quarter' },
  { id: 'year', label: 'This year' },
];

export function enteredAt(deal) {
  const first = deal?.stageHistory?.[0];
  if (first && Number.isFinite(first.at)) return first.at;
  return Number(deal?.createdAt) || 0;
}

export function matchesFunnelWindow(deal, windowId, today = new Date()) {
  if (!windowId || windowId === 'all') return true;
  const ts = enteredAt(deal);
  if (!ts) return false;
  const d = new Date(ts);
  if (windowId === 'year') return d.getFullYear() === today.getFullYear();

  const curQ = Math.floor(today.getMonth() / 3);
  const curY = today.getFullYear();
  const q = Math.floor(d.getMonth() / 3);
  const y = d.getFullYear();
  if (windowId === 'quarter') return y === curY && q === curQ;
  if (windowId === 'last-quarter') {
    const prevQ = (curQ + 3) % 4;
    const prevY = curQ === 0 ? curY - 1 : curY;
    return y === prevY && q === prevQ;
  }
  return true;
}

export function hasOpenHistory(deal) {
  if (!isTerminalStage(deal?.stage)) return true;
  return Boolean(deal?.stageHistory?.some((e) => !isTerminalStage(e.stage)));
}

export function lostFromStage(deal) {
  if (deal?.stage !== 'lost') return null;
  const hist = deal.stageHistory || [];
  for (let i = hist.length - 1; i >= 0; i--) {
    if (!isTerminalStage(hist[i].stage)) return hist[i].stage;
  }
  return null;
}

export function journeyOf(deal, order) {
  const reached = new Set();
  for (const h of deal?.stageHistory || []) {
    if (h?.stage) reached.add(h.stage);
  }
  if (deal?.stage) reached.add(deal.stage);

  let furthest = -1;
  for (const s of reached) {
    if (s === 'won') {
      furthest = Math.max(furthest, order.size);
    } else if (order.has(s)) {
      furthest = Math.max(furthest, order.get(s));
    }
  }
  return { reached, furthest };
}

export function funnelReport(deals, stages = STAGES) {
  const open = openStages(stages);
  const order = new Map(open.map((s, i) => [s.id, i]));
  const tracked = (deals || []).filter(hasOpenHistory);
  const untracked = (deals || []).length - tracked.length;

  const rowMap = new Map(
    open.map((s) => [s.id, { stage: s.id, label: s.label, gone: false, entered: 0, advanced: 0, lostHere: 0, open: 0 }])
  );

  for (const d of tracked) {
    for (const h of d.stageHistory || []) {
      if (h?.stage && !isTerminalStage(h.stage) && !rowMap.has(h.stage)) {
        rowMap.set(h.stage, { stage: h.stage, label: h.stage, gone: true, entered: 0, advanced: 0, lostHere: 0, open: 0 });
      }
    }
    if (d.stage && !isTerminalStage(d.stage) && !rowMap.has(d.stage)) {
      rowMap.set(d.stage, { stage: d.stage, label: d.stage, gone: true, entered: 0, advanced: 0, lostHere: 0, open: 0 });
    }
  }

  let wonCount = 0;
  let lostCount = 0;
  let openCount = 0;

  for (const d of tracked) {
    if (d.stage === 'won') wonCount++;
    else if (d.stage === 'lost') lostCount++;
    else openCount++;

    const { reached, furthest } = journeyOf(d, order);
    const lostAt = lostFromStage(d);

    for (const stage of reached) {
      if (isTerminalStage(stage)) continue;
      const row = rowMap.get(stage);
      if (!row) continue;
      row.entered += 1;
      const idx = order.get(stage);
      const isAdv = idx !== undefined ? furthest > idx : d.stage === 'won' || (d.stage !== 'lost' && d.stage !== stage);
      if (isAdv) {
        row.advanced += 1;
      } else if (lostAt === stage) {
        row.lostHere += 1;
      } else if (d.stage !== 'lost') {
        row.open += 1;
      }
    }
  }

  const rows = [...rowMap.values()].filter((r) => !r.gone || r.entered > 0);
  const lostTotal = rows.reduce((acc, r) => acc + r.lostHere, 0);
  for (const r of rows) {
    r.advanceRate = r.entered ? round2(r.advanced / r.entered) : null;
    r.lossShare = lostTotal ? round2(r.lostHere / lostTotal) : null;
  }

  return {
    rows,
    untracked,
    totals: {
      deals: tracked.length,
      won: wonCount,
      lost: lostCount,
      open: openCount,
      wonRate: tracked.length ? round2(wonCount / tracked.length) : null,
      lostShareBase: lostTotal,
    },
  };
}
