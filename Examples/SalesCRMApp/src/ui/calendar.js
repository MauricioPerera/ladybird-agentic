/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { isOpen } from '../lib/forecast.js';
import { t } from '../lib/i18n.js';
import { buildCalendar, dealEvent, taskEvent } from '../lib/ics.js';
import { toast } from './toast.js';

export function downloadTextFile(filename, content, mime = 'text/plain;charset=utf-8') {
  const blob = new Blob([content], { type: mime });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

export function exportTaskIcs(task, store) {
  const evt = taskEvent(task, {
    contactName: store.contact(task.contactId)?.name || '',
    dealName: store.deal(task.dealId)?.name || '',
  });
  downloadTextFile(`followup-${task.id.slice(0, 8)}.ics`, buildCalendar([evt]), 'text/calendar;charset=utf-8');
}

export function exportOpenCalendarIcs(store) {
  const events = [
    ...store.openTasks().map((t) =>
      taskEvent(t, {
        contactName: store.contact(t.contactId)?.name || '',
        dealName: store.deal(t.dealId)?.name || '',
      })
    ),
    ...store.deals
      .filter((d) => isOpen(d) && d.expectedClose)
      .map((d) => dealEvent(d, { companyName: store.company(d.companyId)?.name || '' })),
  ];
  downloadTextFile('sales-crm-followups.ics', buildCalendar(events), 'text/calendar;charset=utf-8');
  toast(t('Exported .ics calendar'));
}
