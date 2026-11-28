/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from './i18n.js';
import { isValidISODate, normalizeText, uid } from './util.js';

export const CUSTOM_KINDS = ['contacts', 'companies', 'deals'];
export const MAX_CUSTOM_FIELDS = 10;
export const MAX_FIELD_OPTIONS = 20;
export const MAX_FIELD_NAME = 40;
export const MAX_OPTION_LENGTH = 40;

export const FIELD_TYPES = [
  { id: 'text', label: 'Text', hasOptions: false },
  { id: 'number', label: 'Number', hasOptions: false },
  { id: 'date', label: 'Date', hasOptions: false },
  { id: 'select', label: 'Select', hasOptions: true },
  { id: 'checkbox', label: 'Checkbox', hasOptions: false },
];

const TYPE_IDS = new Set(FIELD_TYPES.map((t) => t.id));
const TRUE_WORDS = new Set(['true', '1', 'yes', 'y', 'on', 'done']);
const FALSE_WORDS = new Set(['false', '0', 'no', 'n', 'off']);

export function makeFieldId(name, taken = new Set()) {
  const base =
    normalizeText(name)
      .replace(/[^a-z0-9]+/g, '-')
      .replace(/^-+|-+$/g, '') || 'field';
  let id = base;
  let n = 2;
  while (taken.has(id)) {
    id = `${base}-${n++}`;
  }
  taken.add(id);
  return id;
}

export function blankField(taken = new Set()) {
  return {
    id: makeFieldId(`New field-${uid().slice(0, 4)}`, taken),
    name: 'New field',
    type: 'text',
  };
}

export function sanitizeFieldOptions(raw) {
  const arr = Array.isArray(raw) ? raw : String(raw ?? '').split(/[,;\n]/);
  const seen = new Set();
  const out = [];
  for (const item of arr) {
    const clean = String(item ?? '').trim().slice(0, MAX_OPTION_LENGTH);
    if (!clean) continue;
    const key = clean.toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(clean);
    if (out.length >= MAX_FIELD_OPTIONS) break;
  }
  return out;
}

export function sanitizeCustomFields(raw) {
  const out = { contacts: [], companies: [], deals: [] };
  if (!raw || typeof raw !== 'object') return out;
  for (const kind of CUSTOM_KINDS) {
    const list = Array.isArray(raw[kind]) ? raw[kind] : [];
    const seen = new Set();
    const cleaned = [];
    for (const item of list) {
      if (!item || typeof item !== 'object') continue;
      const name = String(item.name ?? '').trim().slice(0, MAX_FIELD_NAME);
      if (!name) continue;
      const type = TYPE_IDS.has(item.type) ? item.type : 'text';
      let id = String(item.id ?? '').trim();
      if (!id || seen.has(id)) id = makeFieldId(name, seen);
      else seen.add(id);
      const def = { id, name, type };
      if (type === 'select') def.options = sanitizeFieldOptions(item.options);
      cleaned.push(def);
      if (cleaned.length >= MAX_CUSTOM_FIELDS) break;
    }
    out[kind] = cleaned;
  }
  return out;
}

export function normalizeCustomValues(raw) {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return {};
  const out = {};
  for (const [k, v] of Object.entries(raw)) {
    if (v === null || v === undefined || v === '') continue;
    if (typeof v === 'boolean') {
      out[k] = v;
    } else if (typeof v === 'number') {
      if (Number.isFinite(v)) out[k] = v;
    } else if (typeof v === 'string') {
      const trimmed = v.trim().slice(0, 4096);
      if (trimmed) out[k] = trimmed;
    }
  }
  return out;
}

export function coerceFieldValue(value, targetType) {
  if (value === null || value === undefined || value === '') return null;
  if (typeof value === 'string' && value.trim() === '') return null;

  if (targetType === 'number') {
    if (typeof value === 'number') return Number.isFinite(value) ? value : undefined;
    const raw = String(value).replace(/[\s,]+/g, '');
    if (!/^-?\d*\.?\d+$/.test(raw)) return undefined;
    const n = Number(raw);
    return Number.isFinite(n) ? n : undefined;
  }

  if (targetType === 'date') {
    const s = String(value).trim();
    return isValidISODate(s) ? s : undefined;
  }

  if (targetType === 'checkbox') {
    if (typeof value === 'boolean') return value;
    if (value === 1 || value === 0) return Boolean(value);
    const low = String(value).trim().toLowerCase();
    if (TRUE_WORDS.has(low)) return true;
    if (FALSE_WORDS.has(low)) return false;
    return undefined;
  }

  if (typeof value === 'boolean') return value ? 'Yes' : 'No';
  if (typeof value === 'number') return Number.isFinite(value) ? String(value) : undefined;
  const text = String(value).trim();
  return text ? text : null;
}

export function retypeReport(records, field, nextType) {
  const blocked = [];
  const nextValues = [];
  for (const rec of records || []) {
    const current = rec?.custom?.[field.id];
    if (current === null || current === undefined || current === '') continue;
    const converted = coerceFieldValue(current, nextType);
    if (converted === undefined) {
      blocked.push({ id: rec.id, name: rec.name || 'Untitled', value: current });
    } else {
      nextValues.push({ id: rec.id, value: converted });
    }
  }
  return { blocked, nextValues };
}

export function mergeCustomValues(survivorCustom, otherCustom) {
  const base = normalizeCustomValues(survivorCustom);
  const extra = normalizeCustomValues(otherCustom);
  for (const [k, v] of Object.entries(extra)) {
    if (!(k in base)) base[k] = v;
  }
  return base;
}

export function formatFieldValue(field, value) {
  if (value === null || value === undefined || value === '') return '';
  if (field.type === 'checkbox') return value ? t('Yes') : t('No');
  return String(value);
}
