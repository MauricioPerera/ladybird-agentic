/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

function toGCalDateTime(isoDate, timeStr) {
  const clean = String(isoDate || '').replace(/-/g, '');
  if (!timeStr || !/^\d{2}:\d{2}$/.test(timeStr)) return clean;
  return `${clean}T${timeStr.replace(':', '')}00`;
}

function addMinutes(isoDate, timeStr, minutes = 30) {
  const [y, m, d] = String(isoDate || '').split('-').map(Number);
  const [hh, mm] = String(timeStr || '09:00').split(':').map(Number);
  if (!y || !m || !d) return { date: isoDate, time: timeStr };
  const dt = new Date(y, m - 1, d, hh, mm + Number(minutes || 30), 0);
  const pad = (n) => String(n).padStart(2, '0');
  return {
    date: `${dt.getFullYear()}-${pad(dt.getMonth() + 1)}-${pad(dt.getDate())}`,
    time: `${pad(dt.getHours())}:${pad(dt.getMinutes())}`,
  };
}

function todayISO() {
  const d = new Date();
  const pad = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
}

export const BUILTIN_SITE_ACTIONS = [
  {
    id: 'gmail.read_inbox',
    serviceId: 'mail',
    name: 'Gmail: Read Inbox',
    category: 'data',
    defaultSaveAs: 'inbox',
    description: 'Extract latest email rows (subject, sender, date, snippet) from your Gmail tab.',
    fields: [
      { key: 'limit', label: 'Max emails', type: 'number', default: 20, min: 1, max: 50 },
      { key: 'filterText', label: 'Filter text (optional)', type: 'text', default: '', placeholder: 'invoice, meeting...' },
    ],
    compile: (params = {}, saveAs = 'inbox') => {
      const limit = Math.max(1, Math.min(50, Number(params.limit) || 20));
      const filter = String(params.filterText || '').trim().toLowerCase();
      return [
        {
          action: 'wait_for',
          label: 'Wait for Gmail rows',
          locator: { kind: 'css', value: 'tr.zA, [role="row"]', exact: false },
          timeout_ms: 15000,
        },
        {
          action: 'eval',
          label: 'Extract inbox rows',
          save_as: saveAs || 'inbox',
          script: `return (() => {
  const limit = ${limit};
  const filterQ = ${JSON.stringify(filter)};
  const rows = [];
  const seen = new Set();
  const els = document.querySelectorAll('tr.zA, [role="row"]');
  for (let i = 0; i < els.length && rows.length < limit; i++) {
    const r = els[i];
    const txt = (r.innerText || '').trim();
    if (!txt || txt.length < 6) continue;
    if (filterQ && !txt.toLowerCase().includes(filterQ)) continue;
    const subjEl = r.querySelector('.bog, .bqe, [data-thread-id], [role="link"]');
    const fromEl = r.querySelector('.yW span[email], span[email], .zF, .yP');
    const bodyEl = r.querySelector('.y2');
    const dateEl = r.querySelector('td.xW span[title], td.xW span');
    const subject = (subjEl ? subjEl.textContent : txt.split('\\n')[0] || '').trim();
    const senderEmail = fromEl ? (fromEl.getAttribute('email') || '').trim() : '';
    const senderName = fromEl ? (fromEl.textContent || '').trim() : '';
    const sender = senderEmail || senderName;
    const snippet = (bodyEl ? bodyEl.textContent : '').replace(/^\\s*[-\\u2013\\u2014]+\\s*/, '').trim();
    const date = dateEl ? (dateEl.getAttribute('title') || dateEl.textContent || '').trim() : '';
    const key = subject + '|' + sender;
    if (!subject || seen.has(key)) continue;
    seen.add(key);
    rows.push({ subject, sender, senderName, date, snippet });
  }
  return { count: rows.length, filter: filterQ || null, scannedAt: new Date().toISOString(), rows };
})();`,
        },
      ];
    },
  },
  {
    id: 'gmail.search',
    serviceId: 'mail',
    name: 'Gmail: Search Emails',
    category: 'nav',
    defaultSaveAs: 'search_results',
    description: 'Search inside Gmail for a query and extract matching conversation rows.',
    fields: [
      { key: 'query', label: 'Search query', type: 'text', default: 'is:unread', placeholder: 'from:client@acme.com' },
      { key: 'limit', label: 'Max results', type: 'number', default: 15, min: 1, max: 40 },
    ],
    compile: (params = {}, saveAs = 'search_results') => {
      const q = String(params.query || 'is:unread').trim();
      const limit = Math.max(1, Math.min(40, Number(params.limit) || 15));
      const url = q ? `https://mail.google.com/mail/u/0/#search/${encodeURIComponent(q)}` : 'https://mail.google.com/mail/u/0/#inbox';
      return [
        { action: 'open', label: `Search: ${q}`, url, wait_for_load: true, timeout_ms: 15000 },
        { action: 'sleep', label: 'Wait for search render', ms: 1600 },
        {
          action: 'eval',
          label: 'Extract matching threads',
          save_as: saveAs || 'search_results',
          script: `return (() => {
  const limit = ${limit};
  const rows = [];
  const seen = new Set();
  const els = document.querySelectorAll('tr.zA, [role="row"]');
  for (let i = 0; i < els.length && rows.length < limit; i++) {
    const r = els[i];
    const txt = (r.innerText || '').trim();
    if (!txt || txt.length < 6) continue;
    const subjEl = r.querySelector('.bog, .bqe, [data-thread-id], [role="link"]');
    const fromEl = r.querySelector('.yW span[email], span[email], .zF, .yP');
    const bodyEl = r.querySelector('.y2');
    const subject = (subjEl ? subjEl.textContent : txt.split('\\n')[0] || '').trim();
    const sender = fromEl ? (fromEl.getAttribute('email') || fromEl.textContent || '').trim() : '';
    const snippet = (bodyEl ? bodyEl.textContent : '').replace(/^\\s*[-\\u2013\\u2014]+\\s*/, '').trim();
    const key = subject + '|' + sender;
    if (!subject || seen.has(key)) continue;
    seen.add(key);
    rows.push({ subject, sender, snippet });
  }
  return { query: ${JSON.stringify(q)}, count: rows.length, rows };
})();`,
        },
      ];
    },
  },
  {
    id: 'gmail.compose_draft',
    serviceId: 'mail',
    name: 'Gmail: Compose Email',
    category: 'act',
    defaultSaveAs: 'compose',
    description: 'Open a pre-filled Gmail compose window ready to review or send.',
    fields: [
      { key: 'to', label: 'Recipient email', type: 'text', default: '', placeholder: 'client@company.com' },
      { key: 'subject', label: 'Subject', type: 'text', default: '', placeholder: 'Follow-up {{inbox.rows.0.subject}}' },
      { key: 'body', label: 'Message body', type: 'textarea', default: '' },
    ],
    compile: (params = {}, saveAs = 'compose') => {
      const qs = new URLSearchParams({ view: 'cm', fs: '1' });
      if (params.to) qs.set('to', String(params.to).trim());
      if (params.subject) qs.set('su', String(params.subject));
      if (params.body) qs.set('body', String(params.body));
      const url = `https://mail.google.com/mail/?${qs.toString()}`;
      return [
        { action: 'open', label: 'Open Gmail compose', url, wait_for_load: true, timeout_ms: 15000 },
        { action: 'sleep', label: 'Wait for compose dialog', ms: 1200 },
        { action: 'eval', label: 'Confirm compose state', save_as: saveAs || 'compose', script: 'return { url: location.href, title: document.title };' },
      ];
    },
  },
  {
    id: 'gmail.detect_reschedules',
    serviceId: 'mail',
    name: 'Gmail: Detect Calendar Updates',
    category: 'data',
    defaultSaveAs: 'calendar_notices',
    description: 'Scan visible Gmail rows for EN/ES/PT Calendar invitations, updates, or cancellations.',
    fields: [
      { key: 'limit', label: 'Rows to inspect', type: 'number', default: 25, min: 5, max: 50 },
    ],
    compile: (params = {}, saveAs = 'calendar_notices') => {
      const limit = Math.max(5, Math.min(50, Number(params.limit) || 25));
      return [
        {
          action: 'wait_for',
          label: 'Wait for Gmail list',
          locator: { kind: 'css', value: 'tr.zA, [role="row"]', exact: false },
          timeout_ms: 15000,
        },
        {
          action: 'eval',
          label: 'Scan for invite updates',
          save_as: saveAs || 'calendar_notices',
          script: `return (() => {
  const hits = [];
  const els = document.querySelectorAll('tr.zA, [role="row"]');
  const re = /(updated invitation|rescheduled|new time proposed|canceled event|invitación actualizada|nuevo horario|cancelado|convite atualizado|novo horário)/i;
  for (let i = 0; i < els.length && i < ${limit}; i++) {
    const r = els[i];
    const txt = (r.innerText || '').trim();
    if (!re.test(txt)) continue;
    const subjEl = r.querySelector('.bog, .bqe, [data-thread-id]');
    const fromEl = r.querySelector('.yW span[email], span[email]');
    const dateEl = r.querySelector('td.xW span[title], td.xW span');
    hits.push({
      subject: (subjEl ? subjEl.textContent : txt.split('\\n')[0] || '').trim(),
      from: fromEl ? (fromEl.getAttribute('email') || fromEl.textContent || '').trim() : '',
      dateText: dateEl ? (dateEl.getAttribute('title') || dateEl.textContent || '').trim() : '',
      preview: txt.slice(0, 220),
    });
  }
  return { found: hits.length, hits };
})();`,
        },
      ];
    },
  },
  {
    id: 'calendar.read_agenda',
    serviceId: 'calendar',
    name: 'Calendar: Read Agenda',
    category: 'data',
    defaultSaveAs: 'agenda',
    description: 'Read visible upcoming event chips from your open Google Calendar tab.',
    fields: [
      { key: 'limit', label: 'Max events', type: 'number', default: 25, min: 1, max: 60 },
    ],
    compile: (params = {}, saveAs = 'agenda') => {
      const limit = Math.max(1, Math.min(60, Number(params.limit) || 25));
      return [
        { action: 'sleep', label: 'Wait for Calendar grid', ms: 1200 },
        {
          action: 'eval',
          label: 'Read event chips',
          save_as: saveAs || 'agenda',
          script: `return (() => {
  const chips = document.querySelectorAll('[data-eventchip], [data-eventid], [role="button"][jslog]');
  const events = [];
  const seen = new Set();
  for (let i = 0; i < chips.length && events.length < ${limit}; i++) {
    const label = (chips[i].getAttribute('aria-label') || chips[i].innerText || '').trim();
    if (!label || label.length < 3 || seen.has(label)) continue;
    seen.add(label);
    events.push(label.replace(/\\s+/g, ' '));
  }
  return { title: document.title, count: events.length, events };
})();`,
        },
      ];
    },
  },
  {
    id: 'calendar.create_event',
    serviceId: 'calendar',
    name: 'Calendar: Pre-fill Event',
    category: 'act',
    defaultSaveAs: 'calendar_event',
    description: 'Open Google Calendar event template with title, date/time, attendees, and notes.',
    fields: [
      { key: 'title', label: 'Event title', type: 'text', default: 'Follow up', placeholder: '{{calendar_notices.hits.0.subject}}' },
      { key: 'date', label: 'Date (YYYY-MM-DD)', type: 'text', default: todayISO() },
      { key: 'startTime', label: 'Start time (HH:MM)', type: 'text', default: '10:00' },
      { key: 'durationMin', label: 'Duration (min)', type: 'number', default: 30, min: 15, max: 480 },
      { key: 'attendees', label: 'Attendees (comma-separated)', type: 'text', default: '' },
      { key: 'details', label: 'Details / Description', type: 'textarea', default: '' },
    ],
    compile: (params = {}, saveAs = 'calendar_event') => {
      const date = /^\d{4}-\d{2}-\d{2}$/.test(String(params.date || '')) ? params.date : todayISO();
      const time = /^\d{2}:\d{2}$/.test(String(params.startTime || '')) ? params.startTime : '';
      let dates;
      if (time) {
        const end = addMinutes(date, time, params.durationMin || 30);
        dates = `${toGCalDateTime(date, time)}/${toGCalDateTime(end.date, end.time)}`;
      } else {
        const end = addMinutes(date, '00:00', 1440);
        dates = `${toGCalDateTime(date)}/${toGCalDateTime(end.date)}`;
      }
      const qs = new URLSearchParams({ action: 'TEMPLATE', text: String(params.title || 'Follow up'), dates });
      if (params.details) qs.set('details', String(params.details));
      if (params.attendees) qs.set('add', String(params.attendees).trim());
      const url = `https://calendar.google.com/calendar/render?${qs.toString()}`;
      return [
        { action: 'open', label: 'Open event draft', url, wait_for_load: true, timeout_ms: 15000 },
        { action: 'sleep', label: 'Wait for Calendar UI', ms: 1200 },
        { action: 'eval', label: 'Capture event URL', save_as: saveAs || 'calendar_event', script: 'return { url: location.href, title: document.title };' },
      ];
    },
  },
  {
    id: 'docs.create_doc',
    serviceId: 'docs',
    name: 'Docs: Create Document',
    category: 'act',
    defaultSaveAs: 'doc',
    description: 'Create a blank Google Doc in your authenticated Drive account and capture its link.',
    fields: [],
    compile: (_params = {}, saveAs = 'doc') => [
      { action: 'open', label: 'Open docs.new', url: 'https://docs.google.com/document/create', wait_for_load: true, timeout_ms: 20000 },
      { action: 'sleep', label: 'Wait for Doc ID', ms: 2000 },
      { action: 'eval', label: 'Capture Doc URL', save_as: saveAs || 'doc', script: 'return { url: location.href, title: document.title };' },
    ],
  },
  {
    id: 'docs.search_drive',
    serviceId: 'docs',
    name: 'Drive: Search Files',
    category: 'nav',
    defaultSaveAs: 'drive_files',
    description: 'Search files in Google Drive and list matching titles from the page.',
    fields: [
      { key: 'query', label: 'Search query', type: 'text', default: 'proposal', placeholder: 'Contract, Q4...' },
    ],
    compile: (params = {}, saveAs = 'drive_files') => {
      const q = String(params.query || '').trim();
      const url = q ? `https://drive.google.com/drive/search?q=${encodeURIComponent(q)}` : 'https://drive.google.com/drive/my-drive';
      return [
        { action: 'open', label: 'Open Drive search', url, wait_for_load: true, timeout_ms: 15000 },
        { action: 'sleep', label: 'Wait for Drive list', ms: 1800 },
        {
          action: 'eval',
          label: 'List visible files',
          save_as: saveAs || 'drive_files',
          script: `return (() => {
  const els = document.querySelectorAll('[data-id] [role="gridcell"], [data-tooltip], [aria-label]');
  const items = [];
  const seen = new Set();
  for (let i = 0; i < els.length && items.length < 25; i++) {
    const t = (els[i].getAttribute('data-tooltip') || els[i].innerText || '').trim();
    if (!t || t.length < 3 || t.length > 120 || seen.has(t)) continue;
    seen.add(t);
    items.push(t);
  }
  return { query: ${JSON.stringify(q)}, count: items.length, items };
})();`,
        },
      ];
    },
  },
  {
    id: 'github.read_notifications',
    serviceId: 'github',
    name: 'GitHub: Read Notifications',
    category: 'data',
    defaultSaveAs: 'github_notifs',
    description: 'Fetch unread GitHub notifications from your authenticated session without a token.',
    fields: [
      { key: 'limit', label: 'Max notifications', type: 'number', default: 25, min: 1, max: 50 },
    ],
    compile: (params = {}, saveAs = 'github_notifs') => {
      const limit = Math.max(1, Math.min(50, Number(params.limit) || 25));
      return [
        { action: 'open', label: 'Open GitHub notifications', url: 'https://github.com/notifications', wait_for_load: true, timeout_ms: 15000 },
        {
          action: 'eval',
          label: 'Extract notifications',
          save_as: saveAs || 'github_notifs',
          script: `return (() => {
  const items = [];
  const rows = document.querySelectorAll('.notifications-list-item, [data-notification-id]');
  for (let i = 0; i < rows.length && items.length < ${limit}; i++) {
    const el = rows[i];
    const titleEl = el.querySelector('.markdown-title, a[href*="/issues/"], a[href*="/pull/"]');
    const repoEl = el.querySelector('.text-small .f6, a.Link--muted');
    items.push({
      title: (titleEl ? titleEl.textContent : (el.innerText || '').split('\\n')[0] || '').trim(),
      repo: (repoEl ? repoEl.textContent : '').trim(),
      href: titleEl ? titleEl.href : '',
    });
  }
  return { count: items.length, items };
})();`,
        },
      ];
    },
  },
];

