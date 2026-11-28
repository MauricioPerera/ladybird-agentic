/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { formatClock, todayISO } from './util.js';

export const SEEN_KEY = 'crm:notified:v1';
export const CHECK_MS = 60_000;
export const MAX_PER_CHECK = 3;

function readSeen(today, store) {
  try {
    const raw = store?.getNotifiedMap ? store.getNotifiedMap() : JSON.parse(localStorage.getItem(SEEN_KEY) || '{}');
    const clean = {};
    for (const [k, v] of Object.entries(raw || {})) {
      if (v === today) clean[k] = v;
    }
    return clean;
  } catch {
    return {};
  }
}

function writeSeen(seen, store) {
  try {
    if (store?.saveNotifiedMap) {
      store.saveNotifiedMap(seen);
    }
    localStorage.setItem(SEEN_KEY, JSON.stringify(seen));
  } catch {}
}

export function notificationSupported() {
  return typeof window !== 'undefined' && 'Notification' in window;
}

export function notificationPermission() {
  if (!notificationSupported()) return 'unsupported';
  return Notification.permission;
}

export async function requestNotificationPermission() {
  if (!notificationSupported()) return 'unsupported';
  try {
    return await Notification.requestPermission();
  } catch {
    return 'denied';
  }
}

export function checkReminders(store, now = new Date(), NotifyCtor = globalThis.Notification) {
  if (!store?.settings?.notifications) return 0;
  if (!NotifyCtor || NotifyCtor.permission !== 'granted') return 0;

  const today = todayISO(now);
  const hhmm = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;
  const seen = readSeen(today, store);
  const open = store.openTasks();
  let sent = 0;

  const emit = (key, title, body, route) => {
    if (seen[key] === today || sent >= MAX_PER_CHECK) return false;
    seen[key] = today;
    sent += 1;
    try {
      const n = new NotifyCtor(title, { body, tag: key });
      n.onclick = () => {
        window.focus?.();
        window.location.hash = route;
      };
    } catch {}
    return true;
  };

  let overdue = 0;
  let dueToday = 0;
  for (const t of open) {
    if (t.due < today) overdue++;
    else if (t.due === today) dueToday++;
  }
  if (overdue > 0 || dueToday > 0) {
    const parts = [];
    if (overdue > 0) parts.push(`${overdue} overdue`);
    if (dueToday > 0) parts.push(`${dueToday} due today`);
    emit(`digest:${today}`, 'Follow-ups need you', parts.join(' · '), '#/followups');
  }

  for (const t of open) {
    if (sent >= MAX_PER_CHECK) break;
    if (t.due === today && t.dueTime && t.dueTime <= hhmm) {
      emit(`task:${t.id}:${today}`, t.title || 'Follow-up due', `Due at ${formatClock(t.dueTime)}`, `#/followups?task=${t.id}`);
    }
  }

  if (sent > 0) writeSeen(seen, store);
  return sent;
}

export function startReminders(store) {
  const run = () => checkReminders(store);
  run();
  const timer = setInterval(run, CHECK_MS);
  const onVis = () => {
    if (!document.hidden) run();
  };
  document.addEventListener('visibilitychange', onVis);
  store.addEventListener('change', run);
  return () => {
    clearInterval(timer);
    document.removeEventListener('visibilitychange', onVis);
    store.removeEventListener('change', run);
  };
}
