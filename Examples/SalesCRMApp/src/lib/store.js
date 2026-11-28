/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { normalizeTouchTargets } from './activity.js';
import { DEFAULT_COLD_DAYS } from './attention.js';
import { DEFAULT_BACKUP_DAYS, readBackup, readSafetySnapshot, writeBackup, writeSafetySnapshot } from './backups.js';
import {
  dealContactIds,
  normalizeDealContacts,
  primaryContactId,
  repointContact,
  replacePrimary,
  withoutContact,
} from './deal-contacts.js';
import { companyDuplicates, contactDuplicates } from './dupes.js';
import { mergeCustomValues, normalizeCustomValues, retypeReport, sanitizeCustomFields } from './fields.js';
import { deleteStoredFile, isAppFilePath } from './files.js';
import {
  DEFAULT_STALE_DAYS,
  isTerminalStage,
  normalizeConfidence,
  resolveStages,
  sanitizeOpenStages,
  stageEnteredAt,
} from './forecast.js';
import { normalizeLanguage } from './i18n.js';
import { normalizePlaybooks, playbookTasks } from './playbooks.js';
import { normalizeBilling, normalizeTerm } from './recurring.js';
import { nextRepeatDue, normalizeRepeat } from './repeat.js';
import { normalizeActivityType, normalizeContactStatus } from './statuses.js';
import { ShardedKVStorage, SINGLETON_KEYS } from './storage.js';
import { cleanTag, normalizeTags, tagKey } from './tags.js';
import { isValidISODate, todayISO, uid } from './util.js';
import { normalizeViewList, sanitizeViews } from './views.js';

export const TRASH_TTL = 30 * 86400000;
export const TRASH_MAX = 200;
export const STAGE_UNDO_WINDOW = 30_000;
export const MAX_STAGE_HISTORY = 50;

export const DEFAULT_SETTINGS = {
  currency: 'USD',
  theme: 'system',
  language: 'auto',
  onboarded: false,
  sampleData: false,
  stageProbabilities: {},
  stages: null,
  customFields: null,
  views: null,
  playbooks: null,
  staleDays: DEFAULT_STALE_DAYS,
  dealView: 'board',
  followupView: 'list',
  revenueGoal: 0,
  touchTargets: null,
  coldDays: DEFAULT_COLD_DAYS,
  notifications: false,
  nextStepNudges: true,
  autoBackupDays: DEFAULT_BACKUP_DAYS,
};

function capStr(raw, max) {
  return String(raw ?? '').slice(0, max);
}

function capTrim(raw, max) {
  return String(raw ?? '').trim().slice(0, max);
}

function sanitizeDate(raw, fallback = null) {
  const s = String(raw ?? '').trim();
  return isValidISODate(s) ? s : fallback;
}

function sanitizeTime(raw) {
  const s = String(raw ?? '').trim();
  return /^([01]\d|2[0-3]):[0-5]\d$/.test(s) ? s : '';
}

function normalizeStageHistory(rawHistory, currentStage, createdAt) {
  const list = Array.isArray(rawHistory)
    ? rawHistory
        .filter((e) => e && typeof e === 'object' && e.stage && Number.isFinite(Number(e.at)))
        .map((e) => ({ stage: String(e.stage), at: Number(e.at) }))
        .sort((a, b) => a.at - b.at)
    : [];
  if (list.length === 0) {
    list.push({ stage: currentStage, at: createdAt });
  } else if (list[list.length - 1].stage !== currentStage) {
    list.push({ stage: currentStage, at: Date.now() });
  }
  if (list.length > MAX_STAGE_HISTORY) {
    return list.slice(list.length - MAX_STAGE_HISTORY);
  }
  return list;
}

export class SalesCRMStore extends EventTarget {
  constructor() {
    super();
    this.storage = new ShardedKVStorage();
    this.settings = { ...DEFAULT_SETTINGS };
    this.companies = [];
    this.contacts = [];
    this.deals = [];
    this.activities = [];
    this.tasks = [];
    this.templates = [];
    this.attachments = [];
    this.trash = [];
    this.backupMeta = null;
    this.notified = {};
    this.webmcpAudit = [];

    this.currentAccount = { id: 'default', label: 'Default', profileName: 'default' };
    this.accounts = [];
    this.systemInfo = null;

    this.#mutations = 0;
    this.#mutateQueue = Promise.resolve();
    this.#lastContactCache = null;
    this.#lastContactStamp = -1;
    this.#linksCache = null;
    this.#linksStamp = -1;
  }

  #mutations;
  #mutateQueue;
  #lastContactCache;
  #lastContactStamp;
  #linksCache;
  #linksStamp;

