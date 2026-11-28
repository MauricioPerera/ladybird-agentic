/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { rateText } from './recurring.js';

export const MERGE_FIELDS = [
  { token: '{{firstName}}', label: 'First name' },
  { token: '{{name}}', label: 'Full name' },
  { token: '{{company}}', label: 'Company' },
  { token: '{{dealName}}', label: 'Deal name' },
  { token: '{{dealValue}}', label: 'Deal value' },
];

export function templateContext({ contact = null, company = null, deal = null, currency = 'USD' } = {}) {
  const fullName = String(contact?.name || '').trim();
  const firstName = fullName ? fullName.split(/\s+/)[0] : '';
  const dealValue = deal && Number(deal.value) ? rateText(deal, currency) : '';
  return {
    firstName,
    name: fullName,
    company: String(company?.name || '').trim(),
    dealName: String(deal?.name || '').trim(),
    dealValue,
  };
}

export function fillTemplate(text, ctx = {}) {
  return String(text ?? '').replace(/\{\{\s*(\w+)\s*\}\}/g, (full, key) => {
    if (Object.prototype.hasOwnProperty.call(ctx, key)) {
      return String(ctx[key] ?? '');
    }
    return full;
  });
}

export function buildMailto(email, subject = '', body = '') {
  const to = encodeURIComponent(String(email || '').trim());
  const params = [];
  if (subject) params.push(`subject=${encodeURIComponent(subject)}`);
  if (body) params.push(`body=${encodeURIComponent(body)}`);
  return `mailto:${to}${params.length ? `?${params.join('&')}` : ''}`;
}

export function buildTel(phone) {
  const raw = String(phone || '').trim();
  if (!raw) return '';
  const leadPlus = raw.startsWith('+') ? '+' : '';
  const clean = raw.slice(leadPlus.length).replace(/[^\d\-.()]/g, '');
  return clean ? `tel:${leadPlus}${clean}` : '';
}
