/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { coerceFieldValue, formatFieldValue } from './fields.js';
import { resolveStages, stageInfo } from './forecast.js';
import { contractValue, mrrOf, normalizeBilling, normalizeTerm } from './recurring.js';
import { contactStatusInfo, normalizeContactStatus } from './statuses.js';
import { splitTags } from './tags.js';
import { isValidISODate, round2 } from './util.js';

const FORMULA_LEAD = /^(?:[\s\u00A0]*[=+\-@]|[\t\r\n])/;
const PLAIN_NUMBER = /^-?\d+(?:[.,]\d+)?$/;

export function guardFormula(cell) {
  const text = String(cell ?? '');
  if (!text) return '';
  if (FORMULA_LEAD.test(text) && !PLAIN_NUMBER.test(text)) {
    return `'${text}`;
  }
  return text;
}

export function unguardFormula(cell) {
  const text = String(cell ?? '');
  if (text.startsWith("'") && FORMULA_LEAD.test(text.slice(1)) && !PLAIN_NUMBER.test(text.slice(1))) {
    return text.slice(1);
  }
  return text;
}

export function detectDelimiter(text) {
  const clean = String(text ?? '').replace(/^\uFEFF/, '');
  const lines = clean.split(/\r\n|\n|\r/).filter((l) => l.trim().length > 0);
  const first = lines[0] || '';
  const counts = { ',': 0, ';': 0, '\t': 0 };
  let inQuotes = false;
  for (let i = 0; i < first.length; i++) {
    const ch = first[i];
    if (ch === '"') {
      if (inQuotes && first[i + 1] === '"') {
        i++;
      } else {
        inQuotes = !inQuotes;
      }
    } else if (!inQuotes && ch in counts) {
      counts[ch]++;
    }
  }
  let best = ',';
  let bestCount = counts[','];
  for (const delim of [';', '\t']) {
    if (counts[delim] > bestCount) {
      best = delim;
      bestCount = counts[delim];
    }
  }
  return best;
}

export function parseCSV(text, delimiter = null) {
  const clean = String(text ?? '').replace(/^\uFEFF/, '');
  if (!clean.trim()) return [];
  const delim = delimiter || detectDelimiter(clean);
  const rows = [];
  let row = [];
  let cell = '';
  let inQuotes = false;

  for (let i = 0; i < clean.length; i++) {
    const ch = clean[i];
    if (inQuotes) {
      if (ch === '"') {
        if (clean[i + 1] === '"') {
          cell += '"';
          i++;
        } else {
          inQuotes = false;
        }
      } else {
        cell += ch;
      }
      continue;
    }

    if (ch === '"') {
      inQuotes = true;
    } else if (ch === delim) {
      row.push(cell);
      cell = '';
    } else if (ch === '\r' || ch === '\n') {
      if (ch === '\r' && clean[i + 1] === '\n') i++;
      row.push(cell);
      rows.push(row);
      row = [];
      cell = '';
    } else {
      cell += ch;
    }
  }

  if (cell.length > 0 || row.length > 0) {
    row.push(cell);
    rows.push(row);
  }

  return rows.filter((r) => r.some((c) => c.trim().length > 0));
}

export function toCSV(rows, delimiter = ',') {
  const esc = (raw) => {
    const text = String(raw ?? '');
    if (text.includes(delimiter) || text.includes('"') || /[\r\n]/.test(text) || /^\s|\s$/.test(text)) {
      return `"${text.replace(/"/g, '""')}"`;
    }
    return text;
  };
  return (rows || []).map((row) => row.map(esc).join(delimiter)).join('\r\n');
}

export function parseMoney(raw) {
  let s = String(raw ?? '').trim();
  if (!s) return null;
  let negative = false;
  if (s.startsWith('(') && s.endsWith(')')) {
    negative = true;
    s = s.slice(1, -1);
  }
  if (s.endsWith('-')) {
    negative = true;
    s = s.slice(0, -1);
  }
  s = s.replace(/[^\d.,\-+]/g, '');
  if (s.startsWith('-')) {
    negative = true;
    s = s.slice(1);
  } else if (s.startsWith('+')) {
    s = s.slice(1);
  }
  if (!s) return null;

  const lastComma = s.lastIndexOf(',');
  const lastDot = s.lastIndexOf('.');
  if (lastComma > lastDot && /,\d{1,2}$/.test(s)) {
    s = s.replace(/\./g, '').replace(',', '.');
  } else {
    if ((s.match(/\./g) || []).length > 1) s = s.replace(/\./g, '');
    s = s.replace(/,/g, '');
  }
  const n = Number(s);
  if (!Number.isFinite(n)) return null;
  return round2(negative ? -n : n);
}

