/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { touchProgress } from './activity.js';
import { goingCold, noDecisionMaker, noNextStep } from './attention.js';
import { listBackups } from './backups.js';
import {
  avgClosedValue,
  avgSalesCycle,
  closeReasonBreakdown,
  daysInStage,
  forecastByMonth,
  goalProgress,
  isOpen,
  LOST_REASONS,
  pipelineCoverage,
  pipelineValue,
  stalledDeals,
  stageSummary,
  stageVelocity,
  weightedPipeline,
  winRate,
  winRateBy,
  WON_REASONS,
  wonInMonth,
} from './forecast.js';
import { funnelReport, matchesFunnelWindow } from './funnel.js';
import { getLanguage } from './i18n.js';
import { buildCalendar, dealEvent, taskEvent } from './ics.js';
import { contractValue, pipelineMRR, recurringSummary } from './recurring.js';
import { monthlyFlows, salesCycleTrend, winRateTrend } from './trends.js';
import { fuzzyIncludes, isValidISODate, matchScore, monthKey, todayISO } from './util.js';

function isFullTrust(store) {
  const live = store.systemInfo?.mcpAccess || window.ladybirdApp?.info?.mcpAccess;
  return live === 'full';
}

function kSuppress(rows, k = 3) {
  const kept = [];
  let other = { key: 'Other', won: 0, lost: 0, total: 0, value: 0 };
  for (const r of rows) {
    if (r.key !== 'Other' && r.total >= k) {
      kept.push(r);
    } else {
      other.won += r.won;
      other.lost += r.lost;
      other.total += r.total;
      other.value += r.value;
    }
  }
  if (other.total >= k) {
    other.rate = other.won / other.total;
    kept.push(other);
  }
  return kept;
}

function sanitizeReasonBreakdown(breakdown, allowedReasons, suppressSmall = false, k = 3) {
  const allow = new Set(allowedReasons);
  const byReason = new Map();
  for (const r of breakdown?.rows || []) {
    const key = allow.has(r.reason) ? r.reason : 'Other';
    const cur = byReason.get(key) || { reason: key, count: 0, value: 0 };
    cur.count += r.count;
    cur.value += r.value;
    byReason.set(key, cur);
  }
  const merged = [...byReason.values()];
  if (!suppressSmall) {
    return {
      rows: merged.sort((a, b) => b.count - a.count || b.value - a.value),
      unrecorded: breakdown?.unrecorded || 0,
      total: breakdown?.total || 0,
    };
  }
  const kept = [];
  const other = { reason: 'Other', count: 0, value: 0 };
  for (const r of merged) {
    if (r.reason !== 'Other' && r.count >= k) kept.push(r);
    else {
      other.count += r.count;
      other.value += r.value;
    }
  }
  if (other.count >= k) kept.push(other);
  return {
    rows: kept.sort((a, b) => b.count - a.count || b.value - a.value),
    unrecorded: breakdown?.unrecorded || 0,
    total: breakdown?.total || 0,
  };
}

