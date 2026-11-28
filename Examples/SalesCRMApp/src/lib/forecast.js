/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { billingSpread, contractValue } from './recurring.js';
import { clamp, daysInMonth, monthKey, round2, shiftMonthKey, todayISO } from './util.js';

export const DEFAULT_OPEN_STAGES = [
  { id: 'new', label: 'New', probability: 0.1 },
  { id: 'qualified', label: 'Qualified', probability: 0.25 },
  { id: 'proposal', label: 'Proposal', probability: 0.5 },
  { id: 'negotiation', label: 'Negotiation', probability: 0.75 },
];

export const TERMINAL_STAGES = [
  { id: 'won', label: 'Won', probability: 1 },
  { id: 'lost', label: 'Lost', probability: 0 },
];

export const STAGES = [...DEFAULT_OPEN_STAGES, ...TERMINAL_STAGES];
export const MAX_OPEN_STAGES = 8;
export const DEFAULT_STALE_DAYS = 14;

export const CONFIDENCE_LEVELS = [
  { id: 'commit', label: 'Commit', value: 0.9 },
  { id: 'likely', label: 'Likely', value: 0.7 },
  { id: 'best', label: 'Best case', value: 0.25 },
  { id: 'omit', label: 'Omit', value: 0 },
];

export const WON_REASONS = ['Referral strength', 'Product fit', 'Price', 'Relationship', 'Timing', 'Other'];
export const LOST_REASONS = ['Price', 'Competitor', 'Timing', 'No budget', 'Went silent', 'No fit', 'Other'];

export const CLOSE_WINDOWS = [
  { id: 'any', label: 'Any close date' },
  { id: 'month', label: 'This month' },
  { id: 'quarter', label: 'This quarter' },
  { id: 'overdue', label: 'Overdue close' },
];

const clamp01 = (n) => clamp(Number(n), 0, 1);

export function isTerminalStage(stageId) {
  return stageId === 'won' || stageId === 'lost';
}

export function isOpen(deal) {
  return !isTerminalStage(deal?.stage);
}

export function sanitizeOpenStages(raw) {
  if (!Array.isArray(raw)) return null;
  const seen = new Set();
  const out = [];
  for (const item of raw) {
    if (!item || typeof item !== 'object') continue;
    const id = String(item.id ?? '').trim();
    const label = String(item.label ?? '').trim();
    if (!id || !label || isTerminalStage(id) || seen.has(id)) continue;
    seen.add(id);
    const pRaw = item.probability;
    const prob =
      pRaw !== null && pRaw !== undefined && pRaw !== '' && Number.isFinite(Number(pRaw))
        ? clamp01(Number(pRaw))
        : 0.5;
    out.push({ id, label, probability: prob });
    if (out.length >= MAX_OPEN_STAGES) break;
  }
  return out.length > 0 ? out : null;
}

export function resolveStages(settings) {
  const custom = sanitizeOpenStages(settings?.stages);
  return [...(custom || DEFAULT_OPEN_STAGES), ...TERMINAL_STAGES];
}

export function openStages(stages = STAGES) {
  return stages.filter((s) => !isTerminalStage(s.id));
}

export function stageInfo(stageId, stages = STAGES) {
  return stages.find((s) => s.id === stageId) || stages[0];
}

export function normalizeConfidence(raw) {
  if (raw === null || raw === undefined || raw === '') return null;
  const n = Number(raw);
  return Number.isFinite(n) ? clamp01(n) : null;
}

export function probabilityOf(deal, overrides = null, stages = STAGES) {
  if (deal?.stage === 'won') return 1;
  if (deal?.stage === 'lost') return 0;
  const conf = normalizeConfidence(deal?.confidence);
  if (conf !== null) return conf;
  const ov = normalizeConfidence(overrides?.[deal?.stage]);
  if (ov !== null) return ov;
  return stageInfo(deal?.stage, stages).probability;
}

export function weightedValue(deal, overrides = null, stages = STAGES) {
  return round2(contractValue(deal) * probabilityOf(deal, overrides, stages));
}

export function pipelineValue(deals) {
  let total = 0;
  for (const d of deals || []) {
    if (isOpen(d)) total += contractValue(d);
  }
  return round2(total);
}

export function weightedPipeline(deals, overrides = null, stages = STAGES) {
  let total = 0;
  for (const d of deals || []) {
    if (isOpen(d)) total += weightedValue(d, overrides, stages);
  }
  return round2(total);
}

