/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { addDays, isValidISODate, todayISO } from './util.js';

const MONTH_ALIASES = {
  jan: 1, january: 1, ene: 1, enero: 1, janv: 1, janeiro: 1,
  feb: 2, february: 2, fev: 2, febrero: 2, fevereiro: 2,
  mar: 3, march: 3, marzo: 3, marco: 3, 'março': 3,
  apr: 4, april: 4, abr: 4, abril: 4,
  may: 5, may_: 5, mayo: 5, mai: 5, maio: 5,
  jun: 6, june: 6, junio: 6, junho: 6,
  jul: 7, july: 7, julio: 7, julho: 7,
  aug: 8, august: 8, ago: 8, agosto: 8,
  sep: 9, sept: 9, september: 9, set: 9, septiembre: 9, setembro: 9,
  oct: 10, october: 10, out: 10, octubre: 10, outubro: 10,
  nov: 11, november: 11, noviembre: 11, novembro: 11,
  dec: 12, december: 12, dic: 12, dez: 12, diciembre: 12, dezembro: 12,
};

const pad2 = (n) => String(n).padStart(2, '0');

function toGCalDateTime(isoDate, timeStr) {
  const clean = String(isoDate || '').replace(/-/g, '');
  if (!timeStr || !/^\d{2}:\d{2}$/.test(timeStr)) return clean;
  return `${clean}T${timeStr.replace(':', '')}00`;
}

function addMinutesToDateTime(isoDate, timeStr, minutes = 30) {
  const [y, m, d] = isoDate.split('-').map(Number);
  const [hh, mm] = timeStr.split(':').map(Number);
  const dt = new Date(y, m - 1, d, hh, mm + minutes, 0);
  const nextIso = `${dt.getFullYear()}-${pad2(dt.getMonth() + 1)}-${pad2(dt.getDate())}`;
  const nextTime = `${pad2(dt.getHours())}:${pad2(dt.getMinutes())}`;
  return { due: nextIso, dueTime: nextTime };
}

export function googleCalendarEventUrl({
  title = '',
  due = todayISO(),
  dueTime = '',
  durationMin = 30,
  details = '',
  attendees = [],
  location = '',
} = {}) {
  const safeDue = isValidISODate(due) ? due : todayISO();
  let dates;
  if (dueTime && /^\d{2}:\d{2}$/.test(dueTime)) {
    const end = addMinutesToDateTime(safeDue, dueTime, durationMin);
    dates = `${toGCalDateTime(safeDue, dueTime)}/${toGCalDateTime(end.due, end.dueTime)}`;
  } else {
    dates = `${toGCalDateTime(safeDue)}/${toGCalDateTime(addDays(safeDue, 1))}`;
  }
  const params = new URLSearchParams({
    action: 'TEMPLATE',
    text: String(title || 'Follow up'),
    dates,
  });
  if (details) params.set('details', String(details));
  if (location) params.set('location', String(location));
  const cleanAdd = (attendees || []).map((e) => String(e || '').trim()).filter((e) => e.includes('@'));
  if (cleanAdd.length) params.set('add', cleanAdd.join(','));
  return `https://calendar.google.com/calendar/render?${params.toString()}`;
}

export function gmailComposeUrl({ to = '', subject = '', body = '' } = {}) {
  const params = new URLSearchParams({ view: 'cm', fs: '1' });
  if (to) params.set('to', String(to).trim());
  if (subject) params.set('su', String(subject));
  if (body) params.set('body', String(body));
  return `https://mail.google.com/mail/?${params.toString()}`;
}

export function gmailSearchUrl(query = '') {
  const q = String(query || '').trim();
  return q ? `https://mail.google.com/mail/u/0/#search/${encodeURIComponent(q)}` : 'https://mail.google.com/mail/u/0/#inbox';
}

export function googleDocsCreateUrl() {
  return 'https://docs.google.com/document/create';
}

export function googleDriveSearchUrl(query = '') {
  const q = String(query || '').trim();
  return q ? `https://drive.google.com/drive/search?q=${encodeURIComponent(q)}` : 'https://drive.google.com/drive/my-drive';
}