export function getAllSiteActions(customActions = []) {
  const map = new Map();
  for (const a of BUILTIN_SITE_ACTIONS) map.set(a.id, { ...a, builtin: true });
  for (const c of customActions || []) {
    if (c?.id) map.set(c.id, { ...c, builtin: false });
  }
  return [...map.values()];
}

export function findSiteAction(actionId, customActions = []) {
  return getAllSiteActions(customActions).find((a) => a.id === actionId) || null;
}

export function createCustomSiteActionTemplate(serviceId = 'mail') {
  return {
    id: `custom.${serviceId}.extract_${Date.now().toString(36).slice(-4)}`,
    serviceId,
    name: `${serviceId}: Custom Action`,
    category: 'data',
    defaultSaveAs: 'custom_result',
    description: 'Reusable site step using custom CSS selectors or JS.',
    fields: [
      { key: 'query', label: 'Parameter ({{query}})', type: 'text', default: '' },
    ],
    steps: [
      {
        action: 'eval',
        label: 'Run custom script',
        script: 'return { url: location.href, title: document.title, query: "{{query}}" };',
      },
    ],
  };
}

export function validateCustomSiteAction(def) {
  const errs = [];
  if (!def?.id || !/^[a-z0-9][a-z0-9._-]{1,63}$/i.test(def.id)) errs.push('Valid action ID required (e.g. erp.export_invoices)');
  if (!def?.name?.trim()) errs.push('Action name required');
  if (!def?.serviceId) errs.push('Target service required');
  if (!Array.isArray(def?.steps) || !def.steps.length) errs.push('At least one step required');
  return errs;
}