export function stageSummary(deals, overrides = null, stages = STAGES) {
  const map = new Map(stages.map((s) => [s.id, { stage: s.id, label: s.label, count: 0, value: 0, weighted: 0 }]));
  for (const d of deals || []) {
    const row = map.get(d.stage) || map.get(stages[0].id);
    row.count += 1;
    row.value = round2(row.value + contractValue(d));
    row.weighted = round2(row.weighted + weightedValue(d, overrides, stages));
  }
  return [...map.values()];
}

export function forecastByMonth(deals, { months = 6, from = monthKey(), overrides = null, stages = STAGES, today = new Date() } = {}) {
  const rows = [];
  const byKey = new Map();
  for (let i = 0; i < months; i++) {
    const key = shiftMonthKey(from, i);
    const row = { key, best: 0, weighted: 0, won: 0, count: 0 };
    rows.push(row);
    byKey.set(key, row);
  }

  for (const d of deals || []) {
    if (d.stage === 'lost') continue;
    if (d.stage === 'won') {
      const start = monthKey(d.closedAt ? new Date(d.closedAt) : today);
      for (const entry of billingSpread(d, start)) {
        const row = byKey.get(entry.key);
        if (row) row.won = round2(row.won + entry.amount);
      }
      continue;
    }

    const startRaw = d.expectedClose ? monthKey(d.expectedClose) : from;
    const start = startRaw < from ? from : startRaw;
    const prob = probabilityOf(d, overrides, stages);
    let counted = false;
    for (const entry of billingSpread(d, start)) {
      const row = byKey.get(entry.key);
      if (!row) continue;
      row.best = round2(row.best + entry.amount);
      row.weighted = round2(row.weighted + round2(entry.amount * prob));
      if (!counted) {
        row.count += 1;
        counted = true;
      }
    }
  }
  return rows;
}

export function wonInMonth(deals, key = monthKey(), today = new Date()) {
  let total = 0;
  for (const d of deals || []) {
    if (d.stage !== 'won') continue;
    const start = monthKey(d.closedAt ? new Date(d.closedAt) : today);
    for (const entry of billingSpread(d, start)) {
      if (entry.key === key) total = round2(total + entry.amount);
    }
  }
  return total;
}

export function goalProgress(deals, goal, key = monthKey(), today = new Date()) {
  const g = Number(goal) || 0;
  const won = wonInMonth(deals, key, today);
  if (g <= 0) {
    return { won, goal: 0, pct: null, expected: 0, remaining: 0, gap: 0, pace: null };
  }
  const nowKey = monthKey(today);
  let elapsed = 1;
  if (key > nowKey) elapsed = 0;
  else if (key === nowKey) {
    const [y, m] = key.split('-').map(Number);
    elapsed = Math.min(1, today.getDate() / Math.max(1, daysInMonth(y, m)));
  }
  const expected = round2(g * elapsed);
  const remaining = round2(Math.max(0, g - won));
  const gap = round2(Math.max(0, expected - won));
  const pace = won >= g ? 'met' : won >= expected ? 'on-track' : 'behind';
  return { won, goal: g, pct: won / g, expected, remaining, gap, pace };
}

export function pipelineCoverage(weighted, remainingGoal) {
  if (!remainingGoal || remainingGoal <= 0) return null;
  return round2(weighted / remainingGoal);
}

export function winRate(deals) {
  let won = 0;
  let lost = 0;
  for (const d of deals || []) {
    if (d.stage === 'won') won++;
    else if (d.stage === 'lost') lost++;
  }
  const total = won + lost;
  return { won, lost, rate: total ? won / total : null };
}

export function avgClosedValue(deals) {
  let wonSum = 0;
  let wonCount = 0;
  let lostSum = 0;
  let lostCount = 0;
  for (const d of deals || []) {
    if (d.stage === 'won') {
      wonSum += contractValue(d);
      wonCount++;
    } else if (d.stage === 'lost') {
      lostSum += contractValue(d);
      lostCount++;
    }
  }
  return {
    won: wonCount ? round2(wonSum / wonCount) : null,
    lost: lostCount ? round2(lostSum / lostCount) : null,
  };
}

export function avgSalesCycle(deals) {
  let daysSum = 0;
  let count = 0;
  for (const d of deals || []) {
    if (d.stage !== 'won' || !Number.isFinite(d.createdAt) || !Number.isFinite(d.closedAt)) continue;
    daysSum += Math.max(0, (d.closedAt - d.createdAt) / 86400000);
    count++;
  }
  return count ? Math.round(daysSum / count) : null;
}

export function reasonsForStage(stageId, deals = []) {
  const base = stageId === 'won' ? [...WON_REASONS] : [...LOST_REASONS];
  const seen = new Set(base.map((r) => r.toLowerCase()));
  for (const d of deals) {
    if (d.stage !== stageId) continue;
    const r = String(d.closeReason || '').trim();
    if (r && !seen.has(r.toLowerCase())) {
      seen.add(r.toLowerCase());
      base.push(r);
    }
  }
  return base;
}

