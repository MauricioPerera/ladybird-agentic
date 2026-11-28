/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import es from './locales/es.js';
import pt from './locales/pt.js';

export const LANG_STORAGE_KEY = 'crm:lang';

export const SUPPORTED_LANGUAGES = [
  { id: 'auto', labelKey: 'System' },
  { id: 'en', label: 'English' },
  { id: 'es', label: 'Español' },
  { id: 'pt', label: 'Português' },
];

const DICTS = { es, pt };

const MONTHS = {
  en: ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'],
  es: ['Ene', 'Feb', 'Mar', 'Abr', 'May', 'Jun', 'Jul', 'Ago', 'Sep', 'Oct', 'Nov', 'Dic'],
  pt: ['Jan', 'Fev', 'Mar', 'Abr', 'Mai', 'Jun', 'Jul', 'Ago', 'Set', 'Out', 'Nov', 'Dez'],
};

const WEEKDAYS = {
  en: ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'],
  es: ['Lun', 'Mar', 'Mié', 'Jue', 'Vie', 'Sáb', 'Dom'],
  pt: ['Seg', 'Ter', 'Qua', 'Qui', 'Sex', 'Sáb', 'Dom'],
};

const BUILTIN_STAGE_KEYS = {
  new: 'New_stage',
  qualified: 'Qualified',
  proposal: 'Proposal',
  negotiation: 'Negotiation',
  won: 'Won',
  lost: 'Lost',
};

const BUILTIN_STAGE_LABELS = {
  New: 'New_stage',
  Qualified: 'Qualified',
  Proposal: 'Proposal',
  Negotiation: 'Negotiation',
  Won: 'Won',
  Lost: 'Lost',
};

const BUILTIN_REASONS = new Set([
  'Referral strength',
  'Product fit',
  'Price',
  'Relationship',
  'Timing',
  'Competitor',
  'No budget',
  'Went silent',
  'No fit',
  'Other',
]);

const BUILTIN_SOURCES = new Set(['Referral', 'Website', 'Cold outreach', 'Conference', 'Social', 'Inbound', 'Other']);

export function normalizeLanguage(raw) {
  const s = String(raw || '').trim().toLowerCase();
  if (!s || s === 'auto' || s === 'system') return 'auto';
  const base = s.split(/[-_]/)[0];
  if (base === 'en' || base === 'es' || base === 'pt') return base;
  return 'auto';
}

export function resolveLanguage(pref = 'auto') {
  const norm = normalizeLanguage(pref);
  if (norm !== 'auto') return norm;
  if (typeof navigator !== 'undefined') {
    const candidates = [...(navigator.languages || []), navigator.language].filter(Boolean);
    for (const c of candidates) {
      const code = String(c).toLowerCase().split(/[-_]/)[0];
      if (code === 'en' || code === 'es' || code === 'pt') return code;
    }
  }
  return 'en';
}

function readStoredLanguage() {
  try {
    return typeof localStorage !== 'undefined' ? localStorage.getItem(LANG_STORAGE_KEY) || 'auto' : 'auto';
  } catch {
    return 'auto';
  }
}

let langPref = normalizeLanguage(readStoredLanguage());
let activeLang = resolveLanguage(langPref);
if (typeof document !== 'undefined' && document.documentElement) {
  document.documentElement.lang = activeLang;
}

export function setLanguage(pref = 'auto') {
  langPref = normalizeLanguage(pref);
  activeLang = resolveLanguage(langPref);
  try {
    if (typeof localStorage !== 'undefined') localStorage.setItem(LANG_STORAGE_KEY, langPref);
  } catch {}
  if (typeof document !== 'undefined' && document.documentElement) {
    document.documentElement.lang = activeLang;
  }
  return activeLang;
}

export function getLanguage() {
  return activeLang;
}

export function getLanguagePreference() {
  return langPref;
}

export function t(key, vars = null) {
  let out = activeLang === 'en' ? key : DICTS[activeLang]?.[key] ?? key;
  if (out === 'New_stage') out = 'New';
  else if (out === 'Monthly_rep') out = 'Monthly';
  else if (out === 'Activity_rec') out = 'Activity';
  else if (out === 'Edit activity_aria') out = 'Edit activity';
  else if (out === 'Week_mode') out = 'Week';
  else if (out === 'Month_mode') out = 'Month';
  else if (out === 'This month_cal') out = 'This month';
  else if (out === 'This week_cal') out = 'This week';
  if (vars && typeof vars === 'object') {
    out = out.replace(/\{([a-zA-Z0-9_]+)\}/g, (_, k) => (vars[k] !== undefined && vars[k] !== null ? String(vars[k]) : ''));
  }
  return out;
}

export function monthsShort() {
  return MONTHS[activeLang] || MONTHS.en;
}

export function weekdaysShort() {
  return WEEKDAYS[activeLang] || WEEKDAYS.en;
}

export function stageLabel(stageOrId, stages = null) {
  const id = typeof stageOrId === 'string' ? stageOrId : stageOrId?.id;
  let label = typeof stageOrId === 'object' ? stageOrId?.label : null;
  if (!label && Array.isArray(stages)) {
    label = stages.find((s) => s.id === id)?.label || '';
  }
  if (!label) {
    const key = BUILTIN_STAGE_KEYS[id];
    return key ? t(key) : String(id || '');
  }
  if (BUILTIN_STAGE_KEYS[id] && BUILTIN_STAGE_LABELS[label]) {
    return t(BUILTIN_STAGE_LABELS[label]);
  }
  return label;
}

export function reasonLabel(reason) {
  const s = String(reason || '').trim();
  if (!s) return '';
  return BUILTIN_REASONS.has(s) ? t(s) : s;
}

export function leadSourceLabel(source) {
  const s = String(source || '').trim();
  if (!s) return '';
  return BUILTIN_SOURCES.has(s) ? t(s) : s;
}
