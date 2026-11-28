/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { normalizeText } from './util.js';

export const MAX_PAIRS = 50;

export function normName(value) {
  return normalizeText(value).replace(/\s+/g, ' ').trim();
}

export function normEmail(value) {
  return String(value ?? '').trim().toLowerCase();
}

export function normPhone(value) {
  const digits = String(value ?? '').replace(/\D+/g, '');
  return digits.length >= 7 ? digits : '';
}

export function normDomain(value) {
  let s = String(value ?? '').trim().toLowerCase();
  if (!s) return '';
  s = s.replace(/^[a-z][a-z0-9+.-]*:\/\//, '');
  s = s.replace(/^www\./, '');
  s = s.split(/[/?#]/)[0];
  return s;
}

function findPairs(records, matchers) {
  const pairMap = new Map();

  for (const { reason, keyOf } of matchers) {
    const buckets = new Map();
    for (const rec of records || []) {
      const k = keyOf(rec);
      if (!k) continue;
      let list = buckets.get(k);
      if (!list) {
        list = [];
        buckets.set(k, list);
      }
      list.push(rec);
    }

    for (const list of buckets.values()) {
      if (list.length < 2) continue;
      const sorted = [...list].sort((a, b) => (a.createdAt || 0) - (b.createdAt || 0));
      const survivor = sorted[0];
      for (let i = 1; i < sorted.length; i++) {
        const other = sorted[i];
        const pairKey = `${survivor.id}|${other.id}`;
        let entry = pairMap.get(pairKey);
        if (!entry) {
          entry = { a: survivor, b: other, reasons: [] };
          pairMap.set(pairKey, entry);
        }
        if (!entry.reasons.includes(reason)) entry.reasons.push(reason);
      }
    }
  }

  return [...pairMap.values()]
    .sort((x, y) => (x.a.createdAt || 0) - (y.a.createdAt || 0) || (x.b.createdAt || 0) - (y.b.createdAt || 0))
    .slice(0, MAX_PAIRS);
}

export function contactDuplicates(contacts) {
  return findPairs(contacts, [
    { reason: 'Same email', keyOf: (c) => normEmail(c.email) },
    { reason: 'Same name', keyOf: (c) => normName(c.name) },
    { reason: 'Same phone', keyOf: (c) => normPhone(c.phone) },
  ]);
}

export function companyDuplicates(companies) {
  return findPairs(companies, [
    { reason: 'Same name', keyOf: (c) => normName(c.name) },
    { reason: 'Same website', keyOf: (c) => normDomain(c.website) },
  ]);
}

export function contactByEmail(contacts, email, excludeId = null) {
  const norm = normEmail(email);
  if (!norm || !norm.includes('@')) return null;
  return (contacts || []).find((c) => c.id !== excludeId && normEmail(c.email) === norm) || null;
}