function interpolateParamString(str, params) {
  return String(str || '').replace(/\{\{\s*([a-zA-Z0-9_-]+)\s*\}\}/g, (_, k) => (k in params && params[k] !== undefined ? String(params[k]) : ''));
}

export function compileSiteAction(actionId, params = {}, saveAs = '', customActions = []) {
  const def = findSiteAction(actionId, customActions);
  if (!def) return null;
  const resolved = {};
  for (const f of def.fields || []) {
    resolved[f.key] = params[f.key] !== undefined ? params[f.key] : f.default;
  }
  for (const [k, v] of Object.entries(params || {})) {
    if (!(k in resolved)) resolved[k] = v;
  }
  const outVar = String(saveAs || def.defaultSaveAs || 'result').trim();
  if (typeof def.compile === 'function') {
    return { serviceId: def.serviceId, steps: def.compile(resolved, outVar) };
  }
  const steps = (def.steps || []).map((raw, i, arr) => {
    const s = JSON.parse(JSON.stringify(raw));
    for (const field of ['url', 'text', 'contains', 'equals', 'script']) {
      if (typeof s[field] === 'string') s[field] = interpolateParamString(s[field], resolved);
    }
    if (s.locator) {
      if (typeof s.locator.value === 'string') s.locator.value = interpolateParamString(s.locator.value, resolved);
      if (typeof s.locator.name === 'string') s.locator.name = interpolateParamString(s.locator.name, resolved);
    }
    if (i === arr.length - 1 && outVar && ['eval', 'extract', 'screenshot'].includes(s.action)) {
      s.save_as = outVar;
    }
    return s;
  });
  return { serviceId: def.serviceId, steps };
}
