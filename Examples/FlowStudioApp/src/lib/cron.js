/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

export const CRON_PRESETS = [
  { id: '*/15 * * * *', label: 'Every 15 minutes' },
  { id: '0 * * * *', label: 'Every hour' },
  { id: '0 9 * * *', label: 'Daily at 09:00' },
  { id: '0 9 * * 1-5', label: 'Weekdays at 09:00' },
  { id: '30 8 * * 1', label: 'Mondays at 08:30' },
];

function parseField(part, min, max, isDow = false) {
  const set = new Set();
  const tokens = String(part || '').split(',');
  for (const tok of tokens) {
    if (!tok) return null;
    const [rangeStr, stepStr] = tok.split('/');
    if (tok.split('/').length > 2) return null;
    const step = stepStr === undefined ? 1 : Number(stepStr);
    if (!Number.isInteger(step) || step <= 0) return null;

    let start, end;
    if (rangeStr === '*') {
      start = min;
      end = max;
    } else if (rangeStr.includes('-')) {
      const [aStr, bStr] = rangeStr.split('-');
      if (rangeStr.split('-').length > 2 || aStr === '' || bStr === '') return null;
      start = Number(aStr);
      end = Number(bStr);
      if (!Number.isInteger(start) || !Number.isInteger(end) || start > end) return null;
    } else {
      start = Number(rangeStr);
      end = start;
      if (!Number.isInteger(start)) return null;
    }

    const effectiveMax = isDow ? 7 : max;
    if (start < min || end > effectiveMax) return null;
    for (let v = start; v <= end; v += step) {
      set.add(isDow && v === 7 ? 0 : v);
    }
  }
  return set.size ? set : null;
}

export function parseCronExpression(expr) {
  const raw = String(expr || '').trim();
  const parts = raw.split(/\s+/);
  if (parts.length !== 5) {
    return { ok: false, error: 'Cron requires 5 fields: minute hour day month weekday' };
  }
  const minute = parseField(parts[0], 0, 59, false);
  const hour = parseField(parts[1], 0, 23, false);
  const dom = parseField(parts[2], 1, 31, false);
  const month = parseField(parts[3], 1, 12, false);
  const dow = parseField(parts[4], 0, 6, true);
  if (!minute || !hour || !dom || !month || !dow) {
    return { ok: false, error: 'Invalid cron range or step value' };
  }
  return {
    ok: true,
    fields: {
      minute,
      hour,
      dom,
      month,
      dow,
      domWildcard: parts[2] === '*',
      dowWildcard: parts[4] === '*',
    },
  };
}

export function matchesCron(expr, date = new Date()) {
  const parsed = typeof expr === 'object' && expr?.fields ? expr : parseCronExpression(expr);
  if (!parsed.ok) return false;
  const { minute, hour, dom, month, dow, domWildcard, dowWildcard } = parsed.fields;
  if (!minute.has(date.getMinutes())) return false;
  if (!hour.has(date.getHours())) return false;
  if (!month.has(date.getMonth() + 1)) return false;

  const domMatch = dom.has(date.getDate());
  const dowMatch = dow.has(date.getDay());
  if (!domWildcard && !dowWildcard) return domMatch || dowMatch;
  return domMatch && dowMatch;
}

export function nextCronRuns(expr, count = 3, fromDate = new Date()) {
  const parsed = parseCronExpression(expr);
  if (!parsed.ok) return [];
  const out = [];
  const cursor = new Date(fromDate.getTime());
  cursor.setSeconds(0, 0);
  cursor.setMinutes(cursor.getMinutes() + 1);

  const maxScanMinutes = 366 * 24 * 60;
  for (let i = 0; i < maxScanMinutes && out.length < count; i++) {
    if (matchesCron(parsed, cursor)) out.push(new Date(cursor.getTime()));
    cursor.setMinutes(cursor.getMinutes() + 1);
  }
  return out;
}

export function cronSlotKey(date = new Date()) {
  const p = (n) => String(n).padStart(2, '0');
  return `${date.getFullYear()}-${p(date.getMonth() + 1)}-${p(date.getDate())} ${p(date.getHours())}:${p(date.getMinutes())}`;
}
