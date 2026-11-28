/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

export const CONTACT_STATUSES = [
  { id: 'lead', label: 'Lead', tone: 'accent' },
  { id: 'active', label: 'In conversation', tone: 'good' },
  { id: 'customer', label: 'Customer', tone: 'good' },
  { id: 'lost', label: 'Lost touch', tone: 'muted' },
];

export const ACTIVITY_TYPES = [
  { id: 'meeting', label: 'Meeting', icon: 'calendar' },
  { id: 'call', label: 'Call', icon: 'phone' },
  { id: 'email', label: 'Email', icon: 'mail' },
  { id: 'note', label: 'Note', icon: 'fileText' },
];

export function contactStatusInfo(id) {
  return CONTACT_STATUSES.find((s) => s.id === id) || CONTACT_STATUSES[0];
}

export function normalizeContactStatus(id) {
  return CONTACT_STATUSES.some((s) => s.id === id) ? id : CONTACT_STATUSES[0].id;
}

export function activityTypeInfo(id) {
  return ACTIVITY_TYPES.find((t) => t.id === id) || ACTIVITY_TYPES[3];
}

export function normalizeActivityType(id) {
  return ACTIVITY_TYPES.some((t) => t.id === id) ? id : 'note';
}