export function closeReasonBreakdown(deals, stageId) {
  const byReason = new Map();
  let unrecorded = 0;
  let total = 0;
  for (const d of deals || []) {
    if (d.stage !== stageId) continue;
    total++;
    const reason = String(d.closeReason || '').trim();
    if (!reason) {
      unrecorded++;
      continue;
    }
    const row = byReason.get(reason) || { reason, count: 0, value: 0 };
    row.count += 1;
    row.value = round2(row.value + contractValue(d));
    byReason.set(reason, row);
  }
  const rows = [...byReason.values()].sort((a, b) => b.count - a.count || b.value - a.value);
  return { rows, unrecorded, total };
}

export function winRateBy(deals, labelOf) {
  const map = new Map();
  for (const d of deals || []) {
    if (d.stage !== 'won' && d.stage !== 'lost') continue;
    const key = String(labelOf(d) || '').trim();
    if (!key) continue;
    const row = map.get(key) || { key, won: 0, lost: 0, total: 0, value: 0 };
    row.total += 1;
    if (d.stage === 'won') {
      row.won += 1;
      row.value = round2(row.value + contractValue(d));
    } else {
      row.lost += 1;
    }
    map.set(key, row);
  }
  return [...map.values()]
    .map((r) => ({ ...r, rate: r.total ? r.won / r.total : null }))
    .sort((a, b) => b.total - a.total || b.won - a.won || a.key.localeCompare(b.key));
}

export function matchesCloseWindow(deal, windowId, today = new Date()) {
  if (!windowId || windowId === 'any') return true;
  if (!deal?.expectedClose) return false;
  if (windowId === 'month') return monthKey(deal.expectedClose) === monthKey(today);
  if (windowId === 'quarter') {
    const [y, m] = deal.expectedClose.split('-').map(Number);
    const dealQ = Math.floor((m - 1) / 3);
    const nowQ = Math.floor(today.getMonth() / 3);
    return y === today.getFullYear() && dealQ === nowQ;
  }
  if (windowId === 'overdue') return isOpen(deal) && deal.expectedClose < todayISO(today);
  return true;
}

export function stageEnteredAt(deal) {
  const hist = deal?.stageHistory;
  if (Array.isArray(hist) && hist.length > 0 && Number.isFinite(hist[hist.length - 1]?.at)) {
    return hist[hist.length - 1].at;
  }
  return Number(deal?.updatedAt) || Number(deal?.createdAt) || 0;
}

export function daysInStage(deal, now = Date.now()) {
  return Math.max(0, Math.floor((now - stageEnteredAt(deal)) / 86400000));
}

export function isStalled(deal, staleDays = DEFAULT_STALE_DAYS, now = Date.now()) {
  return isOpen(deal) && staleDays > 0 && daysInStage(deal, now) >= staleDays;
}

export function stalledDeals(deals, staleDays = DEFAULT_STALE_DAYS, now = Date.now()) {
  return (deals || [])
    .filter((d) => isStalled(d, staleDays, now))
    .sort((a, b) => stageEnteredAt(a) - stageEnteredAt(b));
}

export function stageVelocity(deals, stages = STAGES) {
  const totals = new Map();
  for (const d of deals || []) {
    const hist = d.stageHistory;
    if (!Array.isArray(hist) || hist.length < 2) continue;
    for (let i = 0; i < hist.length - 1; i++) {
      const cur = hist[i];
      const next = hist[i + 1];
      if (!cur?.stage || !Number.isFinite(cur.at) || !Number.isFinite(next?.at)) continue;
      const t = totals.get(cur.stage) || { days: 0, count: 0 };
      t.days += Math.max(0, (next.at - cur.at) / 86400000);
      t.count += 1;
      totals.set(cur.stage, t);
    }
  }
  return openStages(stages).map((s) => {
    const t = totals.get(s.id);
    return {
      stage: s.id,
      label: s.label,
      days: t ? Math.round(t.days / t.count) : null,
      count: t ? t.count : 0,
    };
  });
}

export function stageRampColor(stageId, stages = STAGES) {
  if (stageId === 'won') return 'var(--stage-won)';
  if (stageId === 'lost') return 'var(--stage-lost)';
  const open = openStages(stages);
  const idx = Math.max(0, open.findIndex((s) => s.id === stageId));
  const pct = open.length > 1 ? Math.round((idx / (open.length - 1)) * 100) : 50;
  return `color-mix(in srgb, var(--stage-ramp-to) ${pct}%, var(--stage-ramp-from))`;
}
