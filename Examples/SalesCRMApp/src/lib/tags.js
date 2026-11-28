/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

export const MAX_TAG_LENGTH = 40;
export const MAX_TAGS_PER_RECORD = 20;

export function cleanTag(raw) {
  return String(raw ?? '')
    .trim()
    .replace(/\s+/g, ' ')
    .slice(0, MAX_TAG_LENGTH);
}

export function dedupeTags(list) {
  const seen = new Set();
  const out = [];
  for (const raw of list || []) {
    const tag = cleanTag(raw);
    if (!tag) continue;
    const key = tag.toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(tag);
    if (out.length >= MAX_TAGS_PER_RECORD) break;
  }
  return out;
}

export function normalizeTags(list) {
  if (!Array.isArray(list)) return [];
  return dedupeTags(list);
}

export function splitTags(raw) {
  return dedupeTags(String(raw ?? '').split(/[,;]/));
}

export function tagKey(tag) {
  return cleanTag(tag).toLowerCase();
}