function parseICSTimestamp(rawVal) {
  const val = String(rawVal || '').trim();
  const mDate = val.match(/^(\d{4})(\d{2})(\d{2})(?:T(\d{2})(\d{2})(?:\d{2})?(Z)?)?$/);
  if (!mDate) return null;
  if (mDate[6] === 'Z' && mDate[4]) {
    const d = new Date(Date.UTC(Number(mDate[1]), Number(mDate[2]) - 1, Number(mDate[3]), Number(mDate[4]), Number(mDate[5])));
    const iso = `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;
    if (!isValidISODate(iso)) return null;
    return { due: iso, dueTime: `${pad2(d.getHours())}:${pad2(d.getMinutes())}` };
  }
  const iso = `${mDate[1]}-${mDate[2]}-${mDate[3]}`;
  if (!isValidISODate(iso)) return null;
  const dueTime = mDate[4] ? `${mDate[4]}:${mDate[5]}` : '';
  return { due: iso, dueTime };
}

export function extractCRMTags(text) {
  const s = String(text || '');
  const taskMatch = s.match(/\[CRM:task:([a-zA-Z0-9_-]+)\]/i);
  const dealMatch = s.match(/\[CRM:deal:([a-zA-Z0-9_-]+)\]/i);
  const contactMatch = s.match(/\[CRM:contact:([a-zA-Z0-9_-]+)\]/i);
  return {
    crmTaskId: taskMatch ? taskMatch[1] : null,
    crmDealId: dealMatch ? dealMatch[1] : null,
    crmContactId: contactMatch ? contactMatch[1] : null,
  };
}

export function parseICSUpdate(icsText) {
  const unfolded = String(icsText || '').replace(/\r?\n[ \t]/g, '');
  const lines = unfolded.split(/\r?\n/);
  const out = {
    uid: '',
    sequence: 0,
    summary: '',
    status: '',
    partstat: '',
    due: null,
    dueTime: '',
    description: '',
    attendees: [],
  };
  let inEvent = false;
  for (const raw of lines) {
    const line = raw.trim();
    if (line === 'BEGIN:VEVENT') {
      inEvent = true;
      continue;
    }
    if (line === 'END:VEVENT') break;
    if (!inEvent && !unfolded.includes('BEGIN:VEVENT') && !/^(DTSTART|SUMMARY|UID|SEQUENCE|STATUS):/i.test(line)) {
      continue;
    }
    const idx = line.indexOf(':');
    if (idx < 0) continue;
    const keyPart = line.slice(0, idx).toUpperCase();
    const val = line.slice(idx + 1).replace(/\\n/g, '\n').replace(/\\([,;\\])/g, '$1').trim();
    const key = keyPart.split(';')[0];
    if (key === 'UID') out.uid = val;
    else if (key === 'SEQUENCE') out.sequence = Number(val) || 0;
    else if (key === 'SUMMARY') out.summary = val;
    else if (key === 'STATUS') out.status = val.toUpperCase();
    else if (key === 'DESCRIPTION') out.description = val;
    else if (key === 'DTSTART') {
      const parsed = parseICSTimestamp(val);
      if (parsed) {
        out.due = parsed.due;
        out.dueTime = parsed.dueTime;
      }
    } else if (key === 'ATTENDEE') {
      const mail = val.replace(/^mailto:/i, '').trim();
      if (mail.includes('@')) out.attendees.push(mail);
      const pm = keyPart.match(/PARTSTAT=([A-Z-]+)/);
      if (pm && !out.partstat) out.partstat = pm[1];
    }
  }
  const tags = extractCRMTags(`${out.summary}\n${out.description}`);
  return { ...out, ...tags };
}

function parseTimeToken(raw) {
  const s = String(raw || '').trim();
  const m = s.match(/\b([01]?\d|2[0-3]):([0-5]\d)\s*(am|pm)?\b/i);
  if (m) {
    let hh = Number(m[1]);
    const mm = Number(m[2]);
    const ampm = m[3]?.toLowerCase();
    if (ampm === 'pm' && hh < 12) hh += 12;
    else if (ampm === 'am' && hh === 12) hh = 0;
    return `${pad2(hh)}:${pad2(mm)}`;
  }
  const mSimple = s.match(/\b(1[0-2]|[1-9])\s*(am|pm)\b/i);
  if (mSimple) {
    let hh = Number(mSimple[1]);
    const ampm = mSimple[2].toLowerCase();
    if (ampm === 'pm' && hh < 12) hh += 12;
    else if (ampm === 'am' && hh === 12) hh = 0;
    return `${pad2(hh)}:00`;
  }
  return '';
}

function parseLooseNoticeDate(text) {
  const s = String(text || '');
  const isoM = s.match(/\b(20\d{2}-\d{2}-\d{2})\b/);
  if (isoM && isValidISODate(isoM[1])) {
    return { due: isoM[1], dueTime: parseTimeToken(s) };
  }

  // English format: "Dec 14, 2026" or "Mon Dec 14, 2026"
  const enM = s.match(/\b([A-Za-záéíóúç]+)\s+(\d{1,2}),?\s+(20\d{2})\b/i);
  if (enM) {
    const mon = MONTH_ALIASES[enM[1].toLowerCase()];
    if (mon) {
      const iso = `${enM[3]}-${pad2(mon)}-${pad2(enM[2])}`;
      if (isValidISODate(iso)) return { due: iso, dueTime: parseTimeToken(s) };
    }
  }

  // ES / PT format: "14 dic 2026" or "14 de dez de 2026"
  const latinM = s.match(/\b(\d{1,2})\s+(?:de\s+)?([A-Za-záéíóúç]+)\.?(?:\s+de)?\s+(20\d{2})\b/i);
  if (latinM) {
    const mon = MONTH_ALIASES[latinM[2].toLowerCase()];
    if (mon) {
      const iso = `${latinM[3]}-${pad2(mon)}-${pad2(latinM[1])}`;
      if (isValidISODate(iso)) return { due: iso, dueTime: parseTimeToken(s) };
    }
  }

  // Numeric DD/MM/YYYY
  const slashM = s.match(/\b(\d{1,2})\/(\d{1,2})\/(20\d{2})\b/);
  if (slashM) {
    const iso = `${slashM[3]}-${pad2(slashM[2])}-${pad2(slashM[1])}`;
    if (isValidISODate(iso)) return { due: iso, dueTime: parseTimeToken(s) };
  }

  return null;
}

function cleanCalendarEventTitle(raw) {
  let out = String(raw || '').trim();
  out = out.replace(
    /^(?:Updated invitation(?: with note)?|New time proposed|Invitation|Accepted|Declined|Tentative|Canceled|Cancelled|Invitación actualizada|Propuesta de nuevo horario|Invitación|Aceptado|Rechazado|Cancelado|Convite atualizado|Novo horário proposto|Convite|Aceito|Recusado)\s*:\s*/i,
    ''
  );
  out = out.replace(/\s*@\s*(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun|Lun|Mar|Mié|Jue|Vie|Sáb|Dom|Seg|Ter|Qua|Qui|Sex)\b.*$/i, '');
  out = out.replace(/\s*\(calendar-notification.*$/i, '');
  return out.trim();
}

export function parseCalendarEmailNotice({
  subject = '',
  title = '',
  body = '',
  from = '',
  timestampMs = Date.now(),
  serviceId = 'mail',
} = {}) {
  const combinedHead = String(subject || title || '').trim();
  const fullText = `${combinedHead}\n${from}\n${body}`;
  const ics = /BEGIN:VEVENT|DTSTART[:;]/i.test(body) ? parseICSUpdate(body) : null;
  const tags = extractCRMTags(fullText);
  const emails = [...new Set((fullText.match(/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/g) || []).map((e) => e.toLowerCase()))];

  const isUpdate = /\b(updated invitation|rescheduled|new time proposed|invitación actualizada|nuevo horario|evento actualizado|convite atualizado|novo horário|evento atualizado)\b/i.test(fullText) || (ics && ics.sequence > 0);
  const isCancel = /\b(canceled|cancelled|cancelado|cancelada)\b/i.test(fullText) || ics?.status === 'CANCELLED';
  const isRsvp = /\b(accepted|declined|tentative|aceptado|rechazado|aceito|recusado)\b/i.test(combinedHead) || Boolean(ics?.partstat);
  const isCalendar =
    serviceId === 'calendar' ||
    isUpdate ||
    isCancel ||
    isRsvp ||
    /calendar-notification@google\.com|\b(invitation|invitación|convite):/i.test(fullText) ||
    Boolean(ics);

  let kind = 'mail';
  if (isCancel) kind = 'calendar-cancel';
  else if (isUpdate) kind = 'calendar-reschedule';
  else if (isRsvp) kind = 'calendar-rsvp';
  else if (isCalendar) kind = 'calendar-event';

  const parsedDate = ics?.due
    ? { due: ics.due, dueTime: ics.dueTime || '' }
    : parseLooseNoticeDate(`${combinedHead}\n${body}`);

  const eventTitle = cleanCalendarEventTitle(ics?.summary || combinedHead);

  return {
    id: `${serviceId}:${timestampMs}:${combinedHead.slice(0, 40)}`,
    serviceId,
    kind,
    timestampMs,
    subject: combinedHead,
    from: String(from || '').trim(),
    body: String(body || '').trim(),
    eventTitle,
    due: parsedDate?.due || null,
    dueTime: parsedDate?.dueTime || '',
    partstat: ics?.partstat || '',
    emails: emails.filter((e) => !e.includes('calendar-notification@')),
    crmTaskId: ics?.crmTaskId || tags.crmTaskId,
    crmDealId: ics?.crmDealId || tags.crmDealId,
    crmContactId: tags.crmContactId,
  };
}

export function matchNoticeToCRM(notice, store) {
  let task = notice.crmTaskId ? store.task(notice.crmTaskId) : null;
  if (!task && notice.eventTitle) {
    const q = notice.eventTitle.toLowerCase();
    task = store.openTasks().find((tsk) => {
      const tt = String(tsk.title || '').trim().toLowerCase();
      return tt && (tt === q || tt.includes(q) || q.includes(tt));
    }) || null;
  }

  let deal = notice.crmDealId ? store.deal(notice.crmDealId) : task?.dealId ? store.deal(task.dealId) : null;
  let contact = notice.crmContactId ? store.contact(notice.crmContactId) : null;
  if (!contact && notice.emails?.length) {
    contact = store.contacts.find((c) => c.email && notice.emails.includes(c.email.toLowerCase())) || null;
  }
  if (!contact && task?.contactId) contact = store.contact(task.contactId);
  if (!contact && deal?.contactId) contact = store.contact(deal.contactId);
  if (!deal && contact) {
    deal = store.dealsFor(contact.id).find((d) => d.stage !== 'won' && d.stage !== 'lost') || null;
  }

  return { task, deal, contact };
}

const INBOX_SCAN_SCRIPT = `return (() => {
  const rows = [];
  const seen = new Set();
  const rowEls = document.querySelectorAll('tr.zA, [role="row"]');
  for (let i = 0; i < rowEls.length && rows.length < 25; i++) {
    const row = rowEls[i];
    const txt = (row.innerText || '').trim();
    if (!txt || txt.length < 6) continue;
    const subjEl = row.querySelector('.bog, .bqe, [data-thread-id], [role="link"]');
    const senderEl = row.querySelector('.yW span[email], span[email], .zF, .yP');
    const bodyEl = row.querySelector('.y2');
    const dateEl = row.querySelector('td.xW span[title], td.xW span');
    const subject = (subjEl ? subjEl.textContent : txt.split('\\n')[0] || '').trim();
    const senderEmail = senderEl ? (senderEl.getAttribute('email') || '').trim() : '';
    const senderName = senderEl ? (senderEl.textContent || '').trim() : '';
    const from = senderEmail && senderName && senderEmail !== senderName ? senderName + ' <' + senderEmail + '>' : (senderEmail || senderName);
    const snippetRaw = (bodyEl ? bodyEl.textContent : txt).trim();
    const snippet = snippetRaw.replace(/^\\s*[-\\u2013\\u2014]+\\s*/, '').trim();
    const dateTitle = dateEl ? (dateEl.getAttribute('title') || dateEl.textContent || '').trim() : '';
    if (!subject && !from) continue;
    const key = subject + '|' + from;
    if (seen.has(key)) continue;
    seen.add(key);
    rows.push({ subject, from, body: snippet, dateTitle });
  }
  const openBodies = document.querySelectorAll('.a3s, [role="main"] [data-message-id]');
  let activeSubject = '';
  const h2 = document.querySelector('h2.hP, [data-legacy-thread-id] h2');
  if (h2) activeSubject = (h2.textContent || '').trim();
  const activeBody = openBodies.length ? (openBodies[openBodies.length - 1].innerText || '').slice(0, 4000) : '';
  return {
    url: location.href,
    title: document.title,
    rows,
    activeSubject,
    activeBody
  };
})()`;

export function createServiceHub(store) {
  const eventsTarget = new EventTarget();
  let states = [];
  let events = [];
  const seenKeys = new Set();
  let unsubscribe = null;
  let scanning = false;
  let lastScan = null;

  const emitChange = () => eventsTarget.dispatchEvent(new Event('change'));
  const bridgeTabs = () => (typeof window !== 'undefined' ? window.ladybirdApp?.tabs || null : null);
  const isAvailable = () => Boolean(bridgeTabs()) && (window.ladybirdApp?.info?.services?.length || 0) > 0;

  const pushNotice = (notice) => {
    const dedup = `${notice.kind}|${notice.eventTitle || notice.subject}|${notice.due || ''}|${notice.dueTime || ''}`;
    if (seenKeys.has(dedup)) return null;
    seenKeys.add(dedup);
    const match = matchNoticeToCRM(notice, store);
    const entry = {
      ...notice,
      taskId: match.task?.id || null,
      taskTitle: match.task?.title || null,
      dealId: match.deal?.id || null,
      dealName: match.deal?.name || null,
      contactId: match.contact?.id || null,
      contactName: match.contact?.name || null,
      handled: false,
    };
    events = [entry, ...events].slice(0, 40);
    emitChange();
    return entry;
  };

  const refreshStates = async () => {
    const tabs = bridgeTabs();
    if (!tabs) return [];
    try {
      const res = await tabs.list();
      states = res?.services || [];
      emitChange();
    } catch {}
    return states;
  };

  const handleRawNotification = (n) => {
    const parsed = parseCalendarEmailNotice({
      title: n.title,
      body: n.body,
      timestampMs: n.timestampMs || Date.now(),
      serviceId: n.serviceId,
    });
    const stored = pushNotice(parsed);
    if (stored && parsed.serviceId === 'mail' && parsed.kind === 'calendar-reschedule') {
      scanMailInbox().catch(() => {});
    }
    emitChange();
  };

  const onFocusRefresh = () => { refreshStates(); };
  const onVisRefresh = () => {
    if (typeof document !== 'undefined' && document.visibilityState === 'visible') refreshStates();
  };

  const start = () => {
    const tabs = bridgeTabs();
    if (!tabs || unsubscribe) return;
    refreshStates();
    unsubscribe = tabs.onNotification((n) => handleRawNotification(n));
    if (typeof window !== 'undefined') window.addEventListener('focus', onFocusRefresh);
    if (typeof document !== 'undefined') document.addEventListener('visibilitychange', onVisRefresh);
  };

  const stop = () => {
    if (unsubscribe) {
      unsubscribe();
      unsubscribe = null;
    }
    if (typeof window !== 'undefined') window.removeEventListener('focus', onFocusRefresh);
    if (typeof document !== 'undefined') document.removeEventListener('visibilitychange', onVisRefresh);
  };

  const open = async (serviceId, url = null, opts = {}) => {
    const tabs = bridgeTabs();
    if (!tabs) throw new Error('Service Tabs API unavailable');
    const res = await tabs.open(serviceId, url ? { url, ...opts } : opts);
    await refreshStates();
    return res?.service || null;
  };

  const focus = async (serviceId) => {
    const tabs = bridgeTabs();
    if (!tabs) throw new Error('Service Tabs API unavailable');
    const res = await tabs.focus(serviceId);
    await refreshStates();
    return res?.service || null;
  };

  const scheduleFollowUpInCalendar = async (task) => {
    const contact = task?.contactId ? store.contact(task.contactId) : null;
    const deal = task?.dealId ? store.deal(task.dealId) : null;
    const marker = task?.id ? `[CRM:task:${task.id}]` : deal?.id ? `[CRM:deal:${deal.id}]` : '';
    const details = [deal ? `Deal: ${deal.name}` : '', contact ? `Lead: ${contact.name}` : '', marker].filter(Boolean).join('\n');
    const url = googleCalendarEventUrl({
      title: task?.title || deal?.name || 'Follow up',
      due: task?.due || todayISO(),
      dueTime: task?.dueTime || '',
      details,
      attendees: contact?.email ? [contact.email] : [],
    });
    return open('calendar', url);
  };

  const composeInGmail = async ({ to = '', subject = '', body = '' } = {}) => {
    return open('mail', gmailComposeUrl({ to, subject, body }));
  };

  const searchContactInGmail = async (query) => {
    return open('mail', gmailSearchUrl(query));
  };

  const openNewGoogleDoc = async () => open('docs', googleDocsCreateUrl());
  const openDriveSearch = async (query) => open('docs', googleDriveSearchUrl(query));

  const linkOpenDocToDeal = async (dealId) => {
    await refreshStates();
    const docsSvc = states.find((s) => s.id === 'docs');
    if (!docsSvc?.open || !docsSvc?.url) {
      return { ok: false, reason: 'not-open' };
    }
    const deal = store.deal(dealId);
    if (!deal) return { ok: false, reason: 'no-deal' };
    const title = docsSvc.title || docsSvc.url;
    await store.addActivity({
      type: 'note',
      date: todayISO(),
      summary: `Google Doc: ${title}`,
      notes: docsSvc.url,
      dealId: deal.id,
      contactId: deal.contactId || null,
      companyId: deal.companyId || null,
    });
    return { ok: true, title, url: docsSvc.url };
  };

  const scanMailInbox = async () => {
    const tabs = bridgeTabs();
    if (!tabs || scanning) return { scanned: false, added: 0, totalRows: 0 };
    scanning = true;
    emitChange();
    try {
      await refreshStates();
      const mailSvc = states.find((s) => s.id === 'mail');
      if (!mailSvc?.open) return { scanned: false, reason: 'not-open', added: 0, totalRows: 0 };
      let res;
      try {
        res = await tabs.eval('mail', INBOX_SCAN_SCRIPT);
      } catch (err) {
        return { scanned: false, reason: 'error', error: err?.message || 'Scan failed', added: 0, totalRows: 0 };
      }
      const data = res?.value || {};
      const rawRows = Array.isArray(data.rows) ? data.rows : [];
      let added = 0;
      if (data.activeBody) {
        const n = parseCalendarEmailNotice({
          subject: data.activeSubject || '',
          body: data.activeBody,
          timestampMs: Date.now(),
          serviceId: 'mail',
        });
        if ((n.kind !== 'mail' || n.emails.length > 0) && pushNotice(n)) added += 1;
      }
      for (const row of rawRows) {
        const combinedBody = [row.body, row.dateTitle].filter(Boolean).join('\n');
        const n = parseCalendarEmailNotice({
          subject: row.subject,
          from: row.from,
          body: combinedBody,
          timestampMs: Date.now(),
          serviceId: 'mail',
        });
        const match = matchNoticeToCRM(n, store);
        if (n.kind !== 'mail' || match.contact || match.task || match.deal) {
          if (pushNotice(n)) added += 1;
        }
      }
      lastScan = {
        at: Date.now(),
        url: data.url || '',
        title: data.title || '',
        totalRows: rawRows.length,
        added,
        activeSubject: data.activeSubject || '',
        rows: rawRows.slice(0, 15),
      };
      return { scanned: true, added, totalRows: rawRows.length };
    } finally {
      scanning = false;
      emitChange();
    }
  };

  const logRawInboxRow = async (row) => {
    if (!row) return null;
    const notice = parseCalendarEmailNotice({
      subject: row.subject,
      from: row.from,
      body: [row.body, row.dateTitle].filter(Boolean).join('\n'),
      timestampMs: Date.now(),
      serviceId: 'mail',
    });
    const match = matchNoticeToCRM(notice, store);
    const saved = await store.addActivity({
      type: 'email',
      date: todayISO(),
      summary: row.subject || '(Gmail)',
      notes: [row.from ? `From: ${row.from}` : '', row.dateTitle || '', row.body || ''].filter(Boolean).join('\n'),
      dealId: match.deal?.id || null,
      contactId: match.contact?.id || null,
    });
    if (lastScan?.rows) {
      lastScan = {
        ...lastScan,
        rows: lastScan.rows.map((r) => (r === row || (r.subject === row.subject && r.from === row.from) ? { ...r, logged: true } : r)),
      };
    }
    emitChange();
    return saved;
  };

  const applyDateUpdate = async (eventId) => {
    const ev = events.find((e) => e.id === eventId);
    if (!ev || !ev.due) return null;
    let updatedTask = null;
    if (ev.taskId && store.task(ev.taskId)) {
      updatedTask = await store.updateTask(ev.taskId, {
        due: ev.due,
        ...(ev.dueTime ? { dueTime: ev.dueTime } : {}),
      });
    } else {
      updatedTask = await store.addTask({
        title: ev.eventTitle || ev.subject || 'Meeting',
        due: ev.due,
        dueTime: ev.dueTime || '',
        dealId: ev.dealId || null,
        contactId: ev.contactId || null,
      });
    }
    await store.addActivity({
      type: 'meeting',
      date: todayISO(),
      summary: ev.eventTitle || ev.subject,
      notes: `${ev.due}${ev.dueTime ? ` ${ev.dueTime}` : ''}\n${ev.body}`.trim(),
      dealId: ev.dealId || updatedTask?.dealId || null,
      contactId: ev.contactId || updatedTask?.contactId || null,
    });
    events = events.map((e) => (e.id === eventId ? { ...e, handled: true } : e));
    emitChange();
    return updatedTask;
  };

  const logNoticeActivity = async (eventId) => {
    const ev = events.find((e) => e.id === eventId);
    if (!ev) return null;
    const saved = await store.addActivity({
      type: ev.kind.startsWith('calendar') ? 'meeting' : 'email',
      date: todayISO(),
      summary: ev.eventTitle || ev.subject,
      notes: [ev.from ? `From: ${ev.from}` : '', ev.body].filter(Boolean).join('\n'),
      dealId: ev.dealId || null,
      contactId: ev.contactId || null,
    });
    events = events.map((e) => (e.id === eventId ? { ...e, handled: true } : e));
    emitChange();
    return saved;
  };

  const dismissEvent = (eventId) => {
    events = events.filter((e) => e.id !== eventId);
    emitChange();
  };

  return {
    addEventListener: (type, fn) => eventsTarget.addEventListener(type, fn),
    removeEventListener: (type, fn) => eventsTarget.removeEventListener(type, fn),
    isAvailable,
    getStates: () => states,
    getEvents: () => events,
    getLastScan: () => lastScan,
    isScanning: () => scanning,
    start,
    stop,
    refreshStates,
    open,
    focus,
    scheduleFollowUpInCalendar,
    composeInGmail,
    searchContactInGmail,
    openNewGoogleDoc,
    openDriveSearch,
    linkOpenDocToDeal,
    scanMailInbox,
    logRawInboxRow,
    applyDateUpdate,
    logNoticeActivity,
    dismissEvent,
  };
}