const MONTH_MAP = {
  jan: 1, january: 1, feb: 2, february: 2, mar: 3, march: 3, apr: 4, april: 4,
  may: 5, jun: 6, june: 6, jul: 7, july: 7, aug: 8, august: 8,
  sep: 9, sept: 9, september: 9, oct: 10, october: 10, nov: 11, november: 11, dec: 12, december: 12,
};

export function parseDateLoose(raw) {
  const s = String(raw ?? '').trim();
  if (!s) return null;
  if (isValidISODate(s)) return s;

  const numMatch = s.match(/^(\d{1,2})([/.])(\d{1,2})\2(\d{2,4})$/);
  if (numMatch) {
    const a = Number(numMatch[1]);
    const sep = numMatch[2];
    const b = Number(numMatch[3]);
    let y = Number(numMatch[4]);
    if (y < 100) y += 2000;
    const pad = (v) => String(v).padStart(2, '0');
    const tryOrder = sep === '.' ? [[b, a], [a, b]] : [[a, b], [b, a]];
    for (const [m, d] of tryOrder) {
      const iso = `${y}-${pad(m)}-${pad(d)}`;
      if (isValidISODate(iso)) return iso;
    }
    return null;
  }

  const m1 = s.match(/^(\d{1,2})\s+([A-Za-z]+),?\s+(\d{4})$/);
  if (m1 && MONTH_MAP[m1[2].toLowerCase()]) {
    const iso = `${m1[3]}-${String(MONTH_MAP[m1[2].toLowerCase()]).padStart(2, '0')}-${String(m1[1]).padStart(2, '0')}`;
    return isValidISODate(iso) ? iso : null;
  }

  const m2 = s.match(/^([A-Za-z]+)\s+(\d{1,2}),?\s+(\d{4})$/);
  if (m2 && MONTH_MAP[m2[1].toLowerCase()]) {
    const iso = `${m2[3]}-${String(MONTH_MAP[m2[1].toLowerCase()]).padStart(2, '0')}-${String(m2[2]).padStart(2, '0')}`;
    return isValidISODate(iso) ? iso : null;
  }

  return null;
}

export function parseBool(raw) {
  return /^(y|yes|true|1|done|complete[d]?|x)$/i.test(String(raw ?? '').trim());
}

export function parseTerm(raw, billing = 'one-time') {
  const s = String(raw ?? '').trim();
  if (!s) return null;
  const num = parseFloat(s.replace(/[^\d.]/g, ''));
  if (!Number.isFinite(num) || num <= 0) return null;
  const months = /y(ea)?rs?\b/i.test(s) ? Math.round(num * 12) : Math.round(num);
  return normalizeTerm(months, billing === 'one-time' ? 'monthly' : billing);
}

export function matchStatus(raw) {
  const s = String(raw ?? '').trim().toLowerCase();
  if (!s) return null;
  if (s === 'lead') return 'lead';
  if (s === 'active' || s === 'in conversation') return 'active';
  if (s === 'customer' || s === 'client') return 'customer';
  if (s === 'lost' || s === 'lost touch') return 'lost';
  return null;
}

export function matchBilling(raw) {
  const s = String(raw ?? '').trim().toLowerCase();
  if (!s) return null;
  if (s === 'monthly' || s === 'mrr' || s === 'month' || s === '/mo') return 'monthly';
  if (s === 'annual' || s === 'arr' || s === 'yearly' || s === 'year' || s === '/yr') return 'annual';
  if (s === 'one-time' || s === 'onetime' || s === 'one-off' || s === 'flat') return 'one-time';
  return null;
}

export function matchStage(raw, stages) {
  const s = String(raw ?? '').trim().toLowerCase();
  if (!s) return null;
  const found = (stages || []).find((st) => st.id.toLowerCase() === s || st.label.toLowerCase() === s);
  return found ? found.id : null;
}

