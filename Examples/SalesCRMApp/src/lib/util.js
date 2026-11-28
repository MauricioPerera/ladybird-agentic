/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { monthsShort, t } from './i18n.js';

export function uid() {
  if (globalThis.crypto?.randomUUID) return crypto.randomUUID();
  return `id-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

const pad2 = (n) => String(n).padStart(2, '0');

export function todayISO(d = new Date()) {
  return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;
}

export function monthKey(date = new Date()) {
  if (typeof date === 'string') return date.slice(0, 7);
  return `${date.getFullYear()}-${pad2(date.getMonth() + 1)}`;
}

export function shiftMonthKey(key, n) {
  const [y, m] = String(key).split('-').map(Number);
  const d = new Date(y, m - 1 + n, 1);
  return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}`;
}

export function addDays(iso, n) {
  const [y, m, d] = String(iso).split('-').map(Number);
  return todayISO(new Date(y, m - 1, d + n));
}

const MONTH_LENGTHS = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
const isLeapYear = (y) => (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0;

export function daysInMonth(year, month) {
  if (month < 1 || month > 12) return 0;
  return month === 2 && isLeapYear(year) ? 29 : MONTH_LENGTHS[month - 1];
}

export function addMonthsClamped(iso, n) {
  const [y, m, d] = String(iso).split('-').map(Number);
  const total = m - 1 + n;
  const year = y + Math.floor(total / 12);
  const month = (((total % 12) + 12) % 12) + 1;
  return `${String(year).padStart(4, '0')}-${pad2(month)}-${pad2(Math.min(d, daysInMonth(year, month)))}`;
}

export function isValidISODate(value) {
  if (typeof value !== 'string' || !/^\d{4}-\d{2}-\d{2}$/.test(value)) return false;
  const [y, m, d] = value.split('-').map(Number);
  return m >= 1 && m <= 12 && d >= 1 && d <= daysInMonth(y, m);
}

export function daysBetween(a, b) {
  const parse = (iso) => {
    const [y, m, d] = String(iso).split('-').map(Number);
    return new Date(y, m - 1, d);
  };
  return Math.round((parse(b) - parse(a)) / 86400000);
}

export function formatDate(iso, { year = 'auto' } = {}) {
  if (!iso) return '';
  const [y, m, d] = String(iso).split('-').map(Number);
  const ms = monthsShort();
  if (!ms[m - 1] || !Number.isFinite(d)) return String(iso);
  const showYear = year === 'always' || (year === 'auto' && y !== new Date().getFullYear());
  return showYear
    ? t('{month} {day}, {year}', { month: ms[m - 1], day: d, year: y })
    : t('{month} {day}', { month: ms[m - 1], day: d });
}

export function formatClock(hhmm) {
  if (!hhmm) return '';
  const [h, m] = String(hhmm).split(':').map(Number);
  if (!Number.isFinite(h) || !Number.isFinite(m)) return '';
  const suffix = h < 12 ? 'am' : 'pm';
  const hour = h % 12 === 0 ? 12 : h % 12;
  return m ? `${hour}:${pad2(m)}${suffix}` : `${hour}${suffix}`;
}

export function formatMonth(key, { year = 'always' } = {}) {
  const [y, m] = String(key).split('-').map(Number);
  const ms = monthsShort();
  const month = ms[m - 1] || key;
  const showYear = year === 'always' || (year === 'auto' && y !== new Date().getFullYear());
  return showYear ? t('{month} {year}', { month, year: y }) : month;
}

export function dueLabel(iso, today = todayISO()) {
  const diff = daysBetween(today, iso);
  if (diff < 0) return { text: t('{n}d overdue', { n: -diff }), status: 'overdue' };
  if (diff === 0) return { text: t('Today'), status: 'today' };
  if (diff === 1) return { text: t('Tomorrow'), status: 'soon' };
  if (diff <= 7) return { text: t('In {n}d', { n: diff }), status: 'soon' };
  return { text: formatDate(iso), status: 'later' };
}

export function sinceLabel(iso, today = todayISO()) {
  const days = Math.max(0, daysBetween(iso, today));
  if (days === 0) return { text: t('today'), days };
  if (days === 1) return { text: t('yesterday'), days };
  if (days < 14) return { text: t('{n}d ago', { n: days }), days };
  if (days < 60) return { text: t('{n}w ago', { n: Math.round(days / 7) }), days };
  return { text: t('{n}mo ago', { n: Math.round(days / 30) }), days };
}

export function timeAgo(ts, now = Date.now()) {
  const s = Math.max(0, Math.floor((now - ts) / 1000));
  if (s < 60) return t('just now');
  if (s < 3600) return t('{n}m ago', { n: Math.floor(s / 60) });
  if (s < 86400) return t('{n}h ago', { n: Math.floor(s / 3600) });
  if (s < 86400 * 7) return t('{n}d ago', { n: Math.floor(s / 86400) });
  return formatDate(todayISO(new Date(ts)));
}

const MONEY_LOCALE = 'en-US';
const trim1 = (n) => (Math.round(n * 10) / 10).toString();

function symbolOf(currency) {
  try {
    const parts = new Intl.NumberFormat(MONEY_LOCALE, { style: 'currency', currency }).formatToParts(1);
    return parts.find((p) => p.type === 'currency')?.value ?? `${currency} `;
  } catch {
    return `${currency} `;
  }
}

export function formatMoney(amount, currency = 'USD') {
  const n = Number(amount) || 0;
  const digits = Number.isInteger(n) ? 0 : 2;
  try {
    return new Intl.NumberFormat(MONEY_LOCALE, {
      style: 'currency',
      currency,
      minimumFractionDigits: digits,
      maximumFractionDigits: digits,
    }).format(n);
  } catch {
    return `${currency} ${n.toLocaleString(MONEY_LOCALE, {
      minimumFractionDigits: digits,
      maximumFractionDigits: digits,
    })}`;
  }
}

export function compactMoney(amount, currency = 'USD') {
  const n = Number(amount) || 0;
  const abs = Math.abs(n);
  const sym = symbolOf(currency);
  const sign = n < 0 ? '-' : '';
  if (abs >= 999_950_000) return `${sign}${sym}${trim1(abs / 1e9)}B`;
  if (abs >= 999_950) return `${sign}${sym}${trim1(abs / 1e6)}M`;
  if (abs >= 10000) return `${sign}${sym}${trim1(abs / 1000)}K`;
  return `${sign}${sym}${abs.toLocaleString(MONEY_LOCALE, { maximumFractionDigits: 0 })}`;
}

export function initials(name) {
  const parts = String(name || '').trim().split(/\s+/).filter(Boolean);
  if (!parts.length) return '?';
  const first = parts[0][0];
  const last = parts.length > 1 ? parts[parts.length - 1][0] : '';
  return (first + last).toUpperCase();
}

export function normalizeText(value) {
  return String(value || '')
    .normalize('NFD')
    .replace(/[\u0300-\u036f]/g, '')
    .toLowerCase();
}

export function fuzzyIncludes(haystack, needle) {
  return normalizeText(haystack).includes(normalizeText(needle));
}

export function matchScore(text, query) {
  const hay = normalizeText(text);
  const needle = normalizeText(query);
  if (!needle) return 0;
  const at = hay.indexOf(needle);
  if (at < 0) return -1;
  if (at === 0) return 100 - Math.min(40, hay.length - needle.length);
  return /[\s\-_/.,(]/.test(hay[at - 1]) ? 60 : 30;
}

export function clamp(n, min, max) {
  return Math.min(max, Math.max(min, n));
}

export function round2(n) {
  return Math.round((Number(n) || 0) * 100) / 100;
}

export const CURRENCIES = ['USD', 'EUR', 'GBP', 'CAD', 'AUD', 'JPY', 'CHF', 'MXN', 'BRL', 'INR'];

export const LEAD_SOURCES = [
  'Referral',
  'Website',
  'Cold outreach',
  'Conference',
  'Social',
  'Inbound',
  'Other',
];