  #serialized(fn) {
    const next = this.#mutateQueue.then(fn, fn);
    this.#mutateQueue = next.catch(() => {});
    return next;
  }

  get bridge() {
    return typeof window !== 'undefined' && window.ladybirdApp ? window.ladybirdApp : null;
  }

  #emit(detail = {}) {
    this.#mutations++;
    this.#lastContactCache = null;
    this.#linksCache = null;
    this.dispatchEvent(new CustomEvent('change', { detail }));
  }

  #buildLinks() {
    if (this.#linksCache && this.#linksStamp === this.#mutations) return this.#linksCache;
    const companyById = new Map(this.companies.map((c) => [c.id, c]));
    const contactById = new Map(this.contacts.map((c) => [c.id, c]));
    const dealById = new Map(this.deals.map((d) => [d.id, d]));
    const taskById = new Map(this.tasks.map((t) => [t.id, t]));
    const actById = new Map(this.activities.map((a) => [a.id, a]));
    const tplById = new Map(this.templates.map((t) => [t.id, t]));
    const attById = new Map(this.attachments.map((a) => [a.id, a]));
    this.#linksCache = { companyById, contactById, dealById, taskById, actById, tplById, attById };
    this.#linksStamp = this.#mutations;
    return this.#linksCache;
  }

  #lastContactMap() {
    if (this.#lastContactCache && this.#lastContactStamp === this.#mutations) return this.#lastContactCache;
    const map = new Map();
    for (const a of this.activities) {
      if (!a.contactId || !a.date) continue;
      const cur = map.get(a.contactId);
      if (!cur || a.date > cur) map.set(a.contactId, a.date);
    }
    this.#lastContactCache = map;
    this.#lastContactStamp = this.#mutations;
    return map;
  }

  normalizeSettings(raw) {
    const merged = { ...DEFAULT_SETTINGS, ...(raw || {}) };
    merged.stages = sanitizeOpenStages(merged.stages);
    const stages = resolveStages(merged);
    const stageSet = new Set(stages.map((s) => s.id));

    const ov = {};
    if (merged.stageProbabilities && typeof merged.stageProbabilities === 'object') {
      for (const [k, v] of Object.entries(merged.stageProbabilities)) {
        if (!stageSet.has(k) || isTerminalStage(k)) continue;
        const p = normalizeConfidence(v);
        if (p !== null) ov[k] = p;
      }
    }
    merged.stageProbabilities = ov;
    merged.customFields = merged.customFields ? sanitizeCustomFields(merged.customFields) : null;
    merged.views = merged.views ? sanitizeViews(merged.views) : null;
    merged.playbooks = Array.isArray(merged.playbooks) ? normalizePlaybooks(merged.playbooks) : null;
    merged.touchTargets = normalizeTouchTargets(merged.touchTargets);
    merged.staleDays = Math.max(0, Math.round(Number(merged.staleDays) || 0));
    merged.coldDays = Math.max(0, Math.round(Number(merged.coldDays) || 0));
    merged.revenueGoal = Math.max(0, Number(merged.revenueGoal) || 0);
    merged.autoBackupDays = Math.max(0, Math.round(Number(merged.autoBackupDays) ?? DEFAULT_BACKUP_DAYS));
    merged.dealView = merged.dealView === 'list' ? 'list' : 'board';
    merged.followupView = merged.followupView === 'week' || merged.followupView === 'month' ? merged.followupView : 'list';
    merged.theme = merged.theme === 'light' || merged.theme === 'dark' ? merged.theme : 'system';
    merged.language = normalizeLanguage(merged.language);
    return merged;
  }

  normalizeCompany(raw) {
    return {
      id: String(raw?.id || uid()),
      name: capTrim(raw?.name, 500),
      industry: capTrim(raw?.industry, 500),
      website: capTrim(raw?.website, 500),
      size: capTrim(raw?.size, 500),
      notes: capStr(raw?.notes, 65536),
      tags: normalizeTags(raw?.tags),
      custom: normalizeCustomValues(raw?.custom),
      createdAt: Number.isFinite(Number(raw?.createdAt)) ? Number(raw.createdAt) : Date.now(),
    };
  }

  normalizeContact(raw) {
    return {
      id: String(raw?.id || uid()),
      name: capTrim(raw?.name, 500),
      title: capTrim(raw?.title, 500),
      email: capTrim(raw?.email, 500),
      phone: capTrim(raw?.phone, 500),
      companyId: raw?.companyId ? String(raw.companyId) : null,
      source: capTrim(raw?.source, 500),
      status: normalizeContactStatus(raw?.status),
      notes: capStr(raw?.notes, 65536),
      tags: normalizeTags(raw?.tags),
      custom: normalizeCustomValues(raw?.custom),
      createdAt: Number.isFinite(Number(raw?.createdAt)) ? Number(raw.createdAt) : Date.now(),
    };
  }

  normalizeDeal(raw, stages = this.resolvedStages()) {
    const validStages = new Set(stages.map((s) => s.id));
    const createdAt = Number.isFinite(Number(raw?.createdAt)) ? Number(raw.createdAt) : Date.now();
    const updatedAt = Number.isFinite(Number(raw?.updatedAt)) ? Number(raw.updatedAt) : createdAt;
    const billing = normalizeBilling(raw?.billing);
    const term = normalizeTerm(raw?.term, billing);
    const rawStage = String(raw?.stage || '');
    const stage = validStages.has(rawStage) ? rawStage : stages[0].id;
    const contacts = normalizeDealContacts(raw?.contacts, raw?.contactId || null);
    const contactId = contacts[0]?.contactId ?? null;
    const terminal = isTerminalStage(stage);
    const stageHistory = normalizeStageHistory(raw?.stageHistory, stage, createdAt);
    const closedAt = terminal
      ? Number.isFinite(Number(raw?.closedAt))
        ? Number(raw.closedAt)
        : stageHistory[stageHistory.length - 1]?.at || updatedAt
      : null;

    return {
      id: String(raw?.id || uid()),
      name: capTrim(raw?.name, 500),
      value: Math.max(0, Number(raw?.value) || 0),
      billing,
      term,
      stage,
      contactId,
      contacts,
      companyId: raw?.companyId ? String(raw.companyId) : null,
      expectedClose: sanitizeDate(raw?.expectedClose, null),
      notes: capStr(raw?.notes, 65536),
      tags: normalizeTags(raw?.tags),
      custom: normalizeCustomValues(raw?.custom),
      confidence: terminal ? null : normalizeConfidence(raw?.confidence),
      closedAt,
      closeReason: terminal ? capTrim(raw?.closeReason, 200) : '',
      closeReasonNote: terminal ? capTrim(raw?.closeReasonNote, 500) : '',
      createdAt,
      updatedAt,
      stageHistory,
    };
  }

  normalizeActivity(raw) {
    return {
      id: String(raw?.id || uid()),
      type: normalizeActivityType(raw?.type),
      date: sanitizeDate(raw?.date, todayISO()),
      summary: capTrim(raw?.summary, 500),
      notes: capStr(raw?.notes, 65536),
      contactId: raw?.contactId ? String(raw.contactId) : null,
      companyId: raw?.companyId ? String(raw.companyId) : null,
      dealId: raw?.dealId ? String(raw.dealId) : null,
      createdAt: Number.isFinite(Number(raw?.createdAt)) ? Number(raw.createdAt) : Date.now(),
    };
  }

  normalizeTask(raw) {
    const done = Boolean(raw?.done);
    return {
      id: String(raw?.id || uid()),
      title: capTrim(raw?.title, 500),
      due: sanitizeDate(raw?.due, todayISO()),
      dueTime: sanitizeTime(raw?.dueTime),
      done,
      doneAt: done ? (Number.isFinite(Number(raw?.doneAt)) ? Number(raw.doneAt) : Date.now()) : null,
      contactId: raw?.contactId ? String(raw.contactId) : null,
      dealId: raw?.dealId ? String(raw.dealId) : null,
      templateId: raw?.templateId ? String(raw.templateId) : null,
      repeat: normalizeRepeat(raw?.repeat),
      spawnedId: raw?.spawnedId ? String(raw.spawnedId) : null,
      createdAt: Number.isFinite(Number(raw?.createdAt)) ? Number(raw.createdAt) : Date.now(),
    };
  }

  normalizeTemplate(raw) {
    return {
      id: String(raw?.id || uid()),
      name: capTrim(raw?.name, 500),
      subject: capStr(raw?.subject, 500),
      body: capStr(raw?.body, 65536),
      createdAt: Number.isFinite(Number(raw?.createdAt)) ? Number(raw.createdAt) : Date.now(),
    };
  }

  normalizeAttachment(raw) {
    return {
      id: String(raw?.id || uid()),
      name: String(raw?.name ?? '').trim(),
      path: isAppFilePath(raw?.path) ? raw.path : '',
      size: Math.max(0, Number(raw?.size) || 0),
      type: String(raw?.type ?? ''),
      contactId: raw?.contactId ? String(raw.contactId) : null,
      companyId: raw?.companyId ? String(raw.companyId) : null,
      dealId: raw?.dealId ? String(raw.dealId) : null,
      uploadedAt: Number.isFinite(Number(raw?.uploadedAt)) ? Number(raw.uploadedAt) : Date.now(),
    };
  }

  async init() {
    if (this.bridge) {
      const [sys, acct, list] = await Promise.all([
        this.bridge.system.info().catch(() => null),
        this.bridge.accounts.current().catch(() => null),
        this.bridge.accounts.list().catch(() => []),
      ]);
      this.systemInfo = sys;
      if (acct) this.currentAccount = acct;
      if (Array.isArray(list)) this.accounts = list;
    }

    const loaded = await this.storage.loadAll();
    this.settings = this.normalizeSettings(loaded.settings);
    const stages = resolveStages(this.settings);
    this.companies = (loaded.companies || []).map((c) => this.normalizeCompany(c));
    this.contacts = (loaded.contacts || []).map((c) => this.normalizeContact(c));
    this.deals = (loaded.deals || []).map((d) => this.normalizeDeal(d, stages));
    this.activities = (loaded.activities || []).map((a) => this.normalizeActivity(a));
    this.tasks = (loaded.tasks || []).map((t) => this.normalizeTask(t));
    this.templates = (loaded.templates || []).map((t) => this.normalizeTemplate(t));
    this.attachments = (loaded.attachments || []).map((a) => this.normalizeAttachment(a)).filter((a) => a.path);
    this.trash = Array.isArray(loaded.trash) ? loaded.trash : [];
    this.backupMeta = loaded.backupMeta || null;
    this.notified = loaded.notified || {};
    this.webmcpAudit = loaded.webmcpAudit || [];

    const pruned = this.#pruneTrash();
    const purged = this.#purgeOrphanAttachments();
    if (loaded.legacyTrashPresent) {
      await this.storage.saveCollection('trash', this.trash);
      await this.storage.rawDelete(SINGLETON_KEYS.trash).catch(() => {});
    } else if (pruned.length > 0 || purged.length > 0) {
      await Promise.all([
        pruned.length ? this.storage.saveCollection('trash', this.trash) : Promise.resolve(),
        purged.length ? this.storage.saveSingleton('attachments', this.attachments) : Promise.resolve(),
      ]);
      await this.#reclaimFiles([...pruned, ...purged]);
    }
    this.#emit({ type: 'init' });
  }

  hasData() {
    return (
      this.companies.length > 0 ||
      this.contacts.length > 0 ||
      this.deals.length > 0 ||
      this.activities.length > 0 ||
      this.tasks.length > 0 ||
      this.templates.length > 0 ||
      this.attachments.length > 0
    );
  }

  resolvedStages() {
    return resolveStages(this.settings);
  }

  firstStage() {
    return this.resolvedStages()[0].id;
  }

  company(id) {
    return id ? this.#buildLinks().companyById.get(id) || null : null;
  }

  contact(id) {
    return id ? this.#buildLinks().contactById.get(id) || null : null;
  }

  deal(id) {
    return id ? this.#buildLinks().dealById.get(id) || null : null;
  }

  task(id) {
    return id ? this.#buildLinks().taskById.get(id) || null : null;
  }

  activity(id) {
    return id ? this.#buildLinks().actById.get(id) || null : null;
  }

  template(id) {
    return id ? this.#buildLinks().tplById.get(id) || null : null;
  }

  attachment(id) {
    return id ? this.#buildLinks().attById.get(id) || null : null;
  }

  contactsFor(companyId) {
    return this.contacts.filter((c) => c.companyId === companyId);
  }

  dealsFor(contactId) {
    return this.deals.filter((d) => dealContactIds(d).includes(contactId));
  }

  dealsForCompany(companyId) {
    return this.deals.filter((d) => d.companyId === companyId);
  }

  activitiesFor(kind, id) {
    const key = kind === 'contact' ? 'contactId' : kind === 'company' ? 'companyId' : 'dealId';
    return this.activities
      .filter((a) => a[key] === id)
      .sort((a, b) => String(b.date).localeCompare(String(a.date)) || b.createdAt - a.createdAt);
  }

  dealActivities(deal) {
    if (!deal) return [];
    const cids = new Set(dealContactIds(deal));
    return this.activities
      .filter((a) => a.dealId === deal.id || (!a.dealId && a.contactId && cids.has(a.contactId)))
      .sort((a, b) => String(b.date).localeCompare(String(a.date)) || b.createdAt - a.createdAt);
  }

  tasksFor(kind, id) {
    const key = kind === 'contact' ? 'contactId' : 'dealId';
    return this.tasks
      .filter((t) => t[key] === id)
      .sort((a, b) => Number(a.done) - Number(b.done) || String(a.due).localeCompare(String(b.due)));
  }

  openTasks() {
    return this.tasks
      .filter((t) => !t.done)
      .sort((a, b) => String(a.due).localeCompare(String(b.due)) || String(a.dueTime || '99:99').localeCompare(String(b.dueTime || '99:99')));
  }

  hasOpenTask(dealId) {
    return this.tasks.some((t) => !t.done && t.dealId === dealId);
  }

  hasOpenFollowUp(contactId, dealId) {
    return this.tasks.some((t) => !t.done && ((dealId && t.dealId === dealId) || (contactId && t.contactId === contactId)));
  }

  lastContactedAt(contactId) {
    return this.#lastContactMap().get(contactId) || null;
  }

  lastTouchedDeal(deal) {
    if (!deal) return null;
    let best = null;
    const cids = new Set(dealContactIds(deal));
    for (const a of this.activities) {
      if (a.dealId === deal.id || (a.contactId && cids.has(a.contactId))) {
        if (a.date && (!best || a.date > best)) best = a.date;
      }
    }
    return best;
  }

  attachmentsFor(kind, id) {
    const key = kind === 'contact' ? 'contactId' : kind === 'company' ? 'companyId' : 'dealId';
    return this.attachments.filter((a) => a[key] === id).sort((a, b) => b.uploadedAt - a.uploadedAt);
  }

  customFieldsFor(kind) {
    return this.settings.customFields?.[kind] || [];
  }

  allTags() {
    const map = new Map();
    for (const list of [this.companies, this.contacts, this.deals]) {
      for (const rec of list) {
        for (const t of rec.tags || []) {
          const k = tagKey(t);
          if (!map.has(k)) map.set(k, t);
        }
      }
    }
    return [...map.values()].sort((a, b) => a.localeCompare(b));
  }

  getNotifiedMap() {
    return this.notified || {};
  }

  saveNotifiedMap(map) {
    this.notified = { ...(map || {}) };
    return this.storage.saveSingleton('notified', this.notified);
  }

  recordWebMCP(entry) {
    this.webmcpAudit.unshift({
      ts: new Date().toISOString().slice(11, 19),
      tool: String(entry.tool || ''),
      tier: String(entry.tier || ''),
      ok: Boolean(entry.ok),
      note: String(entry.note || '').replace(/[\r\n]+/g, ' ').slice(0, 120),
    });
    if (this.webmcpAudit.length > 100) this.webmcpAudit.length = 100;
    this.#emit({ type: 'webmcp' });
    this.storage.saveSingleton('webmcpAudit', this.webmcpAudit).catch(() => {});
  }

  async createAccount(id, label = '') {
    if (!this.bridge) throw new Error('Ladybird accounts bridge unavailable');
    const created = await this.bridge.accounts.create(id, label || id);
    this.accounts = await this.bridge.accounts.list().catch(() => this.accounts);
    this.#emit({ type: 'accounts' });
    return created;
  }

  async launchAccount(id, { headless = false, webmcpOnly = false } = {}) {
    if (!this.bridge) throw new Error('Ladybird accounts bridge unavailable');
    return this.bridge.accounts.launch(id, { headless, webmcpOnly });
  }

  async updateSettings(patch) {
    this.settings = this.normalizeSettings({ ...this.settings, ...(patch || {}) });
    this.#emit({ type: 'settings' });
    await this.storage.saveSingleton('settings', this.settings);
    return this.settings;
  }

  async setStages(nextOpenStages, reassignMap = {}) {
    return this.#serialized(async () => {
      const cleaned = sanitizeOpenStages(nextOpenStages);
      if (!cleaned) throw new Error('At least one open stage is required');
      const nextStages = resolveStages({ stages: cleaned });
      const valid = new Set(nextStages.map((s) => s.id));
      const fallback = nextStages[0].id;
      let dealsChanged = false;
      const now = Date.now();

      for (const d of this.deals) {
        if (!valid.has(d.stage)) {
          const mapped = reassignMap[d.stage];
          const target = mapped && valid.has(mapped) ? mapped : fallback;
          d.stage = target;
          d.stageHistory = normalizeStageHistory(d.stageHistory, target, d.createdAt);
          if (isTerminalStage(target)) {
            d.closedAt = d.closedAt || now;
            d.confidence = null;
          } else {
            d.closedAt = null;
            d.closeReason = '';
            d.closeReasonNote = '';
          }
          d.updatedAt = now;
          Object.assign(d, this.normalizeDeal(d, nextStages));
          dealsChanged = true;
        }
      }

      const cleanedIds = new Set(cleaned.map((s) => s.id));
      const nextProbs = {};
      for (const [k, v] of Object.entries(this.settings.stageProbabilities || {})) {
        if (valid.has(k) && !isTerminalStage(k) && !cleanedIds.has(k)) nextProbs[k] = v;
      }

      this.settings = this.normalizeSettings({ ...this.settings, stages: cleaned, stageProbabilities: nextProbs });
      this.#emit({ type: 'stages' });
      const saves = [this.storage.saveSingleton('settings', this.settings)];
      if (dealsChanged) saves.push(this.storage.saveCollection('deals', this.deals));
      await Promise.all(saves);
      return nextStages;
    });
  }

  async setCustomFields(kind, nextFields) {
    const currentList = this.customFieldsFor(kind);
    const collection = kind === 'contacts' ? this.contacts : kind === 'companies' ? this.companies : this.deals;
    const prevMap = new Map(currentList.map((f) => [f.id, f]));
    const updates = [];

    for (const nextDef of nextFields || []) {
      const prev = prevMap.get(nextDef.id);
      if (prev && prev.type !== nextDef.type) {
        const rep = retypeReport(collection, prev, nextDef.type);
        if (rep.blocked.length > 0) {
          const err = new Error('custom-field-type');
          err.blocked = rep.blocked;
          err.field = prev;
          err.targetType = nextDef.type;
          throw err;
        }
        updates.push({ fieldId: prev.id, values: new Map(rep.nextValues.map((v) => [v.id, v.value])) });
      }
    }

    if (updates.length > 0) {
      for (const rec of collection) {
        for (const u of updates) {
          if (u.values.has(rec.id)) {
            const val = u.values.get(rec.id);
            if (val === null) delete rec.custom[u.fieldId];
            else rec.custom[u.fieldId] = val;
          }
        }
      }
    }

    const all = sanitizeCustomFields({ ...(this.settings.customFields || {}), [kind]: nextFields });
    this.settings = this.normalizeSettings({ ...this.settings, customFields: all });
    this.#emit({ type: 'customFields' });
    const saves = [this.storage.saveSingleton('settings', this.settings)];
    if (updates.length > 0) saves.push(this.storage.saveCollection(kind, collection));
    await Promise.all(saves);
    return this.customFieldsFor(kind);
  }

  async setViews(kind, list) {
    const next = { ...(this.settings.views || {}), [kind]: normalizeViewList(kind, list) };
    return this.updateSettings({ views: next });
  }

  async setPlaybooks(list) {
    return this.updateSettings({ playbooks: normalizePlaybooks(list) });
  }

  async addCompany(data) {
    const rec = this.normalizeCompany({ ...data, id: data?.id || uid(), createdAt: data?.createdAt || Date.now() });
    this.companies.push(rec);
    this.#emit({ type: 'companies' });
    await this.storage.saveCollection('companies', this.companies);
    return rec;
  }

  async updateCompany(id, patch) {
    const rec = this.company(id);
    if (!rec) return null;
    Object.assign(rec, this.normalizeCompany({ ...rec, ...patch, id: rec.id, createdAt: rec.createdAt }));
    this.#emit({ type: 'companies' });
    await this.storage.saveCollection('companies', this.companies);
    return rec;
  }

  async deleteCompany(id) {
    return this.#serialized(async () => {
      const idx = this.companies.findIndex((c) => c.id === id);
      if (idx === -1) return null;
      const [removed] = this.companies.splice(idx, 1);
      const refs = { contacts: [], deals: [], activities: [] };
      for (const c of this.contacts) {
        if (c.companyId === id) {
          c.companyId = null;
          refs.contacts.push(c.id);
        }
      }
      for (const d of this.deals) {
        if (d.companyId === id) {
          d.companyId = null;
          refs.deals.push(d.id);
        }
      }
      for (const a of this.activities) {
        if (a.companyId === id) {
          a.companyId = null;
          refs.activities.push(a.id);
        }
      }
      const entry = await this.#toTrash('company', removed, refs);
      this.#emit({ type: 'delete' });
      await Promise.all([
        this.storage.saveCollection('companies', this.companies),
        refs.contacts.length ? this.storage.saveCollection('contacts', this.contacts) : Promise.resolve(),
        refs.deals.length ? this.storage.saveCollection('deals', this.deals) : Promise.resolve(),
        refs.activities.length ? this.storage.saveCollection('activities', this.activities) : Promise.resolve(),
      ]);
      return entry;
    });
  }

  async bulkDeleteCompanies(ids) {
    return this.#serialized(async () => {
      const set = new Set(ids || []);
      const removed = [];
      for (const id of set) {
        const rec = this.company(id);
        if (rec) removed.push(rec);
      }
      if (removed.length === 0) return [];
      this.companies = this.companies.filter((c) => !set.has(c.id));
      const touchedContacts = [];
      const touchedDeals = [];
      const touchedActs = [];
      for (const c of this.contacts) {
        if (c.companyId && set.has(c.companyId)) {
          touchedContacts.push({ id: c.id, old: c.companyId });
          c.companyId = null;
        }
      }
      for (const d of this.deals) {
        if (d.companyId && set.has(d.companyId)) {
          touchedDeals.push({ id: d.id, old: d.companyId });
          d.companyId = null;
        }
      }
      for (const a of this.activities) {
        if (a.companyId && set.has(a.companyId)) {
          touchedActs.push({ id: a.id, old: a.companyId });
          a.companyId = null;
        }
      }

      const entries = [];
      for (const co of removed) {
        const refs = {
          contacts: touchedContacts.filter((x) => x.old === co.id).map((x) => x.id),
          deals: touchedDeals.filter((x) => x.old === co.id).map((x) => x.id),
          activities: touchedActs.filter((x) => x.old === co.id).map((x) => x.id),
        };
        entries.push(await this.#toTrash('company', co, refs, false));
      }
      const dropped = this.#pruneTrash();
      this.#emit({ type: 'delete' });
      await Promise.all([
        this.storage.saveCollection('companies', this.companies),
        this.storage.saveCollection('trash', this.trash),
        touchedContacts.length ? this.storage.saveCollection('contacts', this.contacts) : Promise.resolve(),
        touchedDeals.length ? this.storage.saveCollection('deals', this.deals) : Promise.resolve(),
        touchedActs.length ? this.storage.saveCollection('activities', this.activities) : Promise.resolve(),
      ]);
      await this.#reclaimFiles(dropped);
      return entries;
    });
  }

  async addContact(data) {
    const rec = this.normalizeContact({ ...data, id: data?.id || uid(), createdAt: data?.createdAt || Date.now() });
    this.contacts.push(rec);
    this.#emit({ type: 'contacts' });
    await this.storage.saveCollection('contacts', this.contacts);
    return rec;
  }

  async updateContact(id, patch) {
    const rec = this.contact(id);
    if (!rec) return null;
    Object.assign(rec, this.normalizeContact({ ...rec, ...patch, id: rec.id, createdAt: rec.createdAt }));
    this.#emit({ type: 'contacts' });
    await this.storage.saveCollection('contacts', this.contacts);
    return rec;
  }

  async deleteContact(id) {
    return this.#serialized(async () => {
      const idx = this.contacts.findIndex((c) => c.id === id);
      if (idx === -1) return null;
      const [removed] = this.contacts.splice(idx, 1);
      const refs = { deals: [], activities: [], tasks: [] };

      for (const d of this.deals) {
        if (dealContactIds(d).includes(id)) {
          refs.deals.push(d.id);
          d.contacts = withoutContact(d.contacts, id);
          d.contactId = primaryContactId(d);
        }
      }
      for (const a of this.activities) {
        if (a.contactId === id) {
          a.contactId = null;
          refs.activities.push(a.id);
        }
      }
      for (const t of this.tasks) {
        if (t.contactId === id) {
          t.contactId = null;
          refs.tasks.push(t.id);
        }
      }

      const entry = await this.#toTrash('contact', removed, refs);
      this.#emit({ type: 'delete' });
      await Promise.all([
        this.storage.saveCollection('contacts', this.contacts),
        refs.deals.length ? this.storage.saveCollection('deals', this.deals) : Promise.resolve(),
        refs.activities.length ? this.storage.saveCollection('activities', this.activities) : Promise.resolve(),
        refs.tasks.length ? this.storage.saveCollection('tasks', this.tasks) : Promise.resolve(),
      ]);
      return entry;
    });
  }

  async bulkUpdateContacts(ids, patch) {
    const set = new Set(ids || []);
    let changed = 0;
    for (const c of this.contacts) {
      if (!set.has(c.id)) continue;
      Object.assign(c, this.normalizeContact({ ...c, ...patch, id: c.id, createdAt: c.createdAt }));
      changed++;
    }
    if (changed > 0) {
      this.#emit({ type: 'contacts' });
      await this.storage.saveCollection('contacts', this.contacts);
    }
    return changed;
  }

  async bulkDeleteContacts(ids) {
    return this.#serialized(async () => {
      const set = new Set(ids || []);
      const removed = [];
      for (const id of set) {
        const rec = this.contact(id);
        if (rec) removed.push(rec);
      }
      if (removed.length === 0) return [];
      this.contacts = this.contacts.filter((c) => !set.has(c.id));

      const refsById = new Map(removed.map((c) => [c.id, { deals: [], activities: [], tasks: [] }]));
      let dealsChanged = false;
      let actsChanged = false;
      let tasksChanged = false;

      for (const d of this.deals) {
        const hits = dealContactIds(d).filter((cid) => set.has(cid));
        if (hits.length > 0) {
          dealsChanged = true;
          for (const cid of hits) {
            refsById.get(cid).deals.push(d.id);
            d.contacts = withoutContact(d.contacts, cid);
          }
          d.contactId = primaryContactId(d);
        }
      }
      for (const a of this.activities) {
        if (a.contactId && set.has(a.contactId)) {
          refsById.get(a.contactId).activities.push(a.id);
          a.contactId = null;
          actsChanged = true;
        }
      }
      for (const t of this.tasks) {
        if (t.contactId && set.has(t.contactId)) {
          refsById.get(t.contactId).tasks.push(t.id);
          t.contactId = null;
          tasksChanged = true;
        }
      }

      const entries = [];
      for (const ct of removed) {
        entries.push(await this.#toTrash('contact', ct, refsById.get(ct.id), false));
      }
      const dropped = this.#pruneTrash();
      this.#emit({ type: 'delete' });
      await Promise.all([
        this.storage.saveCollection('contacts', this.contacts),
        this.storage.saveCollection('trash', this.trash),
        dealsChanged ? this.storage.saveCollection('deals', this.deals) : Promise.resolve(),
        actsChanged ? this.storage.saveCollection('activities', this.activities) : Promise.resolve(),
        tasksChanged ? this.storage.saveCollection('tasks', this.tasks) : Promise.resolve(),
      ]);
      await this.#reclaimFiles(dropped);
      return entries;
    });
  }

  #recordStageEntry(deal, nextStage, at) {
    const hist = Array.isArray(deal.stageHistory) ? deal.stageHistory : [];
    const last = hist[hist.length - 1];
    if (last && last.stage === nextStage) return;
    if (hist.length > 1 && hist[hist.length - 2].stage === nextStage && last && at - last.at < STAGE_UNDO_WINDOW) {
      hist.pop();
    } else {
      hist.push({ stage: nextStage, at });
      if (hist.length > MAX_STAGE_HISTORY) hist.splice(0, hist.length - MAX_STAGE_HISTORY);
    }
    deal.stageHistory = hist;
  }

  async addDeal(data) {
    const stages = this.resolvedStages();
    const rec = this.normalizeDeal({ ...data, id: data?.id || uid() }, stages);
    if (isTerminalStage(rec.stage) && !data?.closedAt) {
      rec.closedAt = Date.now();
    }
    this.deals.push(rec);
    this.#emit({ type: 'deals' });
    await this.storage.saveCollection('deals', this.deals);
    return rec;
  }

  async updateDeal(id, patch) {
    const rec = this.deal(id);
    if (!rec) return null;
    const now = Date.now();
    const stages = this.resolvedStages();
    const prevStage = rec.stage;

    let nextContacts = rec.contacts;
    if ('contacts' in (patch || {})) {
      nextContacts = normalizeDealContacts(patch.contacts);
    } else if ('contactId' in (patch || {})) {
      nextContacts = replacePrimary(rec.contacts, patch.contactId);
    }

    const nextStage = patch?.stage && stages.some((s) => s.id === patch.stage) ? patch.stage : prevStage;
    if (nextStage !== prevStage) {
      this.#recordStageEntry(rec, nextStage, now);
    }

    const merged = {
      ...rec,
      ...patch,
      id: rec.id,
      createdAt: rec.createdAt,
      updatedAt: now,
      stage: nextStage,
      stageHistory: rec.stageHistory,
      contacts: nextContacts,
      contactId: nextContacts[0]?.contactId ?? null,
    };

    if (isTerminalStage(nextStage)) {
      if (nextStage !== prevStage) {
        merged.closedAt = stageEnteredAt(merged) || now;
        if (isTerminalStage(prevStage)) {
          merged.closeReason = patch?.closeReason !== undefined ? patch.closeReason : '';
          merged.closeReasonNote = patch?.closeReasonNote !== undefined ? patch.closeReasonNote : '';
        }
      }
      merged.confidence = null;
    } else {
      merged.closedAt = null;
      merged.closeReason = '';
      merged.closeReasonNote = '';
    }

    Object.assign(rec, this.normalizeDeal(merged, stages));
    this.#emit({ type: 'deals' });
    await this.storage.saveCollection('deals', this.deals);
    return rec;
  }

  async moveDeal(id, nextStage) {
    return this.updateDeal(id, { stage: nextStage });
  }

  async setCloseReason(id, reason, note = '') {
    const rec = this.deal(id);
    if (!rec || !isTerminalStage(rec.stage)) return rec;
    rec.closeReason = capTrim(reason, 200);
    rec.closeReasonNote = capTrim(note, 500);
    rec.updatedAt = Date.now();
    this.#emit({ type: 'deals' });
    await this.storage.saveCollection('deals', this.deals);
    return rec;
  }

  async bulkMoveDeals(ids, nextStage) {
    return this.#serialized(async () => {
      const stages = this.resolvedStages();
      if (!stages.some((s) => s.id === nextStage)) return 0;
      const set = new Set(ids || []);
      const now = Date.now();
      let changed = 0;

      for (const rec of this.deals) {
        if (!set.has(rec.id) || rec.stage === nextStage) continue;
        const prevStage = rec.stage;
        this.#recordStageEntry(rec, nextStage, now);
        const merged = {
          ...rec,
          stage: nextStage,
          updatedAt: now,
          stageHistory: rec.stageHistory,
        };
        if (isTerminalStage(nextStage)) {
          merged.closedAt = stageEnteredAt(merged) || now;
          if (isTerminalStage(prevStage)) {
            merged.closeReason = '';
            merged.closeReasonNote = '';
          }
          merged.confidence = null;
        } else {
          merged.closedAt = null;
          merged.closeReason = '';
          merged.closeReasonNote = '';
        }
        Object.assign(rec, this.normalizeDeal(merged, stages));
        changed++;
      }

      if (changed > 0) {
        this.#emit({ type: 'deals' });
        await this.storage.saveCollection('deals', this.deals);
      }
      return changed;
    });
  }

  async deleteDeal(id) {
    return this.#serialized(async () => {
      const idx = this.deals.findIndex((d) => d.id === id);
      if (idx === -1) return null;
      const [removed] = this.deals.splice(idx, 1);
      const refs = { activities: [], tasks: [] };
      for (const a of this.activities) {
        if (a.dealId === id) {
          a.dealId = null;
          refs.activities.push(a.id);
        }
      }
      for (const t of this.tasks) {
        if (t.dealId === id) {
          t.dealId = null;
          refs.tasks.push(t.id);
        }
      }
      const entry = await this.#toTrash('deal', removed, refs);
      this.#emit({ type: 'delete' });
      await Promise.all([
        this.storage.saveCollection('deals', this.deals),
        refs.activities.length ? this.storage.saveCollection('activities', this.activities) : Promise.resolve(),
        refs.tasks.length ? this.storage.saveCollection('tasks', this.tasks) : Promise.resolve(),
      ]);
      return entry;
    });
  }

  async bulkDeleteDeals(ids) {
    return this.#serialized(async () => {
      const set = new Set(ids || []);
      const removed = [];
      for (const id of set) {
        const rec = this.deal(id);
        if (rec) removed.push(rec);
      }
      if (removed.length === 0) return [];
      this.deals = this.deals.filter((d) => !set.has(d.id));

      const refsById = new Map(removed.map((d) => [d.id, { activities: [], tasks: [] }]));
      let actsChanged = false;
      let tasksChanged = false;

      for (const a of this.activities) {
        if (a.dealId && set.has(a.dealId)) {
          refsById.get(a.dealId).activities.push(a.id);
          a.dealId = null;
          actsChanged = true;
        }
      }
      for (const t of this.tasks) {
        if (t.dealId && set.has(t.dealId)) {
          refsById.get(t.dealId).tasks.push(t.id);
          t.dealId = null;
          tasksChanged = true;
        }
      }

      const entries = [];
      for (const dl of removed) {
        entries.push(await this.#toTrash('deal', dl, refsById.get(dl.id), false));
      }
      const dropped = this.#pruneTrash();
      this.#emit({ type: 'delete' });
      await Promise.all([
        this.storage.saveCollection('deals', this.deals),
        this.storage.saveCollection('trash', this.trash),
        actsChanged ? this.storage.saveCollection('activities', this.activities) : Promise.resolve(),
        tasksChanged ? this.storage.saveCollection('tasks', this.tasks) : Promise.resolve(),
      ]);
      await this.#reclaimFiles(dropped);
      return entries;
    });
  }

  async addActivity(data) {
    const rec = this.normalizeActivity({ ...data, id: data?.id || uid(), createdAt: data?.createdAt || Date.now() });
    if (!rec.companyId) {
      const d = rec.dealId ? this.deal(rec.dealId) : null;
      const c = rec.contactId ? this.contact(rec.contactId) : null;
      rec.companyId = d?.companyId || c?.companyId || null;
    }
    this.activities.push(rec);
    this.#emit({ type: 'activities' });
    await this.storage.saveCollection('activities', this.activities);
    return rec;
  }

  async updateActivity(id, patch) {
    const rec = this.activity(id);
    if (!rec) return null;
    Object.assign(rec, this.normalizeActivity({ ...rec, ...patch, id: rec.id, createdAt: rec.createdAt }));
    this.#emit({ type: 'activities' });
    await this.storage.saveCollection('activities', this.activities);
    return rec;
  }

  async deleteActivity(id) {
    const idx = this.activities.findIndex((a) => a.id === id);
    if (idx === -1) return null;
    const [removed] = this.activities.splice(idx, 1);
    const entry = await this.#toTrash('activity', removed, {});
    this.#emit({ type: 'delete' });
    await this.storage.saveCollection('activities', this.activities);
    return entry;
  }

  async addTask(data) {
    const rec = this.normalizeTask({ ...data, id: data?.id || uid(), createdAt: data?.createdAt || Date.now() });
    this.tasks.push(rec);
    this.#emit({ type: 'tasks' });
    await this.storage.saveCollection('tasks', this.tasks);
    return rec;
  }

  async updateTask(id, patch) {
    const rec = this.task(id);
    if (!rec) return null;
    Object.assign(rec, this.normalizeTask({ ...rec, ...patch, id: rec.id, createdAt: rec.createdAt }));
    this.#emit({ type: 'tasks' });
    await this.storage.saveCollection('tasks', this.tasks);
    return rec;
  }

  async moveTask(id, dueISO) {
    if (!isValidISODate(dueISO)) return null;
    return this.updateTask(id, { due: dueISO });
  }

  async toggleTask(id, done = null) {
    const rec = this.task(id);
    if (!rec) return null;
    const nextDone = done === null ? !rec.done : Boolean(done);
    let spawned = null;

    if (nextDone && !rec.done) {
      rec.done = true;
      rec.doneAt = Date.now();
      if (rec.repeat) {
        const nextDue = nextRepeatDue(todayISO(), rec.repeat);
        if (nextDue) {
          spawned = this.normalizeTask({
            id: uid(),
            title: rec.title,
            due: nextDue,
            dueTime: rec.dueTime,
            done: false,
            doneAt: null,
            contactId: rec.contactId,
            dealId: rec.dealId,
            templateId: rec.templateId,
            repeat: rec.repeat,
            createdAt: Date.now(),
          });
          this.tasks.push(spawned);
          rec.spawnedId = spawned.id;
        }
      }
    } else if (!nextDone && rec.done) {
      rec.done = false;
      rec.doneAt = null;
      if (rec.spawnedId) {
        const child = this.task(rec.spawnedId);
        if (child && !child.done) {
          this.tasks = this.tasks.filter((t) => t.id !== child.id);
        }
        rec.spawnedId = null;
      }
    }

    this.#emit({ type: 'tasks' });
    await this.storage.saveCollection('tasks', this.tasks);
    return { task: rec, spawned };
  }

  async deleteTask(id) {
    const idx = this.tasks.findIndex((t) => t.id === id);
    if (idx === -1) return null;
    const [removed] = this.tasks.splice(idx, 1);
    const entry = await this.#toTrash('task', removed, {});
    this.#emit({ type: 'delete' });
    await this.storage.saveCollection('tasks', this.tasks);
    return entry;
  }

  async applyPlaybook(playbook, { dealId = null, contactId = null, from = todayISO() } = {}) {
    const specs = playbookTasks(playbook, from);
    const created = [];
    for (const s of specs) {
      const task = this.normalizeTask({
        id: uid(),
        title: s.title,
        due: s.due,
        templateId: s.templateId || null,
        dealId,
        contactId,
        done: false,
        createdAt: Date.now(),
      });
      this.tasks.push(task);
      created.push(task);
    }
    this.#emit({ type: 'tasks' });
    await this.storage.saveCollection('tasks', this.tasks);
    return created;
  }

  async undoAddedTasks(ids) {
    const set = new Set(ids || []);
    const before = this.tasks.length;
    this.tasks = this.tasks.filter((t) => !set.has(t.id));
    if (this.tasks.length !== before) {
      this.#emit({ type: 'tasks' });
      await this.storage.saveCollection('tasks', this.tasks);
    }
    return before - this.tasks.length;
  }

  async addTemplate(data) {
    const rec = this.normalizeTemplate({ ...data, id: uid(), createdAt: Date.now() });
    this.templates.push(rec);
    this.#emit({ type: 'templates' });
    await this.storage.saveSingleton('templates', this.templates);
    return rec;
  }

  async updateTemplate(id, patch) {
    const rec = this.template(id);
    if (!rec) return null;
    Object.assign(rec, this.normalizeTemplate({ ...rec, ...patch, id: rec.id, createdAt: rec.createdAt }));
    this.#emit({ type: 'templates' });
    await this.storage.saveSingleton('templates', this.templates);
    return rec;
  }

  async deleteTemplate(id) {
    const idx = this.templates.findIndex((t) => t.id === id);
    if (idx === -1) return null;
    const [removed] = this.templates.splice(idx, 1);
    const entry = await this.#toTrash('template', removed, {});
    this.#emit({ type: 'delete' });
    await this.storage.saveSingleton('templates', this.templates);
    return entry;
  }

  async addAttachment(data) {
    const rec = this.normalizeAttachment({ ...data, id: data?.id || uid() });
    this.attachments.push(rec);
    this.#emit({ type: 'attachments' });
    await this.storage.saveSingleton('attachments', this.attachments);
    return rec;
  }

  async deleteAttachment(id) {
    const idx = this.attachments.findIndex((a) => a.id === id);
    if (idx === -1) return null;
    const [removed] = this.attachments.splice(idx, 1);
    const entry = await this.#toTrash('attachment', removed, {});
    this.#emit({ type: 'attachments' });
    await this.storage.saveSingleton('attachments', this.attachments);
    return entry;
  }

  async bulkTag(kind, ids, tag, add = true) {
    const clean = cleanTag(tag);
    if (!clean) return 0;
    const k = tagKey(clean);
    const set = new Set(ids || []);
    const list = kind === 'contacts' ? this.contacts : kind === 'companies' ? this.companies : this.deals;
    let changed = 0;
    for (const rec of list) {
      if (!set.has(rec.id)) continue;
      if (add) {
        rec.tags = normalizeTags([...(rec.tags || []), clean]);
      } else {
        rec.tags = (rec.tags || []).filter((t) => tagKey(t) !== k);
      }
      changed++;
    }
    if (changed > 0) {
      this.#emit({ type: kind });
      await this.storage.saveCollection(kind, list);
    }
    return changed;
  }

  async renameTag(oldTag, newTag) {
    const oldKey = tagKey(oldTag);
    const next = cleanTag(newTag);
    if (!oldKey || !next) return 0;
    let total = 0;
    const saves = [];
    for (const [kind, list] of [['companies', this.companies], ['contacts', this.contacts], ['deals', this.deals]]) {
      let dirty = false;
      for (const rec of list) {
        if ((rec.tags || []).some((t) => tagKey(t) === oldKey)) {
          rec.tags = normalizeTags(rec.tags.map((t) => (tagKey(t) === oldKey ? next : t)));
          dirty = true;
          total++;
        }
      }
      if (dirty) saves.push(this.storage.saveCollection(kind, list));
    }
    if (total > 0) {
      this.#emit({ type: 'tags' });
      await Promise.all(saves);
    }
    return total;
  }

  async deleteTag(targetTag) {
    const k = tagKey(targetTag);
    if (!k) return 0;
    let total = 0;
    const saves = [];
    for (const [kind, list] of [['companies', this.companies], ['contacts', this.contacts], ['deals', this.deals]]) {
      let dirty = false;
      for (const rec of list) {
        const before = (rec.tags || []).length;
        rec.tags = (rec.tags || []).filter((t) => tagKey(t) !== k);
        if (rec.tags.length !== before) {
          dirty = true;
          total++;
        }
      }
      if (dirty) saves.push(this.storage.saveCollection(kind, list));
    }
    if (total > 0) {
      this.#emit({ type: 'tags' });
      await Promise.all(saves);
    }
    return total;
  }

  async mergeContacts(survivorId, mergedId, patch = {}) {
    return this.#serialized(async () => {
      const survivor = this.contact(survivorId);
      const merged = this.contact(mergedId);
      if (!survivor || !merged || survivorId === mergedId) return null;

      Object.assign(
        survivor,
        this.normalizeContact({
          ...survivor,
          ...patch,
          id: survivor.id,
          companyId: patch.companyId !== undefined ? patch.companyId : survivor.companyId || merged.companyId || null,
          tags: normalizeTags([...(survivor.tags || []), ...(merged.tags || [])]),
          custom: mergeCustomValues(survivor.custom, merged.custom),
          createdAt: Math.min(survivor.createdAt || Date.now(), merged.createdAt || Date.now()),
        })
      );

      const refs = { deals: [], activities: [], tasks: [] };
      for (const d of this.deals) {
        if (dealContactIds(d).includes(mergedId)) {
          refs.deals.push(d.id);
          d.contacts = repointContact(d.contacts, mergedId, survivorId);
          d.contactId = primaryContactId(d);
        }
      }
      for (const a of this.activities) {
        if (a.contactId === mergedId) {
          a.contactId = survivorId;
          refs.activities.push(a.id);
        }
      }
      for (const t of this.tasks) {
        if (t.contactId === mergedId) {
          t.contactId = survivorId;
          refs.tasks.push(t.id);
        }
      }
      let attChanged = false;
      for (const att of this.attachments) {
        if (att.contactId === mergedId) {
          att.contactId = survivorId;
          attChanged = true;
        }
      }

      this.contacts = this.contacts.filter((c) => c.id !== mergedId);
      await this.#toTrash('contact', merged, refs);
      this.#emit({ type: 'merge' });
      await Promise.all([
        this.storage.saveCollection('contacts', this.contacts),
        refs.deals.length ? this.storage.saveCollection('deals', this.deals) : Promise.resolve(),
        refs.activities.length ? this.storage.saveCollection('activities', this.activities) : Promise.resolve(),
        refs.tasks.length ? this.storage.saveCollection('tasks', this.tasks) : Promise.resolve(),
        attChanged ? this.storage.saveSingleton('attachments', this.attachments) : Promise.resolve(),
      ]);
      return survivor;
    });
  }

  async mergeCompanies(survivorId, mergedId, patch = {}) {
    return this.#serialized(async () => {
      const survivor = this.company(survivorId);
      const merged = this.company(mergedId);
      if (!survivor || !merged || survivorId === mergedId) return null;

      Object.assign(
        survivor,
        this.normalizeCompany({
          ...survivor,
          ...patch,
          id: survivor.id,
          tags: normalizeTags([...(survivor.tags || []), ...(merged.tags || [])]),
          custom: mergeCustomValues(survivor.custom, merged.custom),
          createdAt: Math.min(survivor.createdAt || Date.now(), merged.createdAt || Date.now()),
        })
      );

      const refs = { contacts: [], deals: [], activities: [] };
      for (const c of this.contacts) {
        if (c.companyId === mergedId) {
          c.companyId = survivorId;
          refs.contacts.push(c.id);
        }
      }
      for (const d of this.deals) {
        if (d.companyId === mergedId) {
          d.companyId = survivorId;
          refs.deals.push(d.id);
        }
      }
      for (const a of this.activities) {
        if (a.companyId === mergedId) {
          a.companyId = survivorId;
          refs.activities.push(a.id);
        }
      }
      let attChanged = false;
      for (const att of this.attachments) {
        if (att.companyId === mergedId) {
          att.companyId = survivorId;
          attChanged = true;
        }
      }

      this.companies = this.companies.filter((c) => c.id !== mergedId);
      await this.#toTrash('company', merged, refs);
      this.#emit({ type: 'merge' });
      await Promise.all([
        this.storage.saveCollection('companies', this.companies),
        refs.contacts.length ? this.storage.saveCollection('contacts', this.contacts) : Promise.resolve(),
        refs.deals.length ? this.storage.saveCollection('deals', this.deals) : Promise.resolve(),
        refs.activities.length ? this.storage.saveCollection('activities', this.activities) : Promise.resolve(),
        attChanged ? this.storage.saveSingleton('attachments', this.attachments) : Promise.resolve(),
      ]);
      return survivor;
    });
  }

  async #toTrash(kind, record, refs = {}, persist = true) {
    const name = record.name || record.title || record.summary || 'Untitled';
    const entry = {
      id: uid(),
      kind,
      at: Date.now(),
      name,
      record: structuredClone(record),
      refs,
    };
    this.trash.unshift(entry);
    const dropped = this.#pruneTrash();
    if (persist) {
      await this.storage.saveCollection('trash', this.trash);
      await this.#reclaimFiles(dropped);
    }
    return entry;
  }

  #pruneTrash(now = Date.now()) {
    const cutoff = now - TRASH_TTL;
    const kept = [];
    const dropped = [];
    for (const e of this.trash) {
      if (e.at < cutoff || kept.length >= TRASH_MAX) dropped.push(e);
      else kept.push(e);
    }
    this.trash = kept;
    return dropped;
  }

  #attachmentOwnerExists(att, trashOwnerIds) {
    if (att.contactId) return Boolean(this.contact(att.contactId)) || trashOwnerIds.has(att.contactId);
    if (att.companyId) return Boolean(this.company(att.companyId)) || trashOwnerIds.has(att.companyId);
    if (att.dealId) return Boolean(this.deal(att.dealId)) || trashOwnerIds.has(att.dealId);
    return false;
  }

  #purgeOrphanAttachments() {
    const trashOwnerIds = new Set(
      this.trash
        .filter((e) => e.kind === 'contact' || e.kind === 'company' || e.kind === 'deal')
        .map((e) => e.record?.id)
        .filter(Boolean)
    );
    const live = [];
    const purged = [];
    for (const att of this.attachments) {
      if (this.#attachmentOwnerExists(att, trashOwnerIds)) live.push(att);
      else purged.push({ kind: 'attachment', record: att });
    }
    this.attachments = live;
    return purged;
  }

  async #reclaimFiles(entries) {
    if (!entries || entries.length === 0) return;
    const snapshot = await readSafetySnapshot().catch(() => null);
    const protectedPaths = new Set();
    for (const a of this.attachments) if (a.path) protectedPaths.add(a.path);
    for (const e of this.trash) if (e.kind === 'attachment' && e.record?.path) protectedPaths.add(e.record.path);
    for (const a of snapshot?.data?.attachments || []) if (a.path) protectedPaths.add(a.path);

    for (const e of entries) {
      const p = e.kind === 'attachment' ? e.record?.path : null;
      if (p && !protectedPaths.has(p)) {
        await deleteStoredFile(p);
      }
    }
  }

  async restoreTrash(entryId) {
    return this.#serialized(async () => {
      const idx = this.trash.findIndex((e) => e.id === entryId);
      if (idx === -1) return null;
      const [entry] = this.trash.splice(idx, 1);
      const { kind, record, refs } = entry;
      const saves = [this.storage.saveCollection('trash', this.trash)];

      if (kind === 'company') {
        const co = this.normalizeCompany(record);
        this.companies.push(co);
        saves.push(this.storage.saveCollection('companies', this.companies));
        for (const id of refs?.contacts || []) {
          const c = this.contact(id);
          if (c && !c.companyId) c.companyId = co.id;
        }
        for (const id of refs?.deals || []) {
          const d = this.deal(id);
          if (d && !d.companyId) d.companyId = co.id;
        }
        for (const id of refs?.activities || []) {
          const a = this.activity(id);
          if (a && !a.companyId) a.companyId = co.id;
        }
        if (refs?.contacts?.length) saves.push(this.storage.saveCollection('contacts', this.contacts));
        if (refs?.deals?.length) saves.push(this.storage.saveCollection('deals', this.deals));
        if (refs?.activities?.length) saves.push(this.storage.saveCollection('activities', this.activities));
      } else if (kind === 'contact') {
        const ct = this.normalizeContact(record);
        this.contacts.push(ct);
        saves.push(this.storage.saveCollection('contacts', this.contacts));
        for (const id of refs?.deals || []) {
          const d = this.deal(id);
          if (d && !dealContactIds(d).includes(ct.id)) {
            d.contacts = normalizeDealContacts([...(d.contacts || []), { contactId: ct.id, role: '' }]);
            if (!d.contactId) d.contactId = ct.id;
          }
        }
        for (const id of refs?.activities || []) {
          const a = this.activity(id);
          if (a && !a.contactId) a.contactId = ct.id;
        }
        for (const id of refs?.tasks || []) {
          const t = this.task(id);
          if (t && !t.contactId) t.contactId = ct.id;
        }
        if (refs?.deals?.length) saves.push(this.storage.saveCollection('deals', this.deals));
        if (refs?.activities?.length) saves.push(this.storage.saveCollection('activities', this.activities));
        if (refs?.tasks?.length) saves.push(this.storage.saveCollection('tasks', this.tasks));
      } else if (kind === 'deal') {
        const dl = this.normalizeDeal(record, this.resolvedStages());
        this.deals.push(dl);
        saves.push(this.storage.saveCollection('deals', this.deals));
        for (const id of refs?.activities || []) {
          const a = this.activity(id);
          if (a && !a.dealId) a.dealId = dl.id;
        }
        for (const id of refs?.tasks || []) {
          const t = this.task(id);
          if (t && !t.dealId) t.dealId = dl.id;
        }
        if (refs?.activities?.length) saves.push(this.storage.saveCollection('activities', this.activities));
        if (refs?.tasks?.length) saves.push(this.storage.saveCollection('tasks', this.tasks));
      } else if (kind === 'activity') {
        this.activities.push(this.normalizeActivity(record));
        saves.push(this.storage.saveCollection('activities', this.activities));
      } else if (kind === 'task') {
        this.tasks.push(this.normalizeTask(record));
        saves.push(this.storage.saveCollection('tasks', this.tasks));
      } else if (kind === 'template') {
        this.templates.push(this.normalizeTemplate(record));
        saves.push(this.storage.saveSingleton('templates', this.templates));
      } else if (kind === 'attachment') {
        const att = this.normalizeAttachment(record);
        if (att.path) this.attachments.push(att);
        saves.push(this.storage.saveSingleton('attachments', this.attachments));
      }

      this.#emit({ type: 'restore' });
      await Promise.all(saves);
      return entry;
    });
  }

  async emptyTrash() {
    return this.#serialized(async () => {
      const oldTrash = this.trash;
      this.trash = [];
      const orphans = this.#purgeOrphanAttachments();
      this.#emit({ type: 'trash' });
      await Promise.all([
        this.storage.saveCollection('trash', this.trash),
        this.storage.saveSingleton('attachments', this.attachments),
      ]);
      await this.#reclaimFiles([...oldTrash, ...orphans]);
    });
  }

  exportAll() {
    return {
      app: 'crm',
      version: 1,
      exportedAt: Date.now(),
      settings: structuredClone(this.settings),
      companies: structuredClone(this.companies),
      contacts: structuredClone(this.contacts),
      deals: structuredClone(this.deals),
      activities: structuredClone(this.activities),
      tasks: structuredClone(this.tasks),
      templates: structuredClone(this.templates),
      attachments: structuredClone(this.attachments),
    };
  }

  async saveSnapshot(reason = 'manual') {
    const snap = { at: Date.now(), reason, data: this.exportAll() };
    const meta = await writeSafetySnapshot(snap);
    if (!meta) return null;
    this.backupMeta = { at: snap.at, reason };
    await this.storage.saveSingleton('backupMeta', this.backupMeta);
    return this.backupMeta;
  }

  async restoreBackup() {
    const snap = await readSafetySnapshot();
    if (!snap?.data) return false;
    await this.importAll(snap.data, { reason: 'restore' });
    return true;
  }

  async importAll(payload, { reason = 'import' } = {}) {
    return this.#serialized(async () => {
      if (!payload || payload.app !== 'crm') throw new Error('Invalid CRM export payload');
      if (this.hasData()) {
        await this.saveSnapshot(reason);
      }

      this.settings = this.normalizeSettings(payload.settings || {});
      const stages = resolveStages(this.settings);
      this.companies = (payload.companies || []).map((c) => this.normalizeCompany(c));
      this.contacts = (payload.contacts || []).map((c) => this.normalizeContact(c));
      this.deals = (payload.deals || []).map((d) => this.normalizeDeal(d, stages));
      this.activities = (payload.activities || []).map((a) => this.normalizeActivity(a));
      this.tasks = (payload.tasks || []).map((t) => this.normalizeTask(t));
      this.templates = (payload.templates || []).map((t) => this.normalizeTemplate(t));
      this.attachments = (payload.attachments || []).map((a) => this.normalizeAttachment(a)).filter((a) => a.path);

      this.#emit({ type: 'importAll' });
      await Promise.all([
        this.storage.saveSingleton('settings', this.settings),
        this.storage.saveSingleton('templates', this.templates),
        this.storage.saveSingleton('attachments', this.attachments),
        this.storage.saveCollection('companies', this.companies),
        this.storage.saveCollection('contacts', this.contacts),
        this.storage.saveCollection('deals', this.deals),
        this.storage.saveCollection('activities', this.activities),
        this.storage.saveCollection('tasks', this.tasks),
      ]);
    });
  }

  async eraseAll() {
    await this.importAll(
      {
        app: 'crm',
        version: 1,
        settings: { ...DEFAULT_SETTINGS, onboarded: true },
        companies: [],
        contacts: [],
        deals: [],
        activities: [],
        tasks: [],
        templates: [],
        attachments: [],
      },
      { reason: 'erase' }
    );
    await this.emptyTrash();
  }

  async createFileBackup() {
    return writeBackup(this.exportAll());
  }

  async restoreFileBackup(path) {
    const payload = await readBackup(path);
    await this.importAll(payload, { reason: 'file-backup' });
  }

  async importRecords(kind, rows, { update = true } = {}) {
    return this.#serialized(async () => {
      const stats = { added: 0, updated: 0, companiesCreated: 0 };
      const companyByName = new Map(this.companies.map((c) => [c.name.trim().toLowerCase(), c]));
      const ensureCompany = (name) => {
        const clean = String(name || '').trim();
        if (!clean) return null;
        const k = clean.toLowerCase();
        let co = companyByName.get(k);
        if (!co) {
          co = this.normalizeCompany({ id: uid(), name: clean, createdAt: Date.now() });
          this.companies.push(co);
          companyByName.set(k, co);
          stats.companiesCreated++;
        }
        return co.id;
      };

      const assignFilled = (target, patch) => {
        for (const [k, v] of Object.entries(patch)) {
          if (k === 'tags') {
            if (v?.length) target.tags = normalizeTags([...(target.tags || []), ...v]);
          } else if (k === 'custom') {
            if (v && Object.keys(v).length) target.custom = { ...(target.custom || {}), ...v };
          } else if (v !== null && v !== undefined && v !== '') {
            target[k] = v;
          }
        }
      };

      if (kind === 'companies') {
        for (const r of rows) {
          const existing = update ? companyByName.get(r.name.trim().toLowerCase()) : null;
          if (existing) {
            assignFilled(existing, r);
            stats.updated++;
          } else {
            const co = this.normalizeCompany({ ...r, id: uid(), createdAt: Date.now() });
            this.companies.push(co);
            companyByName.set(co.name.trim().toLowerCase(), co);
            stats.added++;
          }
        }
      } else if (kind === 'contacts') {
        const byEmail = new Map(this.contacts.filter((c) => c.email).map((c) => [c.email.trim().toLowerCase(), c]));
        for (const r of rows) {
          const copy = { ...r };
          if (copy.companyName) {
            copy.companyId = ensureCompany(copy.companyName);
            delete copy.companyName;
          }
          const emailKey = copy.email ? copy.email.trim().toLowerCase() : '';
          const existing = update && emailKey ? byEmail.get(emailKey) : null;
          if (existing) {
            assignFilled(existing, copy);
            stats.updated++;
          } else {
            const ct = this.normalizeContact({ ...copy, id: uid(), createdAt: Date.now() });
            this.contacts.push(ct);
            if (ct.email) byEmail.set(ct.email.trim().toLowerCase(), ct);
            stats.added++;
          }
        }
      } else if (kind === 'deals') {
        const byName = new Map(this.deals.map((d) => [d.name.trim().toLowerCase(), d]));
        const contactCounts = new Map();
        const contactByLower = new Map();
        for (const c of this.contacts) {
          const k = c.name.trim().toLowerCase();
          contactCounts.set(k, (contactCounts.get(k) || 0) + 1);
          contactByLower.set(k, c);
        }
        const stages = this.resolvedStages();
        const now = Date.now();

        for (const r of rows) {
          const copy = { ...r };
          if (copy.companyName) {
            copy.companyId = ensureCompany(copy.companyName);
            delete copy.companyName;
          }
          if (copy.contactName) {
            const k = copy.contactName.trim().toLowerCase();
            if (contactCounts.get(k) === 1) copy.contactId = contactByLower.get(k).id;
            delete copy.contactName;
          }

          const existing = update ? byName.get(copy.name.trim().toLowerCase()) : null;
          if (existing) {
            const prevStage = existing.stage;
            assignFilled(existing, copy);
            if (copy.contactId) {
              existing.contacts = replacePrimary(existing.contacts, copy.contactId);
              existing.contactId = primaryContactId(existing);
            }
            if (existing.stage !== prevStage) {
              this.#recordStageEntry(existing, existing.stage, now);
              if (isTerminalStage(existing.stage)) {
                existing.closedAt = now;
                existing.confidence = null;
              } else {
                existing.closedAt = null;
                existing.closeReason = '';
                existing.closeReasonNote = '';
              }
            }
            existing.updatedAt = now;
            stats.updated++;
          } else {
            const dl = this.normalizeDeal({ ...copy, id: uid(), createdAt: now, updatedAt: now }, stages);
            if (isTerminalStage(dl.stage)) dl.closedAt = now;
            this.deals.push(dl);
            byName.set(dl.name.trim().toLowerCase(), dl);
            stats.added++;
          }
        }
      }

      this.#emit({ type: 'importRecords' });
      await Promise.all([
        kind === 'companies' || stats.companiesCreated > 0 ? this.storage.saveCollection('companies', this.companies) : Promise.resolve(),
        kind === 'contacts' ? this.storage.saveCollection('contacts', this.contacts) : Promise.resolve(),
        kind === 'deals' ? this.storage.saveCollection('deals', this.deals) : Promise.resolve(),
      ]);
      return stats;
    });
  }

  duplicates() {
    return {
      contacts: contactDuplicates(this.contacts),
      companies: companyDuplicates(this.companies),
    };
  }
}

export const store = new SalesCRMStore();