export const IMPORT_FIELDS = {
  contacts: [
    { key: 'name', label: 'Name', required: true, aliases: ['full name', 'contact', 'lead', 'person'] },
    { key: 'title', label: 'Title', aliases: ['job title', 'role', 'position'] },
    { key: 'email', label: 'Email', aliases: ['email address', 'e-mail'] },
    { key: 'phone', label: 'Phone', aliases: ['telephone', 'mobile', 'cell'] },
    { key: 'company', label: 'Company', aliases: ['account', 'organization', 'org'] },
    { key: 'source', label: 'Source', aliases: ['lead source', 'channel'] },
    { key: 'status', label: 'Status', aliases: ['state', 'stage'] },
    { key: 'tags', label: 'Tags', aliases: ['labels'] },
    { key: 'notes', label: 'Notes', aliases: ['note', 'description', 'comments'] },
  ],
  companies: [
    { key: 'name', label: 'Name', required: true, aliases: ['company', 'company name', 'account', 'organization'] },
    { key: 'industry', label: 'Industry', aliases: ['sector', 'vertical'] },
    { key: 'website', label: 'Website', aliases: ['url', 'domain', 'site'] },
    { key: 'size', label: 'Size', aliases: ['employees', 'headcount'] },
    { key: 'tags', label: 'Tags', aliases: ['labels'] },
    { key: 'notes', label: 'Notes', aliases: ['description', 'comments'] },
  ],
  deals: [
    { key: 'name', label: 'Deal name', required: true, aliases: ['deal', 'title', 'opportunity', 'name'] },
    { key: 'value', label: 'Value', aliases: ['amount', 'price', 'rate', 'mrr', 'arr'] },
    { key: 'billing', label: 'Billing', aliases: ['billing cycle', 'cadence', 'type'] },
    { key: 'term', label: 'Term (months)', aliases: ['term', 'months', 'duration'] },
    { key: 'stage', label: 'Stage', aliases: ['pipeline stage', 'status'] },
    { key: 'company', label: 'Company', aliases: ['account'] },
    { key: 'contact', label: 'Contact', aliases: ['lead', 'person', 'primary contact'] },
    { key: 'expectedClose', label: 'Expected close', aliases: ['close date', 'target close', 'expected close date'] },
    { key: 'closeReason', label: 'Close reason', aliases: ['reason', 'win reason', 'loss reason'] },
    { key: 'tags', label: 'Tags', aliases: ['labels'] },
    { key: 'notes', label: 'Notes', aliases: ['description'] },
  ],
};

export function importFieldsFor(kind, customFields = []) {
  const base = IMPORT_FIELDS[kind] || [];
  const custom = (customFields || []).map((f) => ({
    key: `custom:${f.id}`,
    label: f.name,
    field: f,
    aliases: [f.id],
  }));
  return [...base, ...custom];
}

const normHeader = (h) => String(h ?? '').toLowerCase().replace(/[^a-z0-9]+/g, '');

export function autoMap(headers, fields) {
  const mapping = {};
  const usedCols = new Set();
  const normHeaders = headers.map((h) => normHeader(unguardFormula(h)));

  for (const field of fields) {
    const candidates = [field.label, field.key, ...(field.aliases || [])].map(normHeader);
    let idx = normHeaders.findIndex((nh, i) => !usedCols.has(i) && nh && candidates.includes(nh));
    if (idx < 0) {
      idx = normHeaders.findIndex(
        (nh, i) => !usedCols.has(i) && nh && candidates.some((c) => c.length > 2 && (nh.includes(c) || c.includes(nh)))
      );
    }
    mapping[field.key] = idx >= 0 ? idx : -1;
    if (idx >= 0) usedCols.add(idx);
  }
  return mapping;
}

function coerceCustomCell(raw, field) {
  const direct = coerceFieldValue(raw, field.type);
  if (direct !== undefined) return direct;
  if (field.type === 'number') return parseMoney(raw);
  if (field.type === 'date') return parseDateLoose(raw);
  if (field.type === 'checkbox') return parseBool(raw) ? true : null;
  return null;
}

export function mapRows(rows, kind, mapping, { stages = [], customFields = [] } = {}) {
  const fields = importFieldsFor(kind, customFields);
  const records = [];
  let skipped = 0;

  for (const row of rows || []) {
    const out = { custom: {} };
    const cellFor = (key) => {
      const idx = mapping[key];
      if (idx == null || idx < 0) return '';
      return unguardFormula(String(row[idx] ?? '').trim());
    };

    const name = cellFor('name');
    if (!name) {
      skipped++;
      continue;
    }
    out.name = name;

    if (kind === 'contacts') {
      if (cellFor('title')) out.title = cellFor('title');
      if (cellFor('email')) out.email = cellFor('email');
      if (cellFor('phone')) out.phone = cellFor('phone');
      if (cellFor('company')) out.companyName = cellFor('company');
      if (cellFor('source')) out.source = cellFor('source');
      const st = matchStatus(cellFor('status'));
      if (st) out.status = normalizeContactStatus(st);
    } else if (kind === 'companies') {
      if (cellFor('industry')) out.industry = cellFor('industry');
      if (cellFor('website')) out.website = cellFor('website');
      if (cellFor('size')) out.size = cellFor('size');
    } else if (kind === 'deals') {
      const val = parseMoney(cellFor('value'));
      if (val !== null) out.value = val;
      const b = matchBilling(cellFor('billing'));
      if (b) out.billing = normalizeBilling(b);
      const t = parseTerm(cellFor('term'), out.billing || 'one-time');
      if (t !== null) out.term = t;
      const stg = matchStage(cellFor('stage'), stages);
      if (stg) out.stage = stg;
      if (cellFor('company')) out.companyName = cellFor('company');
      if (cellFor('contact')) out.contactName = cellFor('contact');
      const close = parseDateLoose(cellFor('expectedClose'));
      if (close) out.expectedClose = close;
      if (cellFor('closeReason')) out.closeReason = cellFor('closeReason');
    }

    if (cellFor('tags')) out.tags = splitTags(cellFor('tags'));
    if (cellFor('notes')) out.notes = cellFor('notes');

    for (const f of fields) {
      if (!f.field) continue;
      const raw = cellFor(f.key);
      if (!raw) continue;
      const coerced = coerceCustomCell(raw, f.field);
      if (coerced !== null && coerced !== undefined) out.custom[f.field.id] = coerced;
    }

    records.push(out);
  }

  return { records, skipped };
}

