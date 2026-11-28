/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from './i18n.js';
import { addDays, clamp, todayISO, uid } from './util.js';

export const MAX_PLAYBOOKS = 8;
export const MAX_PLAYBOOK_STEPS = 10;
export const MAX_STEP_OFFSET = 365;
export const MAX_PLAYBOOK_NAME = 60;
export const MAX_STEP_TITLE = 200;

export function normalizeStep(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const title = String(raw.title ?? '').trim().slice(0, MAX_STEP_TITLE);
  if (!title) return null;
  const offsetNum = Number(raw.offset);
  const offset = Number.isFinite(offsetNum) ? clamp(Math.round(offsetNum), 0, MAX_STEP_OFFSET) : 0;
  const out = { title, offset };
  const templateId = String(raw.templateId ?? '').trim();
  if (templateId) out.templateId = templateId;
  return out;
}

export function normalizePlaybooks(raw) {
  if (!Array.isArray(raw)) return [];
  const seen = new Set();
  const out = [];
  for (const item of raw) {
    if (!item || typeof item !== 'object') continue;
    const name = String(item.name ?? '').trim().slice(0, MAX_PLAYBOOK_NAME);
    if (!name) continue;
    let id = String(item.id ?? '').trim();
    if (!id || seen.has(id)) id = uid();
    seen.add(id);
    const steps = (Array.isArray(item.steps) ? item.steps : [])
      .map(normalizeStep)
      .filter(Boolean)
      .sort((a, b) => a.offset - b.offset)
      .slice(0, MAX_PLAYBOOK_STEPS);
    out.push({
      id,
      name,
      trigger: String(item.trigger ?? '').trim(),
      steps,
    });
    if (out.length >= MAX_PLAYBOOKS) break;
  }
  return out;
}

export function playbookTasks(playbook, from = todayISO()) {
  const steps = (playbook?.steps || []).map(normalizeStep).filter(Boolean).sort((a, b) => a.offset - b.offset);
  return steps.map((step) => {
    const item = {
      title: step.title,
      due: step.offset ? addDays(from, step.offset) : from,
    };
    if (step.templateId) item.templateId = step.templateId;
    return item;
  });
}

export function playbookForStage(settings, stageId) {
  if (!stageId) return null;
  return (settings?.playbooks || []).find((p) => p.trigger === stageId) || null;
}

export function offsetLabel(offset) {
  if (!offset) return t('Same day');
  if (offset === 1) return t('In 1 day');
  return t('In {n} days', { n: offset });
}