export function registerSalesCRMWebMCP(store) {
  let registered = false;

  const install = () => {
    if (registered) return;
    const ctx = document.modelContext || navigator.modelContext;
    if (!ctx || typeof ctx.registerTool !== 'function') return;
    registered = true;

    const wrap = (name, tier, fn) => async (args = {}) => {
      try {
        const res = await fn(args || {});
        store.recordWebMCP({ tool: name, tier, ok: true });
        return res;
      } catch (err) {
        store.recordWebMCP({ tool: name, tier, ok: false, note: err?.message || 'error' });
        throw err;
      }
    };

    const mcpMode = isFullTrust(store) ? 'full' : 'webmcp-only';

    // -------------------------------------------------------------------------
    // Tier A: Privacy-Safe Read & Analytics Tools (Zero Raw PII / Free Text)
    // -------------------------------------------------------------------------

    ctx.registerTool({
      name: 'get_crm_status',
      description: 'Return CRM runtime metadata, account slug, mcpAccess mode, and aggregate record counts (zero PII).',
      inputSchema: { type: 'object', properties: {}, additionalProperties: false },
      execute: wrap('get_crm_status', 'A', async () => {
        const sys = window.ladybirdApp ? await window.ladybirdApp.system.info().catch(() => store.systemInfo) : store.systemInfo;
        const today = todayISO();
        const open = store.openTasks();
        return {
          account: store.currentAccount,
          mcpAccess: sys?.mcpAccess || mcpMode,
          currency: store.settings.currency,
          language: getLanguage(),
          languagePreference: store.settings.language,
          counts: {
            companies: store.companies.length,
            contacts: store.contacts.length,
            deals: store.deals.length,
            openDeals: store.deals.filter(isOpen).length,
            wonDeals: store.deals.filter((d) => d.stage === 'won').length,
            lostDeals: store.deals.filter((d) => d.stage === 'lost').length,
            activities: store.activities.length,
            tasks: store.tasks.length,
            openTasks: open.length,
            overdueTasks: open.filter((t) => t.due < today).length,
            dueTodayTasks: open.filter((t) => t.due === today).length,
            templates: store.templates.length,
            attachments: store.attachments.length,
            trash: store.trash.length,
          },
        };
      }),
    });

    ctx.registerTool({
      name: 'get_pipeline_summary',
      description: 'Return open/weighted pipeline totals, recurring MRR/ARR, monthly goal pace, and per-stage aggregates.',
      inputSchema: { type: 'object', properties: {}, additionalProperties: false },
      execute: wrap('get_pipeline_summary', 'A', async () => {
        const stages = store.resolvedStages();
        const ov = store.settings.stageProbabilities;
        const key = monthKey();
        const openVal = pipelineValue(store.deals);
        const weightedVal = weightedPipeline(store.deals, ov, stages);
        const goal = goalProgress(store.deals, store.settings.revenueGoal, key);
        const velocity = new Map(stageVelocity(store.deals, stages).map((v) => [v.stage, v]));
        const summary = stageSummary(store.deals, ov, stages).map((s) => ({
          ...s,
          avgDays: velocity.get(s.stage)?.days ?? null,
        }));
        return {
          currency: store.settings.currency,
          openPipeline: openVal,
          weightedPipeline: weightedVal,
          pipelineMRR: pipelineMRR(store.deals),
          recurringWon: recurringSummary(store.deals, key),
          wonInMonth: wonInMonth(store.deals, key),
          goalProgress: goal,
          pipelineCoverage: pipelineCoverage(weightedVal, goal.remaining),
          stages: summary,
        };
      }),
    });

    ctx.registerTool({
      name: 'get_revenue_forecast',
      description: 'Return multi-month revenue forecast, win rate, closed values, close reasons (aggregated, no notes), and k-anonymized source/industry breakdown.',
      inputSchema: {
        type: 'object',
        properties: { months: { type: 'integer', minimum: 1, maximum: 24 } },
        additionalProperties: false,
      },
      execute: wrap('get_revenue_forecast', 'A', async ({ months = 6 }) => {
        const stages = store.resolvedStages();
        const ov = store.settings.stageProbabilities;
        const suppressSmall = mcpMode === 'webmcp-only';
        return {
          currency: store.settings.currency,
          monthly: forecastByMonth(store.deals, { months, overrides: ov, stages }),
          winRate: winRate(store.deals),
          avgClosedValue: avgClosedValue(store.deals),
          avgSalesCycleDays: avgSalesCycle(store.deals),
          wonReasons: sanitizeReasonBreakdown(closeReasonBreakdown(store.deals, 'won'), WON_REASONS, suppressSmall),
          lostReasons: sanitizeReasonBreakdown(closeReasonBreakdown(store.deals, 'lost'), LOST_REASONS, suppressSmall),
          byLeadSource: kSuppress(winRateBy(store.deals, (d) => store.contact(d.contactId)?.source || '')),
          byIndustry: kSuppress(winRateBy(store.deals, (d) => store.company(d.companyId)?.industry || '')),
        };
      }),
    });

    ctx.registerTool({
      name: 'get_conversion_funnel',
      description: 'Return cohort stage-to-stage conversion funnel metrics (excludes untracked legacy deals).',
      inputSchema: {
        type: 'object',
        properties: { window: { type: 'string', enum: ['all', 'quarter', 'last-quarter', 'year'] } },
        additionalProperties: false,
      },
      execute: wrap('get_conversion_funnel', 'A', async ({ window: win = 'all' }) => {
        const subset = store.deals.filter((d) => matchesFunnelWindow(d, win));
        return { window: win, ...funnelReport(subset, store.resolvedStages()) };
      }),
    });

    ctx.registerTool({
      name: 'get_pipeline_trends',
      description: 'Return 12-month pipeline flows (added vs won vs lost), win-rate trend, and sales cycle trend.',
      inputSchema: {
        type: 'object',
        properties: { months: { type: 'integer', minimum: 3, maximum: 24 } },
        additionalProperties: false,
      },
      execute: wrap('get_pipeline_trends', 'A', async ({ months = 12 }) => {
        const flows = monthlyFlows(store.deals, { months });
        return {
          currency: store.settings.currency,
          flows,
          winRateTrend: winRateTrend(flows),
          salesCycleTrend: salesCycleTrend(store.deals, { months }),
        };
      }),
    });

    ctx.registerTool({
      name: 'get_attention_summary',
      description: 'Return anonymized counts of stalled deals, going-cold leads, deals missing next step/decider, weekly touch pace, and opaque stalled deal metrics (zero PII).',
      inputSchema: { type: 'object', properties: {}, additionalProperties: false },
      execute: wrap('get_attention_summary', 'A', async () => {
        const today = todayISO();
        const coldDays = store.settings.coldDays;
        const staleDays = store.settings.staleDays;
        const cold = goingCold(store.contacts, (id) => store.lastContactedAt(id), { coldDays, today });
        const noStep = noNextStep(store.deals, {
          lastTouchOf: (d) => store.lastTouchedDeal(d),
          hasOpenTask: (id) => store.hasOpenTask(id),
          coldDays,
          today,
        });
        const noDecider = noDecisionMaker(store.deals);
        const stalled = stalledDeals(store.deals, staleDays);
        const open = store.openTasks();
        return {
          thresholds: { staleDays, coldDays },
          counts: {
            goingColdContacts: cold.length,
            noNextStepDeals: noStep.length,
            noDecisionMakerDeals: noDecider.length,
            stalledDeals: stalled.length,
            overdueTasks: open.filter((t) => t.due < today).length,
            dueTodayTasks: open.filter((t) => t.due === today).length,
          },
          touchProgress: touchProgress(store.activities, store.settings.touchTargets, today),
          stalledDealsAnonymized: stalled.slice(0, 25).map((d) => ({
            dealId: d.id,
            stage: d.stage,
            contractValue: contractValue(d),
            daysInStage: daysInStage(d),
            hasOpenTask: store.hasOpenTask(d.id),
          })),
        };
      }),
    });

    ctx.registerTool({
      name: 'list_playbooks_and_views',
      description: 'Return configured stages, playbook cadence metadata (offsets and triggers, no template text), saved view counts, and backup file metadata.',
      inputSchema: { type: 'object', properties: {}, additionalProperties: false },
      execute: wrap('list_playbooks_and_views', 'A', async () => {
        const backups = await listBackups().catch(() => []);
        const restricted = mcpMode === 'webmcp-only';
        return {
          stages: store.resolvedStages().map((s) => (restricted ? { id: s.id, probability: s.probability } : s)),
          playbooks: (store.settings.playbooks || []).map((p) =>
            restricted
              ? {
                  id: p.id,
                  trigger: p.trigger,
                  stepCount: p.steps.length,
                  offsets: p.steps.map((s) => s.offset),
                }
              : {
                  id: p.id,
                  name: p.name,
                  trigger: p.trigger,
                  stepCount: p.steps.length,
                  offsets: p.steps.map((s) => s.offset),
                }
          ),
          savedViewCounts: {
            contacts: store.settings.views?.contacts?.length || 0,
            companies: store.settings.views?.companies?.length || 0,
            deals: store.settings.views?.deals?.length || 0,
          },
          backups: backups.map((b) => (restricted ? { at: b.at, size: b.size } : { name: b.name, path: b.path, at: b.at, size: b.size })),
        };
      }),
    });

    // -------------------------------------------------------------------------
    // Tier B: Privacy-Safe Structured Actions (Opaque IDs, Zero PII Echo)
    // -------------------------------------------------------------------------

    ctx.registerTool({
      name: 'move_deal_stage',
      description: 'Transition a deal by opaque dealId to targetStage (honors 30s stage-flip undo window and updates weighted totals).',
      inputSchema: {
        type: 'object',
        properties: {
          dealId: { type: 'string' },
          toStage: { type: 'string' },
          confidence: { type: 'number', minimum: 0, maximum: 1 },
        },
        required: ['dealId', 'toStage'],
        additionalProperties: false,
      },
      execute: wrap('move_deal_stage', 'B', async ({ dealId, toStage, confidence }) => {
        const before = store.deal(dealId);
        if (!before) throw new Error('Deal not found');
        if (!store.resolvedStages().some((s) => s.id === toStage)) {
          throw new Error(`Invalid stage: ${toStage}`);
        }
        const fromStage = before.stage;
        const patch = { stage: toStage };
        if (confidence !== undefined) patch.confidence = confidence;
        const updated = await store.updateDeal(dealId, patch);
        return {
          dealId: updated.id,
          fromStage,
          toStage: updated.stage,
          closedAt: updated.closedAt,
          stageHistoryLength: updated.stageHistory.length,
          weightedPipeline: weightedPipeline(store.deals, store.settings.stageProbabilities, store.resolvedStages()),
        };
      }),
    });

    ctx.registerTool({
      name: 'apply_playbook',
      description: 'Instantiate all steps of a configured playbook as follow-up tasks linked to an opaque dealId or contactId.',
      inputSchema: {
        type: 'object',
        properties: {
          playbookId: { type: 'string' },
          dealId: { type: 'string' },
          contactId: { type: 'string' },
          fromDate: { type: 'string' },
        },
        required: ['playbookId'],
        additionalProperties: false,
      },
      execute: wrap('apply_playbook', 'B', async ({ playbookId, dealId = null, contactId = null, fromDate }) => {
        const pb = (store.settings.playbooks || []).find((p) => p.id === playbookId);
        if (!pb) throw new Error('Playbook not found');
        if (dealId && !store.deal(dealId)) throw new Error('Deal not found');
        if (contactId && !store.contact(contactId)) throw new Error('Contact not found');
        if (fromDate !== undefined && fromDate !== null && fromDate !== '' && !isValidISODate(fromDate)) {
          throw new Error('Invalid ISO date (expected YYYY-MM-DD)');
        }
        const created = await store.applyPlaybook(pb, { dealId, contactId, from: fromDate || todayISO() });
        return {
          playbookId: pb.id,
          tasksCreated: created.length,
          taskIds: created.map((t) => t.id),
          dueDates: created.map((t) => t.due),
        };
      }),
    });

    ctx.registerTool({
      name: 'complete_followup',
      description: 'Mark a follow-up task done/undone by opaque taskId (auto-spawns next occurrence when task has repeat cadence).',
      inputSchema: {
        type: 'object',
        properties: {
          taskId: { type: 'string' },
          done: { type: 'boolean' },
        },
        required: ['taskId'],
        additionalProperties: false,
      },
      execute: wrap('complete_followup', 'B', async ({ taskId, done = true }) => {
        const res = await store.toggleTask(taskId, done);
        if (!res) throw new Error('Task not found');
        return {
          taskId: res.task.id,
          done: res.task.done,
          spawnedTaskId: res.spawned ? res.spawned.id : null,
          spawnedDue: res.spawned ? res.spawned.due : null,
          remainingOpenTasks: store.openTasks().length,
        };
      }),
    });

    ctx.registerTool({
      name: 'log_structured_touch',
      description: 'Record an activity touchpoint using strictly typed enums and opaque IDs (no free-text returned or required).',
      inputSchema: {
        type: 'object',
        properties: {
          type: { type: 'string', enum: ['meeting', 'call', 'email', 'note'] },
          dealId: { type: 'string' },
          contactId: { type: 'string' },
          date: { type: 'string' },
        },
        required: ['type'],
        additionalProperties: false,
      },
      execute: wrap('log_structured_touch', 'B', async ({ type, dealId = null, contactId = null, date }) => {
        if (dealId && !store.deal(dealId)) throw new Error('Deal not found');
        if (contactId && !store.contact(contactId)) throw new Error('Contact not found');
        if (date !== undefined && date !== null && date !== '' && !isValidISODate(date)) {
          throw new Error('Invalid ISO date (expected YYYY-MM-DD)');
        }
        const act = await store.addActivity({
          type,
          dealId,
          contactId,
          date: date || todayISO(),
          summary: `Logged ${type}`,
        });
        return { activityId: act.id, type: act.type, date: act.date, dealId: act.dealId, contactId: act.contactId };
      }),
    });

    ctx.registerTool({
      name: 'create_backup',
      description: 'Write a full binary JSON backup under backups/ via /__ladybird_fs (supports >4 MiB up to 128 MiB).',
      inputSchema: { type: 'object', properties: {}, additionalProperties: false },
      execute: wrap('create_backup', 'B', async () => {
        const res = await store.createFileBackup();
        return {
          path: res.path,
          bytesWritten: res.bytesWritten,
          counts: {
            companies: store.companies.length,
            contacts: store.contacts.length,
            deals: store.deals.length,
            activities: store.activities.length,
            tasks: store.tasks.length,
          },
        };
      }),
    });

    ctx.registerTool({
      name: 'export_ics_summary',
      description: 'Generate an RFC 5545 .ics calendar for open follow-ups and expected deal closes into exports/ via /__ladybird_fs.',
      inputSchema: {
        type: 'object',
        properties: { filename: { type: 'string' } },
        additionalProperties: false,
      },
      execute: wrap('export_ics_summary', 'B', async ({ filename = 'crm-followups.ics' }) => {
        const fs = window.ladybirdApp?.fs;
        if (!fs) throw new Error('Ladybird fs bridge unavailable');
        const restricted = mcpMode === 'webmcp-only';
        const events = [
          ...store.openTasks().map((t) =>
            restricted
              ? taskEvent({ ...t, title: `Follow-up #${String(t.id).slice(0, 8)}` }, { contactName: '', dealName: '' })
              : taskEvent(t, {
                  contactName: store.contact(t.contactId)?.name || '',
                  dealName: store.deal(t.dealId)?.name || '',
                })
          ),
          ...store.deals
            .filter((d) => isOpen(d) && d.expectedClose)
            .map((d) =>
              restricted
                ? dealEvent({ ...d, name: `Deal close #${String(d.id).slice(0, 8)}` }, { companyName: '' })
                : dealEvent(d, { companyName: store.company(d.companyId)?.name || '' })
            ),
        ];
        const ics = buildCalendar(events);
        let clean = String(filename || '')
          .replace(/^.*[\\/]/, '')
          .replace(/[^a-zA-Z0-9._-]/g, '_')
          .replace(/^\.+/, '')
          .slice(0, 80);
        if (!clean || clean === '.' || clean === '..') clean = 'crm-followups.ics';
        const path = `exports/${clean.endsWith('.ics') ? clean : `${clean}.ics`}`;
        const bytes = new TextEncoder().encode(ics);
        await fs.mkdir('exports').catch(() => {});
        await fs.writeBytes(path, bytes);
        return { path, eventCount: events.length, bytesWritten: bytes.byteLength };
      }),
    });

    // -------------------------------------------------------------------------
    // Tier C: Full-Trust Lookup & Mutation Tools (Disabled in webmcp-only)
    // -------------------------------------------------------------------------
    if (mcpMode === 'full') {
      const guardFullTrust = async () => {
        const sys = window.ladybirdApp ? await window.ladybirdApp.system.info().catch(() => null) : null;
        if (sys?.mcpAccess !== 'full') {
          throw new Error('Tool is disabled unless mcpAccess is explicitly full');
        }
      };

      const capSearchRecord = (rec) => {
        if (!rec || typeof rec !== 'object') return rec;
        if (typeof rec.notes === 'string' && rec.notes.length > 300) {
          return { ...rec, notes: `${rec.notes.slice(0, 300)}...` };
        }
        return rec;
      };

      ctx.registerTool({
        name: 'search_crm',
        description: 'Search contacts, companies, deals (including all buying-committee contacts), and tasks (full mode only).',
        inputSchema: {
          type: 'object',
          properties: {
            query: { type: 'string' },
            limit: { type: 'integer', minimum: 1, maximum: 50 },
          },
          required: ['query'],
          additionalProperties: false,
        },
        execute: wrap('search_crm', 'C', async ({ query, limit = 15 }) => {
          await guardFullTrust();
          const rank = (list, fieldsFn) =>
            list
              .map((item) => {
                const best = Math.max(...fieldsFn(item).map((v) => matchScore(v, query)));
                return best >= 0 ? { item, score: best } : null;
              })
              .filter(Boolean)
              .sort((a, b) => b.score - a.score)
              .slice(0, limit)
              .map((x) => capSearchRecord(x.item));

          return {
            contacts: rank(store.contacts, (c) => [c.name, c.email, c.phone, store.company(c.companyId)?.name || '']),
            companies: rank(store.companies, (c) => [c.name, c.industry, c.website]),
            deals: rank(store.deals, (d) => [
              d.name,
              store.company(d.companyId)?.name || '',
              ...((d.contacts || []).map((x) => store.contact(x.contactId)?.name || '')),
            ]),
            tasks: store.tasks.filter((t) => fuzzyIncludes(t.title, query)).slice(0, limit),
          };
        }),
      });

      ctx.registerTool({
        name: 'get_record_detail',
        description: 'Return a full contact, company, or deal record with its linked relationships (full mode only).',
        inputSchema: {
          type: 'object',
          properties: {
            kind: { type: 'string', enum: ['contact', 'company', 'deal'] },
            id: { type: 'string' },
          },
          required: ['kind', 'id'],
          additionalProperties: false,
        },
        execute: wrap('get_record_detail', 'C', async ({ kind, id }) => {
          await guardFullTrust();
          if (kind === 'contact') {
            const record = store.contact(id);
            if (!record) throw new Error('Contact not found');
            return {
              record,
              company: store.company(record.companyId),
              deals: store.dealsFor(id),
              activities: store.activitiesFor('contact', id),
              tasks: store.tasksFor('contact', id),
              attachments: store.attachmentsFor('contact', id),
            };
          }
          if (kind === 'company') {
            const record = store.company(id);
            if (!record) throw new Error('Company not found');
            return {
              record,
              contacts: store.contactsFor(id),
              deals: store.dealsForCompany(id),
              activities: store.activitiesFor('company', id),
              attachments: store.attachmentsFor('company', id),
            };
          }
          const record = store.deal(id);
          if (!record) throw new Error('Deal not found');
          return {
            record,
            company: store.company(record.companyId),
            contacts: (record.contacts || []).map((e) => ({ ...e, contact: store.contact(e.contactId) })),
            activities: store.dealActivities(record),
            tasks: store.tasksFor('deal', id),
            attachments: store.attachmentsFor('deal', id),
          };
        }),
      });

      ctx.registerTool({
        name: 'upsert_record',
        description: 'Create or update a company, contact, deal, activity, or task (full mode only).',
        inputSchema: {
          type: 'object',
          properties: {
            kind: { type: 'string', enum: ['company', 'contact', 'deal', 'activity', 'task'] },
            id: { type: 'string' },
            data: { type: 'object' },
          },
          required: ['kind', 'data'],
          additionalProperties: false,
        },
        execute: wrap('upsert_record', 'C', async ({ kind, id = null, data }) => {
          await guardFullTrust();
          const clean = { ...(data || {}) };
          delete clean.id;
          delete clean.createdAt;
          delete clean.stageHistory;
          let record = null;
          if (kind === 'company') record = id ? await store.updateCompany(id, clean) : await store.addCompany(clean);
          else if (kind === 'contact') record = id ? await store.updateContact(id, clean) : await store.addContact(clean);
          else if (kind === 'deal') record = id ? await store.updateDeal(id, clean) : await store.addDeal(clean);
          else if (kind === 'activity') record = id ? await store.updateActivity(id, clean) : await store.addActivity(clean);
          else record = id ? await store.updateTask(id, clean) : await store.addTask(clean);
          if (id && !record) throw new Error(`${kind} not found`);
          return { record };
        }),
      });

      ctx.registerTool({
        name: 'parse_contact_or_company_text',
        description: 'Extract structured contact/company fields deterministically from pasted signature/bio text (full mode only).',
        inputSchema: {
          type: 'object',
          properties: {
            text: { type: 'string' },
            kind: { type: 'string', enum: ['contact', 'company'] },
          },
          required: ['text'],
          additionalProperties: false,
        },
        execute: wrap('parse_contact_or_company_text', 'C', async ({ text, kind = 'contact' }) => {
          await guardFullTrust();
          const raw = String(text || '').slice(0, 4000);
          const email = (raw.match(/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/) || [])[0] || '';
          const phone = (raw.match(/(?:\+?\d[\d\s().-]{6,}\d)/) || [])[0]?.trim() || '';
          const website = (raw.match(/https?:\/\/[^\s]+|(?:www\.)?[a-z0-9-]+\.[a-z]{2,}/i) || [])[0] || '';
          const lines = raw.split(/\r?\n/).map((l) => l.trim()).filter(Boolean);
          if (kind === 'company') {
            return { name: lines[0] || '', industry: '', website };
          }
          let title = lines[1] && !lines[1].includes('@') && lines[1] !== phone ? lines[1] : '';
          let company = lines[2] && !lines[2].includes('@') && lines[2] !== phone && !lines[2].includes(website) ? lines[2] : '';
          if (title && !company) {
            const splitMatch = title.match(/^(.+?)(?:,\s*|\s+(?:at|@)\s+)(.+)$/i);
            if (splitMatch) {
              title = splitMatch[1].trim();
              company = splitMatch[2].trim();
            }
          }
          return {
            name: lines[0] && !lines[0].includes('@') ? lines[0] : '',
            title,
            email,
            phone,
            company,
          };
        }),
      });
    }
  };

  window.addEventListener('ladybird:webmcp-ready', install);
  install();
}
