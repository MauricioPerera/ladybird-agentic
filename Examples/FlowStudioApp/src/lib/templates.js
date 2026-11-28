/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

export const SESSION_PACK_TEMPLATES = [
  {
    id: 'gmail-inbox-digest',
    name: 'Gmail Inbox Digest',
    summary: 'Extract the top 20 threads (subject, sender, date, snippet) from your authenticated Gmail tab.',
    serviceId: 'mail',
    trigger: {
      type: 'manual',
      enabled: true,
      intervalMs: 300000,
      cooldownMs: 5000,
      openIfClosed: true,
      activate: false,
      closeOnFinish: false,
    },
    steps: [
      {
        kind: 'site_action',
        siteActionId: 'gmail.read_inbox',
        serviceId: 'mail',
        label: 'Gmail: Read Inbox',
        params: { limit: 20, filterText: '' },
        save_as: 'inbox',
      },
    ],
  },
  {
    id: 'gmail-inbox-to-export',
    name: 'Gmail Digest -> Transform -> File',
    summary: 'Hybrid pipeline: reads Gmail inbox, summarizes senders locally with JS, and saves exports/gmail-digest.json.',
    serviceId: 'mail',
    trigger: {
      type: 'manual',
      enabled: true,
      intervalMs: 300000,
      cooldownMs: 5000,
      openIfClosed: true,
      activate: false,
      closeOnFinish: false,
    },
    steps: [
      {
        kind: 'site_action',
        siteActionId: 'gmail.read_inbox',
        serviceId: 'mail',
        label: 'Gmail: Read Inbox',
        params: { limit: 20, filterText: '' },
        save_as: 'inbox',
      },
      {
        kind: 'transform',
        serviceId: 'local',
        label: 'Summarize by sender',
        save_as: 'digest',
        code: `const rows = (input && input.rows) || [];
const bySender = {};
for (const r of rows) {
  const key = r.sender || 'unknown';
  bySender[key] = (bySender[key] || 0) + 1;
}
return { total: rows.length, scannedAt: input?.scannedAt || new Date().toISOString(), topSenders: bySender, sampleCount: Math.min(5, rows.length) };`,
      },
      {
        kind: 'save_file',
        serviceId: 'local',
        label: 'Save exports/gmail-digest.json',
        path: 'exports/gmail-digest.json',
        content: '{{digest}}',
        save_as: 'exported',
      },
    ],
  },
  {
    id: 'gmail-to-calendar-pipeline',
    name: 'Gmail Updates -> IF Found -> Google Calendar',
    summary: 'Cross-service flow: scans Gmail for invite/reschedule updates and pre-fills a Google Calendar event when found.',
    serviceId: 'mail',
    trigger: {
      type: 'manual',
      enabled: true,
      intervalMs: 300000,
      cooldownMs: 10000,
      openIfClosed: true,
      activate: true,
      closeOnFinish: false,
    },
    steps: [
      {
        kind: 'site_action',
        siteActionId: 'gmail.detect_reschedules',
        serviceId: 'mail',
        label: 'Detect Calendar Updates',
        params: { limit: 25 },
        save_as: 'calendar_notices',
      },
      {
        kind: 'condition',
        serviceId: 'local',
        label: 'IF notice found > 0',
        path: 'calendar_notices.found',
        operator: 'gt',
        expected: '0',
        save_as: 'has_notice',
      },
      {
        kind: 'site_action',
        siteActionId: 'calendar.create_event',
        serviceId: 'calendar',
        label: 'Pre-fill Google Calendar',
        params: {
          title: '{{calendar_notices.hits.0.subject}}',
          date: '',
          startTime: '10:00',
          durationMin: 30,
          attendees: '{{calendar_notices.hits.0.from}}',
          details: '{{calendar_notices.hits.0.preview}}',
        },
        save_as: 'calendar_event',
      },
    ],
  },
  {
    id: 'gmail-sync-watcher',
    name: 'Gmail Sync & Reschedule Watcher',
    summary: 'Triggers automatically when Gmail receives a background sync network response and detects Calendar updates.',
    serviceId: 'mail',
    trigger: {
      type: 'network',
      enabled: true,
      urlContains: '/sync/u/0/i/s',
      method: 'POST',
      statusCode: 200,
      cooldownMs: 10000,
      openIfClosed: false,
      activate: false,
      closeOnFinish: false,
    },
    steps: [
      {
        kind: 'step',
        action: 'sleep',
        label: 'Let Gmail render rows',
        ms: 600,
      },
      {
        kind: 'site_action',
        siteActionId: 'gmail.detect_reschedules',
        serviceId: 'mail',
        label: 'Detect Calendar notices',
        params: { limit: 25 },
        save_as: 'calendar_notices',
      },
    ],
  },
  {
    id: 'calendar-agenda-reader',
    name: 'Google Calendar Agenda Reader',
    summary: 'Reads visible upcoming meetings directly from your open Google Calendar tab.',
    serviceId: 'calendar',
    trigger: {
      type: 'manual',
      enabled: true,
      intervalMs: 300000,
      cooldownMs: 5000,
      openIfClosed: true,
      activate: false,
      closeOnFinish: false,
    },
    steps: [
      {
        kind: 'site_action',
        siteActionId: 'calendar.read_agenda',
        serviceId: 'calendar',
        label: 'Calendar: Read Agenda',
        params: { limit: 25 },
        save_as: 'agenda',
      },
    ],
  },
  {
    id: 'docs-quick-create',
    name: 'Create Blank Google Doc',
    summary: 'Opens Google Docs, creates a fresh document in your Personal account, and captures its URL.',
    serviceId: 'docs',
    trigger: {
      type: 'manual',
      enabled: true,
      intervalMs: 60000,
      cooldownMs: 5000,
      openIfClosed: true,
      activate: true,
      closeOnFinish: false,
    },
    steps: [
      {
        kind: 'site_action',
        siteActionId: 'docs.create_doc',
        serviceId: 'docs',
        label: 'Docs: Create Document',
        params: {},
        save_as: 'doc',
      },
    ],
  },
  {
    id: 'gmail-loop-triage',
    name: 'Gmail -> IF Rows -> For Each -> Export',
    summary: 'Uses IF/ELSE branching and a For Each loop to classify urgent Gmail threads locally and export exports/gmail-triage.json.',
    serviceId: 'mail',
    trigger: {
      type: 'manual',
      enabled: true,
      intervalMs: 300000,
      cooldownMs: 5000,
      openIfClosed: true,
      activate: false,
      closeOnFinish: false,
    },
    steps: [
      {
        kind: 'site_action',
        siteActionId: 'gmail.read_inbox',
        serviceId: 'mail',
        label: 'Gmail: Read Inbox',
        params: { limit: 10, filterText: '' },
        save_as: 'inbox',
      },
      {
        kind: 'condition',
        serviceId: 'local',
        label: 'IF inbox has emails',
        path: 'inbox.count',
        operator: 'gt',
        expected: '0',
        onFalse: 'branch',
        save_as: 'has_rows',
      },
      {
        kind: 'foreach',
        serviceId: 'local',
        label: 'Triage each email',
        runWhen: 'if_true',
        itemsPath: 'inbox.rows',
        maxItems: 10,
        save_as: 'triaged',
        bodyStep: {
          kind: 'transform',
          serviceId: 'local',
          label: 'Classify urgency',
          code: `const txt = ((input?.subject || '') + ' ' + (input?.snippet || '')).toLowerCase();
return { subject: input?.subject || '', sender: input?.sender || '', urgent: /(urgent|asap|invoice|factura|deadline)/i.test(txt) };`,
          save_as: 'item_triage',
        },
      },
      {
        kind: 'save_file',
        serviceId: 'local',
        label: 'Save triage JSON',
        runWhen: 'if_true',
        path: 'exports/gmail-triage.json',
        content: '{{triaged}}',
        save_as: 'saved',
      },
      {
        kind: 'transform',
        serviceId: 'local',
        label: 'Handle empty inbox',
        runWhen: 'if_false',
        code: 'return { message: "Inbox empty", checkedAt: new Date().toISOString() };',
        save_as: 'empty_notice',
      },
    ],
  },
];

export function cloneTemplate(templateId) {
  const tpl = SESSION_PACK_TEMPLATES.find((t) => t.id === templateId);
  if (!tpl) return null;
  return JSON.parse(JSON.stringify({
    ...tpl,
    description: tpl.summary,
    default_timeout_ms: 15000,
    variables: {},
    nodePositions: {},
    pinnedOutputs: {},
    trigger: {
      ...tpl.trigger,
      serviceId: tpl.serviceId,
      lastFiredAtMs: 0,
      lastRunId: null,
      fireCount: 0,
    },
  }));
}
