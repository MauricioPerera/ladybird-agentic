/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from './i18n.js';
import { addDays, addMonthsClamped, clamp, todayISO } from './util.js';

export const REPEAT_UNITS = ['day', 'week', 'month'];

export function normalizeRepeat(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const unit = String(raw.unit || '').toLowerCase();
  if (!REPEAT_UNITS.includes(unit)) return null;
  const every = clamp(Math.round(Number(raw.every) || 0), 1, 365);
  return { every, unit };
}

export function nextRepeatDue(fromISO = todayISO(), repeat) {
  const norm = normalizeRepeat(repeat);
  if (!norm) return null;
  if (norm.unit === 'day') return addDays(fromISO, norm.every);
  if (norm.unit === 'week') return addDays(fromISO, norm.every * 7);
  return addMonthsClamped(fromISO, norm.every);
}

export function repeatLabel(repeat) {
  const norm = normalizeRepeat(repeat);
  if (!norm) return '';
  if (norm.every === 1) {
    if (norm.unit === 'day') return t('Daily');
    if (norm.unit === 'week') return t('Weekly');
    return t('Monthly_rep');
  }
  if (norm.unit === 'day') return t('Every {n} days', { n: norm.every });
  if (norm.unit === 'week') return t('Every {n} weeks', { n: norm.every });
  return t('Every {n} months', { n: norm.every });
}
