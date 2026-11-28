/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { normalizeCustomValues } from './fields.js';
import { t } from './i18n.js';
import { normalizeTags } from './tags.js';
import { uid } from './util.js';

export const VIEW_KINDS = ['contacts', 'companies', 'deals'];
export const MAX_VIEWS = 6;
export const MAX_VIEW_NAME = 40;
export const DEAL_SORT_COLUMNS = ['name', 'link', 'stage', 'value', 'expectedClose', 'updatedAt'];
export const DEFAULT_DEAL_SORT = { key: 'updatedAt', dir: 'desc' };

export function normalizeSort(raw) {
  if (!raw || typeof raw !== 'object' || !raw.key) return null;
  const key = String(raw.key).trim();
  if (!key) return null;
  return { key, dir: raw.dir === 'asc' ? 'asc' : 'desc' };
}

export function normalizeFilters(kind, raw = {}) {
  const base = {
    query: String(raw?.query ?? '').trim(),
    tags: normalizeTags(raw?.tags),
    fields: normalizeCustomValues(raw?.fields),
  };
  if (kind === 'contacts') {
    base.status = String(raw?.status || 'all');
  } else if (kind === 'deals') {
    base.stage = String(raw?.stage || 'open');
    base.closeWindow = String(raw?.closeWindow || 'any');
    base.sort = normalizeSort(raw?.sort);
  }
  return base;
}

export function sanitizeViews(raw) {
  const out = { contacts: [], companies: [], deals: [] };
  if (!raw || typeof raw !== 'object') return out;
  for (const kind of VIEW_KINDS) {
    out[kind] = normalizeViewList(kind, raw[kind]);
  }
  return out;
}

export function normalizeViewList(kind, list) {
  const seen = new Set();
  const out = [];
  let hasDefault = false;
  for (const item of Array.isArray(list) ? list : []) {
    if (!item || typeof item !== 'object') continue;
    const name = String(item.name ?? '').trim().slice(0, MAX_VIEW_NAME);
    if (!name) continue;
    let id = String(item.id ?? '').trim();
    if (!id || seen.has(id)) id = uid();
    seen.add(id);
    const isDefault = Boolean(item.default) && !hasDefault;
    if (isDefault) hasDefault = true;
    out.push({
      id,
      name,
      default: isDefault,
      filters: normalizeFilters(kind, item.filters),
    });
    if (out.length >= MAX_VIEWS) break;
  }
  return out;
}

export function sameFilters(kind, a, b) {
  const norm = (x) => {
    const f = normalizeFilters(kind, x);
    const fieldsSorted = {};
    for (const k of Object.keys(f.fields).sort()) fieldsSorted[k] = f.fields[k];
    const obj = {
      ...f,
      tags: [...f.tags].map((t) => t.toLowerCase()).sort(),
      fields: fieldsSorted,
    };
    if (kind === 'deals' && !obj.sort) obj.sort = DEFAULT_DEAL_SORT;
    return JSON.stringify(obj);
  };
  return norm(a) === norm(b);
}

export function hasActiveFilters(kind, raw) {
  const f = normalizeFilters(kind, raw);
  if (f.query || f.tags.length > 0 || Object.keys(f.fields).length > 0) return true;
  if (kind === 'contacts' && f.status !== 'all') return true;
  if (kind === 'deals' && (f.stage !== 'open' || f.closeWindow !== 'any' || (f.sort && JSON.stringify(f.sort) !== JSON.stringify(DEFAULT_DEAL_SORT)))) {
    return true;
  }
  return false;
}

export function applyFilters(kind, raw, context = {}) {
  const filters = normalizeFilters(kind, raw);
  const dropped = [];

  if (Array.isArray(context.tags)) {
    const knownTags = new Set(context.tags.map((x) => String(x).toLowerCase()));
    filters.tags = filters.tags.filter((tag) => {
      const ok = knownTags.has(tag.toLowerCase());
      if (!ok) dropped.push(t('the tag “{tag}”', { tag }));
      return ok;
    });
  }

  if (Array.isArray(context.fields)) {
    const knownFields = new Set(context.fields.map((f) => (typeof f === 'string' ? f : f.id)));
    const nextFields = {};
    for (const [k, v] of Object.entries(filters.fields)) {
      if (knownFields.has(k)) {
        nextFields[k] = v;
      } else {
        dropped.push(t('a field that was removed'));
      }
    }
    filters.fields = nextFields;
  }

  if (kind === 'deals') {
    if (Array.isArray(context.stages)) {
      const allowedStages = new Set([...context.stages.map((s) => (typeof s === 'string' ? s : s.id)), 'open', 'all']);
      if (!allowedStages.has(filters.stage)) {
        filters.stage = 'open';
        dropped.push(t('a stage that was removed'));
      }
    }
    if (filters.sort && Array.isArray(context.sortKeys)) {
      if (!context.sortKeys.includes(filters.sort.key)) {
        filters.sort = null;
        dropped.push(t('a sort column that no longer exists'));
      }
    }
  }

  return { filters, dropped: [...new Set(dropped)] };
}

export function defaultView(views, kind) {
  return (views?.[kind] || []).find((v) => v.default) || null;
}

export function matchingView(views, kind, currentFilters) {
  return (views?.[kind] || []).find((v) => sameFilters(kind, v.filters, currentFilters)) || null;
}
