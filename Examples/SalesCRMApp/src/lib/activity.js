/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { ACTIVITY_TYPES } from './statuses.js';
import { addDays, clamp, round2, todayISO } from './util.js';

export const ACTIVITY_WINDOWS = [
  { id: 'week', label: 'This week' },
  { id: '30', label: 'Last 30 days' },
  { id: 'month', label: 'This month' },
  { id: 'all', label: 'All time' },
];

export function weekStartISO(today = todayISO()) {
  const [y, m, d] = today.split('-').map(Number);
  const dt = new Date(y, m - 1, d);
  const monIndex = (dt.getDay() + 6) % 7;
  return addDays(today, -monIndex);
}

export function weekRange(today = todayISO()) {
  const start = weekStartISO(today);
  return { start, end: addDays(start, 6) };
}

export function weekElapsed(today = todayISO()) {
  const [y, m, d] = today.split('-').map(Number);
  const dt = new Date(y, m - 1, d);
  return (((dt.getDay() + 6) % 7) + 1) / 7;
}

export function matchesActivityWindow(act, windowId, today = todayISO()) {
  if (!windowId || windowId === 'all') return true;
  const date = act?.date;
  if (!date) return false;
  if (windowId === 'week') {
    const { start, end } = weekRange(today);
    return date >= start && date <= end;
  }
  if (windowId === '30') {
    return date >= addDays(today, -29) && date <= today;
  }
  if (windowId === 'month') {
    return date.slice(0, 7) === today.slice(0, 7);
  }
  return true;
}

export function normalizeTouchTargets(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const normInt = (v) => clamp(Math.round(Number(v) || 0), 0, 999);
  const out = {
    all: normInt(raw.all),
    meeting: normInt(raw.meeting),
    call: normInt(raw.call),
    email: normInt(raw.email),
    note: normInt(raw.note),
  };
  return Object.values(out).some((v) => v > 0) ? out : null;
}

export function touchTarget(targets) {
  if (!targets) return 0;
  if (targets.all > 0) return targets.all;
  return (targets.meeting || 0) + (targets.call || 0) + (targets.email || 0) + (targets.note || 0);
}

export function touchCounts(activities, range) {
  const counts = { all: 0, meeting: 0, call: 0, email: 0, note: 0 };
  for (const a of activities || []) {
    if (!a.date || a.date < range.start || a.date > range.end) continue;
    counts.all += 1;
    const t = ACTIVITY_TYPES.some((k) => k.id === a.type) ? a.type : 'note';
    counts[t] += 1;
  }
  return counts;
}

export function touchProgress(activities, targets, today = todayISO()) {
  const range = weekRange(today);
  const counts = touchCounts(activities, range);
  const target = touchTarget(targets);
  const elapsed = weekElapsed(today);
  const expected = round2(target * elapsed);
  const logged = counts.all;
  const gap = Math.max(0, Math.ceil(expected - logged));
  const pace = target === 0 ? null : logged >= target ? 'met' : logged >= expected ? 'on-track' : 'behind';

  const byType = ACTIVITY_TYPES.map((t) => {
    const typeTarget = targets?.[t.id] || 0;
    const typeLogged = counts[t.id] || 0;
    return {
      type: t.id,
      label: t.label,
      logged: typeLogged,
      target: typeTarget,
      pct: typeTarget > 0 ? typeLogged / typeTarget : null,
    };
  });

  return { range, logged, target, expected, gap, pace, byType };
}