export const EXPORT_SETS = [
  { id: 'contacts', label: 'Leads & contacts' },
  { id: 'companies', label: 'Companies' },
  { id: 'deals', label: 'Deals' },
  { id: 'activities', label: 'Activity history' },
  { id: 'tasks', label: 'Follow-ups' },
];

export function customCellText(record, fields) {
  return (fields || []).map((f) => guardFormula(formatFieldValue(f, record?.custom?.[f.id])));
}

export function exportRecords(kind, store, subset = null) {
  const stages = resolveStages(store.settings);
  const custom = store.customFieldsFor(kind);
  const customHeaders = custom.map((f) => guardFormula(f.name));
  const companyName = (id) => (id ? store.company(id)?.name || '' : '');
  const contactName = (id) => (id ? store.contact(id)?.name || '' : '');
  const dealName = (id) => (id ? store.deal(id)?.name || '' : '');

  if (kind === 'contacts') {
    const list = subset || store.contacts;
    const rows = [['Name', 'Title', 'Email', 'Phone', 'Company', 'Source', 'Status', 'Tags', 'Notes', ...customHeaders]];
    for (const c of list) {
      rows.push([
        guardFormula(c.name),
        guardFormula(c.title),
        guardFormula(c.email),
        guardFormula(c.phone),
        guardFormula(companyName(c.companyId)),
        guardFormula(c.source),
        guardFormula(contactStatusInfo(c.status).label),
        guardFormula((c.tags || []).join(', ')),
        guardFormula(c.notes),
        ...customCellText(c, custom),
      ]);
    }
    return toCSV(rows);
  }

  if (kind === 'companies') {
    const list = subset || store.companies;
    const rows = [['Name', 'Industry', 'Website', 'Size', 'Tags', 'Notes', ...customHeaders]];
    for (const co of list) {
      rows.push([
        guardFormula(co.name),
        guardFormula(co.industry),
        guardFormula(co.website),
        guardFormula(co.size),
        guardFormula((co.tags || []).join(', ')),
        guardFormula(co.notes),
        ...customCellText(co, custom),
      ]);
    }
    return toCSV(rows);
  }

  if (kind === 'deals') {
    const list = subset || store.deals;
    const rows = [
      ['Name', 'Value', 'Billing', 'Term (months)', 'Contract value', 'MRR', 'Stage', 'Company', 'Contact', 'Expected close', 'Close reason', 'Tags', 'Notes', ...customHeaders],
    ];
    for (const d of list) {
      rows.push([
        guardFormula(d.name),
        String(d.value ?? 0),
        guardFormula(d.billing || 'one-time'),
        d.term != null ? String(d.term) : '',
        String(contractValue(d)),
        String(mrrOf(d)),
        guardFormula(stageInfo(d.stage, stages).label),
        guardFormula(companyName(d.companyId)),
        guardFormula(contactName(d.contactId)),
        guardFormula(d.expectedClose || ''),
        guardFormula(d.closeReason || ''),
        guardFormula((d.tags || []).join(', ')),
        guardFormula(d.notes),
        ...customCellText(d, custom),
      ]);
    }
    return toCSV(rows);
  }

  if (kind === 'activities') {
    const list = subset || store.activities;
    const rows = [['Date', 'Type', 'Summary', 'Contact', 'Company', 'Deal', 'Notes']];
    for (const a of list) {
      rows.push([
        guardFormula(a.date),
        guardFormula(a.type),
        guardFormula(a.summary),
        guardFormula(contactName(a.contactId)),
        guardFormula(companyName(a.companyId)),
        guardFormula(dealName(a.dealId)),
        guardFormula(a.notes),
      ]);
    }
    return toCSV(rows);
  }

  const list = subset || store.tasks;
  const rows = [['Title', 'Due', 'Time', 'Done', 'Contact', 'Deal']];
  for (const t of list) {
    rows.push([
      guardFormula(t.title),
      guardFormula(t.due),
      guardFormula(t.dueTime || ''),
      t.done ? 'Yes' : 'No',
      guardFormula(contactName(t.contactId)),
      guardFormula(dealName(t.dealId)),
    ]);
  }
  return toCSV(rows);
}
