/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import es from './locales/es.js';
import pt from './locales/pt.js';

const DICTS = { es, pt };

export const SUPPORTED_LANGUAGES = [
  { id: 'system', label: 'System' },
  { id: 'en', label: 'English' },
  { id: 'es', label: 'Español' },
  { id: 'pt', label: 'Português' },
];

let preference = 'system';
let activeLang = detectSystemLang();

export function detectSystemLang() {
  const raw = (typeof navigator !== 'undefined' && (navigator.language || navigator.userLanguage)) || 'en';
  const base = String(raw).toLowerCase().split(/[-_]/)[0];
  return base === 'es' || base === 'pt' ? base : 'en';
}

export function normalizeLanguage(pref) {
  const p = String(pref || '').toLowerCase();
  if (p === 'en' || p === 'es' || p === 'pt') return p;
  return 'system';
}

export function setLanguage(pref) {
  preference = normalizeLanguage(pref);
  activeLang = preference === 'system' ? detectSystemLang() : preference;
  if (typeof document !== 'undefined' && document.documentElement) {
    document.documentElement.lang = activeLang;
  }
  return activeLang;
}

export function getLanguage() {
  return activeLang;
}

export function getLanguagePreference() {
  return preference;
}

export function t(key, vars = null) {
  const dict = DICTS[activeLang];
  const raw = (dict && Object.prototype.hasOwnProperty.call(dict, key)) ? dict[key] : key;
  if (!vars) return raw;
  return String(raw).replace(/\{(\w+)\}/g, (_, k) => (k in vars && vars[k] !== undefined && vars[k] !== null ? String(vars[k]) : `{${k}}`));
}
