/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { addDays, daysInMonth, monthKey, shiftMonthKey, todayISO } from './util.js';

export const WEEKDAYS = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];

export function weekdayIndex(iso) {
  const [y, m, d] = String(iso).split('-').map(Number);
  return (new Date(y, m - 1, d).getDay() + 6) % 7;
}

function makeCell(iso, today, targetMonth = null) {
  const day = Number(iso.slice(8, 10));
  const weekday = weekdayIndex(iso);
  return {
    iso,
    day,
    weekday,
    today: iso === today,
    past: iso < today,
    weekend: weekday >= 5,
    inMonth: targetMonth ? iso.slice(0, 7) === targetMonth : true,
  };
}

export function monthCells(key = monthKey(), today = todayISO()) {
  const [y, m] = String(key).split('-').map(Number);
  const first = `${y}-${String(m).padStart(2, '0')}-01`;
  const last = `${y}-${String(m).padStart(2, '0')}-${String(daysInMonth(y, m)).padStart(2, '0')}`;
  const start = addDays(first, -weekdayIndex(first));
  const end = addDays(last, 6 - weekdayIndex(last));
  const out = [];
  for (let cur = start; cur <= end; cur = addDays(cur, 1)) {
    out.push(makeCell(cur, today, key));
  }
  return out;
}

export function weekCells(anchorISO = todayISO(), today = todayISO()) {
  const start = addDays(anchorISO, -weekdayIndex(anchorISO));
  const out = [];
  for (let i = 0; i < 7; i++) {
    out.push(makeCell(addDays(start, i), today, null));
  }
  return out;
}

export function shiftPeriod(mode, anchor, n) {
  if (mode === 'month') return shiftMonthKey(anchor, n);
  return addDays(anchor, n * 7);
}

export function periodAnchor(mode, today = todayISO()) {
  return mode === 'month' ? monthKey(today) : addDays(today, -weekdayIndex(today));
}

export function byDay(entries, dateOf) {
  const map = new Map();
  for (const item of entries || []) {
    const key = dateOf(item);
    if (!key) continue;
    const list = map.get(key) || [];
    list.push(item);
    map.set(key, list);
  }
  return map;
}

export function cellRange(cells) {
  if (!cells || cells.length === 0) return { start: '', end: '' };
  return { start: cells[0].iso, end: cells[cells.length - 1].iso };
}

export function inRange(entries, range, dateOf) {
  if (!range?.start || !range?.end) return [];
  return (entries || []).filter((e) => {
    const d = dateOf(e);
    return d && d >= range.start && d <= range.end;
  });
}
