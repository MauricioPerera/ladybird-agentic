/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from './i18n.js';
import { clamp, formatMoney, monthKey, round2, shiftMonthKey } from './util.js';

export const BILLING_MODES = [
  { id: 'one-time', label: 'One-time' },
  { id: 'monthly', label: 'Monthly' },
  { id: 'annual', label: 'Annual' },
];

export const DEFAULT_TERM = 12;
export const MAX_TERM = 120;

const MODE_SET = new Set(BILLING_MODES.map((m) => m.id));

export function normalizeBilling(raw) {
  const b = String(raw || '').trim().toLowerCase();
  return MODE_SET.has(b) ? b : 'one-time';
}

export function isRecurring(deal) {
  return normalizeBilling(deal?.billing) !== 'one-time';
}

export function normalizeTerm(value, billing = 'one-time') {
  if (normalizeBilling(billing) === 'one-time') return null;
  const n = Math.round(Number(value));
  if (!Number.isFinite(n) || n < 1) return DEFAULT_TERM;
  return clamp(n, 1, MAX_TERM);
}

export function termOf(deal) {
  const billing = normalizeBilling(deal?.billing);
  if (billing === 'one-time') return 1;
  return normalizeTerm(deal?.term, billing) ?? DEFAULT_TERM;
}

export function mrrOf(deal) {
  const billing = normalizeBilling(deal?.billing);
  const val = Number(deal?.value) || 0;
  if (billing === 'monthly') return round2(val);
  if (billing === 'annual') return round2(val / 12);
  return 0;
}

export function arrOf(deal) {
  return round2(mrrOf(deal) * 12);
}

export function contractValue(deal) {
  const billing = normalizeBilling(deal?.billing);
  const val = Number(deal?.value) || 0;
  if (billing === 'monthly') return round2(val * termOf(deal));
  if (billing === 'annual') return round2(val * (termOf(deal) / 12));
  return round2(val);
}

export function billingSpread(deal, startKey = monthKey()) {
  const billing = normalizeBilling(deal?.billing);
  const val = Number(deal?.value) || 0;
  if (billing === 'one-time') {
    return [{ key: startKey, amount: round2(val) }];
  }
  const term = termOf(deal);
  const out = [];
  if (billing === 'monthly') {
    const monthly = round2(val);
    for (let i = 0; i < term; i++) {
      out.push({ key: shiftMonthKey(startKey, i), amount: monthly });
    }
    return out;
  }
  for (let i = 0; i < term; i += 12) {
    const monthsCovered = Math.min(12, term - i);
    out.push({
      key: shiftMonthKey(startKey, i),
      amount: round2(val * (monthsCovered / 12)),
    });
  }
  return out;
}

export function recurringSummary(deals, key = monthKey(), today = new Date()) {
  let mrr = 0;
  let active = 0;
  for (const deal of deals || []) {
    if (deal.stage !== 'won' || !isRecurring(deal)) continue;
    const start = monthKey(deal.closedAt ? new Date(deal.closedAt) : today);
    const end = shiftMonthKey(start, termOf(deal));
    if (key >= start && key < end) {
      mrr = round2(mrr + mrrOf(deal));
      active++;
    }
  }
  return { mrr, arr: round2(mrr * 12), active };
}

export function pipelineMRR(deals) {
  let mrr = 0;
  for (const deal of deals || []) {
    if (deal.stage === 'won' || deal.stage === 'lost') continue;
    if (isRecurring(deal)) mrr = round2(mrr + mrrOf(deal));
  }
  return mrr;
}

export function rateText(deal, currency = 'USD') {
  const billing = normalizeBilling(deal?.billing);
  const money = formatMoney(deal?.value || 0, currency);
  if (billing === 'monthly') return t('{money}/mo', { money });
  if (billing === 'annual') return t('{money}/yr', { money });
  return money;
}

export function billingSummary(deal, currency = 'USD') {
  const billing = normalizeBilling(deal?.billing);
  if (billing === 'one-time') return formatMoney(deal?.value || 0, currency);
  const term = termOf(deal);
  return t('{rate} × {term} mo ({total} total)', {
    rate: rateText(deal, currency),
    term,
    total: formatMoney(contractValue(deal), currency),
  });
}
