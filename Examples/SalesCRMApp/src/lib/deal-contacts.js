/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

export const MAX_DEAL_CONTACTS = 8;

export const DEAL_ROLES = [
  { id: 'champion', label: 'Champion' },
  { id: 'decision-maker', label: 'Decision maker' },
  { id: 'influencer', label: 'Influencer' },
  { id: 'blocker', label: 'Blocker' },
  { id: 'other', label: 'Other' },
];

const ROLE_IDS = new Set(DEAL_ROLES.map((r) => r.id));

export function normalizeRole(role) {
  const r = String(role || '').trim();
  return ROLE_IDS.has(r) ? r : '';
}

export function roleInfo(role) {
  return DEAL_ROLES.find((r) => r.id === role) || null;
}

export function normalizeDealContacts(rawContacts, fallbackPrimaryId = null) {
  const out = [];
  const seen = new Set();
  for (const entry of Array.isArray(rawContacts) ? rawContacts : []) {
    const cid = typeof entry === 'string' ? entry : entry?.contactId;
    if (!cid || seen.has(cid)) continue;
    seen.add(cid);
    out.push({ contactId: cid, role: normalizeRole(entry?.role) });
    if (out.length >= MAX_DEAL_CONTACTS) break;
  }
  if (fallbackPrimaryId && !seen.has(fallbackPrimaryId)) {
    out.unshift({ contactId: fallbackPrimaryId, role: '' });
    if (out.length > MAX_DEAL_CONTACTS) out.length = MAX_DEAL_CONTACTS;
  }
  return out;
}

export function primaryContactId(deal) {
  return deal?.contacts?.[0]?.contactId ?? deal?.contactId ?? null;
}

export function dealContactIds(deal) {
  if (Array.isArray(deal?.contacts) && deal.contacts.length > 0) {
    return deal.contacts.map((c) => c.contactId).filter(Boolean);
  }
  return deal?.contactId ? [deal.contactId] : [];
}

export function hasContact(deal, contactId) {
  return dealContactIds(deal).includes(contactId);
}

export function hasDecisionMaker(deal) {
  return Boolean(deal?.contacts?.some((c) => c.role === 'decision-maker'));
}

export function replacePrimary(existingContacts, nextPrimaryId) {
  const rest = normalizeDealContacts(existingContacts).filter((c) => c.contactId !== nextPrimaryId);
  if (!nextPrimaryId) return rest;
  const existingEntry = (existingContacts || []).find((c) => c.contactId === nextPrimaryId);
  return [{ contactId: nextPrimaryId, role: normalizeRole(existingEntry?.role) }, ...rest].slice(0, MAX_DEAL_CONTACTS);
}

export function withContact(existingContacts, contactId, role = '') {
  if (!contactId) return normalizeDealContacts(existingContacts);
  const current = normalizeDealContacts(existingContacts);
  if (current.some((c) => c.contactId === contactId)) return current;
  return [...current, { contactId, role: normalizeRole(role) }].slice(0, MAX_DEAL_CONTACTS);
}

export function withoutContact(existingContacts, contactId) {
  return normalizeDealContacts(existingContacts).filter((c) => c.contactId !== contactId);
}

export function repointContact(existingContacts, fromId, toId) {
  const list = normalizeDealContacts(existingContacts);
  const merged = list.find((c) => c.contactId === fromId);
  if (!merged) return list;
  const survivor = list.find((c) => c.contactId === toId);
  if (survivor) {
    if (!survivor.role && merged.role) survivor.role = merged.role;
    return list.filter((c) => c.contactId !== fromId);
  }
  return list.map((c) => (c.contactId === fromId ? { contactId: toId, role: c.role } : c));
}
