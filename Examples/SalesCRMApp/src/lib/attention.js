/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { hasDecisionMaker } from './deal-contacts.js';
import { isOpen } from './forecast.js';
import { contractValue } from './recurring.js';
import { daysBetween, todayISO } from './util.js';

export const DEFAULT_COLD_DAYS = 14;
const CHASEABLE = new Set(['lead', 'active']);

export function quietSince(lastAt, createdAt, today = todayISO()) {
  const since = lastAt || (Number.isFinite(createdAt) ? todayISO(new Date(createdAt)) : today);
  return {
    since,
    never: !lastAt,
    days: Math.max(0, daysBetween(since, today)),
  };
}

export function goingCold(contacts, lastTouchOf, { coldDays = DEFAULT_COLD_DAYS, today = todayISO() } = {}) {
  if (!coldDays || coldDays <= 0) return [];
  const out = [];
  for (const contact of contacts || []) {
    if (!CHASEABLE.has(contact.status)) continue;
    const info = quietSince(lastTouchOf(contact.id), contact.createdAt, today);
    if (info.days >= coldDays) {
      out.push({ contact, ...info });
    }
  }
  out.sort((a, b) => b.days - a.days || String(a.contact.name).localeCompare(String(b.contact.name)));
  return out;
}

export function noNextStep(deals, { lastTouchOf, hasOpenTask, coldDays = DEFAULT_COLD_DAYS, today = todayISO() } = {}) {
  if (!coldDays || coldDays <= 0) return [];
  const out = [];
  for (const deal of deals || []) {
    if (!isOpen(deal)) continue;
    if (hasOpenTask(deal.id)) continue;
    const info = quietSince(lastTouchOf(deal), deal.createdAt, today);
    if (info.days >= coldDays) {
      out.push({ deal, ...info });
    }
  }
  out.sort((a, b) => b.days - a.days || contractValue(b.deal) - contractValue(a.deal));
  return out;
}

export function noDecisionMaker(deals) {
  return (deals || [])
    .filter((d) => isOpen(d) && !hasDecisionMaker(d))
    .sort((a, b) => contractValue(b) - contractValue(a) || String(a.name).localeCompare(String(b.name)));
}
