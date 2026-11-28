/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { addDays, isValidISODate, todayISO } from './util.js';

const CONTROL_EXCEPT_TAB = /[\x00-\x08\x0b-\x1f\x7f\u0085\u2028\u2029]/g;
const ALL_CONTROL = /[\x00-\x1f\x7f\u0085\u2028\u2029]/g;

export function safeIdentifier(value) {
  return String(value ?? '').replace(ALL_CONTROL, '');
}

export function escapeText(value) {
  return String(value ?? '')
    .replace(/\r\n|\r|\n/g, '\n')
    .replace(CONTROL_EXCEPT_TAB, '')
    .replace(/\\/g, '\\\\')
    .replace(/;/g, '\\;')
    .replace(/,/g, '\\,')
    .replace(/\n/g, '\\n');
}

function utf8Octets(cp) {
  if (cp <= 0x7f) return 1;
  if (cp <= 0x7ff) return 2;
  if (cp <= 0xffff) return 3;
  return 4;
}

export function foldLine(line) {
  const out = [];
  let cur = '';
  let size = 0;
  for (const ch of String(line)) {
    const n = utf8Octets(ch.codePointAt(0));
    if (size + n > 75 && cur.length > 0) {
      out.push(cur);
      cur = ' ' + ch;
      size = 1 + n;
    } else {
      cur += ch;
      size += n;
    }
  }
  if (cur.length > 0 || out.length === 0) out.push(cur);
  return out.join('\r\n');
}

function stampUTC(d = new Date()) {
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getUTCFullYear()}${p(d.getUTCMonth() + 1)}${p(d.getUTCDate())}T${p(d.getUTCHours())}${p(d.getUTCMinutes())}${p(d.getUTCSeconds())}Z`;
}

export function buildEvent(event, now = new Date()) {
  const lines = [
    'BEGIN:VEVENT',
    `UID:${safeIdentifier(event.uid)}`,
    `DTSTAMP:${stampUTC(now)}`,
    `SUMMARY:${escapeText(event.summary)}`,
  ];
  if (event.description) lines.push(`DESCRIPTION:${escapeText(event.description)}`);

  const dateStr = isValidISODate(String(event.date || '')) ? String(event.date) : todayISO(now);
  const ymd = dateStr.replace(/-/g, '');
  if (event.time && /^([01]\d|2[0-3]):[0-5]\d$/.test(String(event.time))) {
    const [hh, mm] = String(event.time).split(':').map(Number);
    const startMin = hh * 60 + mm;
    const durMin = Math.max(1, Number(event.minutes) || 30);
    const totalMin = startMin + durMin;
    const dayShift = Math.floor(totalMin / 1440);
    const endMin = totalMin % 1440;
    const endDate = dayShift > 0 ? addDays(dateStr, dayShift).replace(/-/g, '') : ymd;
    const p = (n) => String(n).padStart(2, '0');
    lines.push(`DTSTART:${ymd}T${p(hh)}${p(mm)}00`);
    lines.push(`DTEND:${endDate}T${p(Math.floor(endMin / 60))}${p(endMin % 60)}00`);
    if (event.alarm) {
      lines.push('BEGIN:VALARM', 'ACTION:DISPLAY', `DESCRIPTION:${escapeText(event.summary)}`, 'TRIGGER:-PT30M', 'END:VALARM');
    }
  } else {
    const nextDay = addDays(dateStr, 1).replace(/-/g, '');
    lines.push(`DTSTART;VALUE=DATE:${ymd}`);
    lines.push(`DTEND;VALUE=DATE:${nextDay}`);
  }

  lines.push('END:VEVENT');
  return lines.map(foldLine).join('\r\n');
}

export function taskEvent(task, { contactName = '', dealName = '' } = {}) {
  const details = [];
  if (contactName) details.push(`Lead: ${contactName}`);
  if (dealName) details.push(`Deal: ${dealName}`);
  return {
    uid: `crm-task-${task.id}@sales-crm.ladybird`,
    summary: `Follow up: ${task.title || 'Task'}`,
    description: details.join('\n'),
    date: task.due,
    time: task.dueTime || '',
    minutes: 30,
    alarm: Boolean(task.dueTime),
  };
}

export function dealEvent(deal, { companyName = '' } = {}) {
  return {
    uid: `crm-deal-${deal.id}@sales-crm.ladybird`,
    summary: `Expected close: ${deal.name || 'Deal'}`,
    description: companyName ? `Company: ${companyName}` : '',
    date: deal.expectedClose,
    time: '',
    alarm: false,
  };
}

export function buildCalendar(events, now = new Date()) {
  const head = [
    'BEGIN:VCALENDAR',
    'VERSION:2.0',
    'PRODID:-//Sales CRM//Ladybird App Mode//EN',
    'CALSCALE:GREGORIAN',
    'METHOD:PUBLISH',
  ];
  const body = (events || []).map((e) => buildEvent(e, now));
  return [...head, ...body, 'END:VCALENDAR', ''].join('\r\n');
}
