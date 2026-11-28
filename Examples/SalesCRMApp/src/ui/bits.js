/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { stageInfo, stageRampColor } from '../lib/forecast.js';
import { stageLabel, t } from '../lib/i18n.js';
import { activityTypeInfo, contactStatusInfo } from '../lib/statuses.js';
import { dueLabel, formatClock, formatDate, initials, sinceLabel, todayISO } from '../lib/util.js';
import { el } from './dom.js';
import { icon } from './icons.js';
import { toast } from './toast.js';

export function pageHead(title, { subtitle = '', actions = [] } = {}) {
  return el(
    'header.page-head',
    {},
    el('div', {}, el('h1.page-title', {}, title), subtitle ? el('p.page-sub', {}, subtitle) : null),
    el(
      'div.page-actions',
      {},
      el(
        'button.icon-btn',
        {
          type: 'button',
          title: t('Search (Cmd/Ctrl+K)'),
          'aria-label': t('Search'),
          onclick: () => window.dispatchEvent(new CustomEvent('crm:search')),
        },
        icon('search', 16)
      ),
      ...actions
    )
  );
}

export function statTile(label, value, note = '', { progress = null } = {}) {
  return el(
    'div.stat-tile',
    {},
    el('span.stat-label', {}, label),
    el('strong.stat-value', {}, value),
    note ? el('span.stat-note', {}, note) : null,
    progress != null
      ? el('div.progress-track', {}, el('div.progress-fill', { style: `width:${Math.min(100, Math.max(0, Math.round(progress * 100)))}%` }))
      : null
  );
}

export function emptyState(iconName, title, subtitle = '', actions = []) {
  return el(
    'div.card.empty-state',
    {},
    el('div', {}, icon(iconName, 28)),
    el('h3', {}, title),
    subtitle ? el('p', { style: 'margin:0;max-width:420px' }, subtitle) : null,
    actions.length ? el('div.chip-row', {}, ...actions) : null
  );
}

export function stageBadge(stageId, stages) {
  const info = stageInfo(stageId, stages);
  const color = stageRampColor(stageId, stages);
  return el(
    'span.stage-badge',
    {},
    el('span', { style: `width:8px;height:8px;border-radius:999px;background:${color};display:inline-block` }),
    stageLabel(info, stages)
  );
}

export function statusPill(statusId) {
  const info = contactStatusInfo(statusId);
  const cls = info.tone === 'good' ? 'pill pill-good' : info.tone === 'accent' ? 'pill pill-accent' : 'pill';
  return el('span', { class: cls }, t(info.label));
}

export function duePill(iso, time = '') {
  const lbl = dueLabel(iso);
  const cls = lbl.status === 'overdue' ? 'pill pill-critical' : lbl.status === 'today' ? 'pill pill-warn' : 'pill';
  return el('span', { class: cls }, `${lbl.text}${time ? ` · ${formatClock(time)}` : ''}`);
}

export function lastTouchChip(lastIso, fallbackCreatedAt) {
  if (!lastIso && !fallbackCreatedAt) return null;
  const iso = lastIso || todayISO(new Date(fallbackCreatedAt));
  const info = sinceLabel(iso);
  return el(
    'span.pill',
    { title: t('Last touch: {date}', { date: formatDate(iso) }) },
    lastIso ? t('Touched {time}', { time: info.text }) : t('Created {time}', { time: info.text })
  );
}

export function avatarEl(name, large = false) {
  return el('span', { class: `avatar${large ? ' avatar-lg' : ''}`, 'aria-hidden': 'true' }, initials(name));
}

export function toggleSwitch(checked, onToggle, { label = '', focusKey = '' } = {}) {
  const btn = el(
    'button.switch',
    {
      type: 'button',
      role: 'switch',
      'aria-checked': String(Boolean(checked)),
      'aria-label': label || undefined,
      'data-focus-key': focusKey || undefined,
      onclick: () => {
        const next = btn.getAttribute('aria-checked') !== 'true';
        btn.setAttribute('aria-checked', String(next));
        onToggle(next);
      },
    },
    el('span.switch-thumb')
  );
  return btn;
}

export function tagChips(tags = [], onTagClick = null) {
  if (!tags?.length) return null;
  return el(
    'div.chip-row',
    {},
    ...tags.map((t) =>
      onTagClick
        ? el('button.pill', { type: 'button', onclick: () => onTagClick(t) }, `#${t}`)
        : el('span.pill', {}, `#${t}`)
    )
  );
}

export function activityRow(act, store, onEdit) {
  const info = activityTypeInfo(act.type);
  const contact = store.contact(act.contactId);
  const deal = store.deal(act.dealId);
  const company = store.company(act.companyId);
  const links = [contact?.name, deal?.name, company?.name].filter(Boolean).join(' · ');
  return el(
    'div.list-row',
    {},
    el(
      'div.row-main',
      {},
      el('span.pill', {}, icon(info.icon, 13), ' ', t(info.label)),
      el(
        'div',
        { style: 'min-width:0;flex:1' },
        el('div.row-title.truncate', {}, act.summary || t('(No summary)')),
        el('div.row-meta.truncate', {}, `${formatDate(act.date)}${links ? ` · ${links}` : ''}`)
      )
    ),
    onEdit
      ? el('button.icon-btn', { type: 'button', 'aria-label': t('Edit activity_aria'), onclick: () => onEdit(act) }, icon('edit', 14))
      : null
  );
}

export function deletedToast(store, entry, label = t('Item')) {
  toast(t('Moved to trash', { label }), {
    duration: 7000,
    action: t('Undo'),
    onAction: () => {
      if (entry?.id) {
        store.restoreTrash(entry.id).catch((err) => {
          toast(err?.message || t('Could not restore item'), { tone: 'danger' });
        });
      }
    },
  });
}
