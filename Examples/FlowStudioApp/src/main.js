/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { cronSlotKey, matchesCron } from './lib/cron.js';
import { SUPPORTED_LANGUAGES, getLanguagePreference, setLanguage, t } from './lib/i18n.js';
import { buildFlowPack, diffFlowPackInstall, validateFlowPack } from './lib/packs.js';
import { RECORDER_DRAIN_SCRIPT, RECORDER_START_SCRIPT, RECORDER_STOP_SCRIPT, normalizeRecordedEvents } from './lib/recorder.js';
import { isLikelyAuthRedirect, runPipelineWorkflow } from './lib/runner.js';
import { createSecretStore, stripSecretsFromExport } from './lib/secrets.js';
import { getAllSiteActions } from './lib/site-packs.js';
import { cloneTemplate } from './lib/templates.js';
import {
  buildFlowDefinition,
  buildTriggerPayload,
  createBlankTrigger,
  createBlankWorkflow,
  createBlankStep,
  createLocalNode,
  createSiteActionNode,
  isSingleServiceNativeFlow,
  mergeLoadedState,
  sanitizeId,
  triggerIdForFlow,
  validateWorkflow,
} from './lib/workflow.js';
import { renderWorkflowCanvas } from './ui/canvas.js';
import { el, icon, openModal, toast } from './ui/dom.js';
import { renderInspector } from './ui/inspector.js';
import { renderEventsView, renderRunsView, renderSecretsView, renderTemplatesView, renderTriggersView } from './ui/views.js';

const META_KV_KEY = 'flowstudio:meta:v1';
const WORKFLOWS_KV_KEY = 'flowstudio:workflows:v2';
const CUSTOM_ACTIONS_KV_KEY = 'flowstudio:custom_actions:v1';
const PIPELINE_RUNS_KV_KEY = 'flowstudio:pipeline_runs:v1';

function bridge() {
  return typeof window !== 'undefined' ? window.ladybirdApp || null : null;
}

function applyTheme(mode) {
  const resolved = mode === 'light' ? 'light' : 'dark';
  document.documentElement.setAttribute('data-theme', resolved);
}

const state = {
  view: 'workflows',
  workflows: [],
  customActions: [],
  triggers: [],
  clientTriggers: [],
  runs: [],
  pipelineRuns: [],
  services: [],
  secrets: [],
  workspacePacks: [],
  events: [],
  filterEventType: 'all',
  activeWorkflowId: null,
  selectedKey: 'trigger',
  running: false,
  runQueue: [],
  activeRunAbort: null,
  activeNativeRunId: null,
  authPause: null,
  latestRun: null,
  meta: {
    bootstrapped: false,
    onboardingDismissed: false,
    theme: 'dark',
    language: 'system',
    workflows: {},
  },
};

const clientIntervalHandles = new Map();
const clientCooldowns = new Map();
const cronLastSlot = new Map();
const eventDedupe = new Map();
let cronTimerHandle = null;
let drainingQueue = false;
let secretStore = null;

function activeWorkflow() {
  return state.workflows.find((w) => w.id === state.activeWorkflowId) || null;
}

async function saveMeta() {
  const b = bridge();
  if (!b?.kv) return;
  try { await b.kv.set(META_KV_KEY, state.meta); } catch {}
}

async function saveWorkflowsKV() {
  const b = bridge();
  if (!b?.kv) return;
  const serialized = state.workflows.map(({ _isNew, _savedId, ...rest }) => rest);
  try { await b.kv.set(WORKFLOWS_KV_KEY, serialized); } catch {}
}

async function saveCustomActionsKV() {
  const b = bridge();
  if (!b?.kv) return;
  try { await b.kv.set(CUSTOM_ACTIONS_KV_KEY, state.customActions); } catch {}
}

async function savePipelineRun(rec) {
  state.pipelineRuns = [rec, ...state.pipelineRuns.filter((r) => r.run_id !== rec.run_id)].slice(0, 30);
  const b = bridge();
  if (b?.kv) {
    try { await b.kv.set(PIPELINE_RUNS_KV_KEY, state.pipelineRuns); } catch {}
  }
}

async function refreshServices() {
  const b = bridge();
  if (!b?.tabs) return;
  try {
    const res = await b.tabs.list();
    state.services = res?.services || [];
    render();
  } catch {}
}

function mergedRuns(nativeRuns = []) {
  const byId = new Map();
  for (const r of state.pipelineRuns) if (r?.run_id) byId.set(r.run_id, r);
  for (const r of nativeRuns || []) if (r?.run_id && !byId.has(r.run_id)) byId.set(r.run_id, r);
  return [...byId.values()].sort((a, b) => (b.started_at_ms || 0) - (a.started_at_ms || 0)).slice(0, 35);
}

async function refreshRuns() {
  const b = bridge();
  if (!b?.flows) return;
  try {
    const res = await b.flows.listRuns({ limit: 25 });
    state.runs = mergedRuns(res?.runs || []);
    if (state.activeWorkflowId && (!state.latestRun || state.latestRun.flow_id !== state.activeWorkflowId)) {
      state.latestRun = state.runs.find((r) => r.flow_id === state.activeWorkflowId) || null;
    }
    render();
  } catch {}
}

function allCombinedTriggers() {
  return [...state.triggers, ...state.clientTriggers];
}

function syncClientTriggers() {
  for (const h of clientIntervalHandles.values()) clearInterval(h);
  clientIntervalHandles.clear();

  state.clientTriggers = state.workflows
    .filter((w) => !isSingleServiceNativeFlow(w, state.customActions) && w.trigger?.type && w.trigger.type !== 'manual')
    .map((w) => buildTriggerPayload(w))
    .filter(Boolean)
    .map((t) => ({ ...t, clientOnly: true }));

  for (const ct of state.clientTriggers) {
    if (!ct.enabled || ct.type !== 'interval') continue;
    const ms = Math.max(5000, Number(ct.intervalMs) || 60000);
    const h = setInterval(() => {
      const wf = state.workflows.find((w) => w.id === ct.flowId);
      if (wf) fireClientWorkflowTrigger(ct, wf, { trigger_event: 'interval', trigger_service: ct.serviceId });
    }, ms);
    clientIntervalHandles.set(ct.id, h);
  }

  if (cronTimerHandle) clearInterval(cronTimerHandle);
  const runCronTick = () => {
    const now = new Date();
    const slot = cronSlotKey(now);
    for (const ct of state.clientTriggers) {
      if (!ct.enabled || ct.type !== 'cron') continue;
      if (cronLastSlot.get(ct.id) === slot) continue;
      const expr = ct.cronExpr || '0 9 * * 1-5';
      if (!matchesCron(expr, now)) continue;
      cronLastSlot.set(ct.id, slot);
      const wf = state.workflows.find((w) => w.id === ct.flowId);
      if (wf) fireClientWorkflowTrigger(ct, wf, { trigger_event: 'cron', cron_slot: slot, trigger_service: ct.serviceId });
    }
  };
  cronTimerHandle = setInterval(runCronTick, 20000);
}

async function refreshTriggers() {
  const b = bridge();
  if (!b?.triggers) return;
  try {
    const res = await b.triggers.list();
    state.triggers = res?.triggers || [];
    syncClientTriggers();
    render();
  } catch {}
}

async function ensureServiceTabReady(serviceId, activate = false, requireOrigin = true, timeoutMs = 15000) {
  const b = bridge();
  if (!b?.tabs) throw new Error('Service Tabs API unavailable');
  await refreshServices();
  let svc = state.services.find((s) => s.id === serviceId);
  if (!svc?.open) {
    await b.tabs.open(serviceId, { activate });
  }
  const deadline = Date.now() + timeoutMs;
  let sawRedirectAt = 0;
  while (Date.now() < deadline) {
    await refreshServices();
    svc = state.services.find((s) => s.id === serviceId);
    if (svc?.open && (!requireOrigin || svc?.originAllowed)) return svc;
    if (svc?.open && requireOrigin && !svc?.originAllowed && svc?.url && !svc.url.startsWith('about:')) {
      if (!sawRedirectAt) sawRedirectAt = Date.now();
      if (isLikelyAuthRedirect(svc.url) || Date.now() - sawRedirectAt > 3500) {
        const err = new Error(`Login required for '${serviceId}' (${svc.url})`);
        err.code = 'AUTH_REQUIRED';
        err.tabUrl = svc.url;
        throw err;
      }
    }
    await new Promise((r) => setTimeout(r, 350));
  }
  if (!svc?.open) throw new Error(`Service tab '${serviceId}' could not be opened`);
  if (requireOrigin && !svc?.originAllowed) {
    const err = new Error(`Service tab '${serviceId}' did not commit an allowed origin in time`);
    err.code = 'AUTH_REQUIRED';
    err.tabUrl = svc.url || '';
    throw err;
  }
  return svc;
}

function handleAuthRequired({ serviceId, url, signal }) {
  return new Promise((resolve) => {
    const b = bridge();
    let settled = false;
    let pollHandle = null;
    let modal = null;

    const finish = (ok) => {
      if (settled) return;
      settled = true;
      if (pollHandle) clearInterval(pollHandle);
      state.authPause = null;
      modal?.close();
      renderEditorOnly();
      resolve(ok);
    };

    b?.tabs?.focus(serviceId).catch(() => {});

    const body = el(
      'div.row-list',
      {},
      el('p', { style: 'margin:0' }, t('Flow paused because service "{service}" needs authentication or navigated outside allowed origins.', { service: serviceId })),
      url ? el('div.muted.mono', { style: 'font-size:11.5px;word-break:break-all' }, url) : null,
      el('p.muted', { style: 'margin:0' }, t('Sign in inside the service tab. Flow Studio will resume automatically once ready.'))
    );
    modal = openModal({
      title: t('Authentication Required'),
      body,
      foot: el(
        'div.chip-row',
        {},
        el('button.btn.btn-sm', { type: 'button', onclick: () => openOrFocusService(serviceId) }, icon('tab', 12), t('Focus Tab')),
        el('button.btn.btn-sm.btn-danger', { type: 'button', onclick: () => finish(false) }, t('Cancel')),
        el('button.btn.btn-sm.btn-primary', { type: 'button', onclick: () => finish(true) }, t('Resume Run'))
      ),
    });

    state.authPause = { serviceId, url, resolve: finish };
    renderEditorOnly();

    signal?.addEventListener('abort', () => finish(false), { once: true });

    pollHandle = setInterval(async () => {
      if (settled) return;
      await refreshServices();
      const svc = state.services.find((s) => s.id === serviceId);
      if (svc?.open && svc?.originAllowed) finish(true);
    }, 1000);
  });
}

function handleApprovalRequired({ stepIndex, label, serviceId, action, preview, signal }) {
  return new Promise((resolve) => {
    let settled = false;
    let modal = null;
    const finish = (ok) => {
      if (settled) return;
      settled = true;
      modal?.close();
      resolve(ok);
    };
    const body = el(
      'div.row-list',
      {},
      el('p', { style: 'margin:0' }, t('Step {n} "{label}" requires your approval before executing on {service}.', { n: stepIndex + 1, label, service: serviceId })),
      el('div.chip-row', {}, el('span.pill.pill-accent', {}, serviceId), el('span.pill', {}, action)),
      el('pre.approval-preview', {}, JSON.stringify(preview || {}, null, 2))
    );
    modal = openModal({
      title: t('Approval Required'),
      body,
      foot: el(
        'div.chip-row',
        {},
        el('button.btn.btn-sm.btn-danger', { type: 'button', onclick: () => finish(false) }, t('Reject')),
        el('button.btn.btn-sm.btn-good', { type: 'button', onclick: () => finish(true) }, t('Approve & Run'))
      ),
    });
    signal?.addEventListener('abort', () => finish(false), { once: true });
  });
}

function openRecorderModal({ asSiteAction = false } = {}) {
  const b = bridge();
  const svcOpts = state.services.length ? state.services : [{ id: 'mail', label: 'Gmail' }];
  let targetSvc = activeWorkflow()?.serviceId || svcOpts[0].id;
  let recording = false;
  let pollHandle = null;
  const capturedRaw = [];
  let dlg = null;

  const svcSelect = el(
    'select.select',
    { onchange: (e) => { targetSvc = e.target.value; } },
    ...svcOpts.map((s) => el('option', { value: s.id, selected: s.id === targetSvc }, `${s.label} (${s.id})`))
  );
  const statusBanner = el('div.chip-row', {}, el('span.pill', {}, t('Idle — choose service and click Start Recording')));
  const listBox = el('div.row-list', { style: 'max-height:220px;overflow:auto' });

  const renderCaptured = () => {
    const steps = normalizeRecordedEvents(capturedRaw, targetSvc);
    if (!steps.length) {
      listBox.replaceChildren(el('p.muted', { style: 'margin:4px 0' }, t('No interactions recorded yet. Click, type, or Alt+Click on the target page to extract text.')));
      return;
    }
    listBox.replaceChildren(...steps.map((st, idx) =>
      el('div.list-row', { style: 'padding:6px 10px' },
        el('span.mono', { style: 'font-size:11.5px' }, `${idx + 1}. [${st.action}] ${st.label}`),
        el('span.pill', {}, st.locator?.kind || st.key || '')
      )
    ));
  };

  const stopRecordingScript = async () => {
    if (pollHandle) {
      clearInterval(pollHandle);
      pollHandle = null;
    }
    if (recording && b?.tabs?.eval) {
      recording = false;
      try {
        const res = await b.tabs.eval(targetSvc, RECORDER_DRAIN_SCRIPT);
        if (Array.isArray(res?.value?.events)) capturedRaw.push(...res.value.events);
      } catch {}
      await b.tabs.eval(targetSvc, RECORDER_STOP_SCRIPT).catch(() => {});
    }
  };

  const startRec = async () => {
    if (!b?.tabs?.eval) {
      toast('Service Tabs API unavailable', { tone: 'danger' });
      return;
    }
    try {
      await ensureServiceTabReady(targetSvc, true, true);
      await b.tabs.eval(targetSvc, RECORDER_START_SCRIPT);
      recording = true;
      svcSelect.disabled = true;
      statusBanner.replaceChildren(el('span.rec-dot'), el('span.pill.pill-danger', {}, t('Recording live on "{service}"…', { service: targetSvc })));
      pollHandle = setInterval(async () => {
        if (!recording) return;
        try {
          const res = await b.tabs.eval(targetSvc, RECORDER_DRAIN_SCRIPT);
          if (Array.isArray(res?.value?.events) && res.value.events.length) {
            capturedRaw.push(...res.value.events);
            renderCaptured();
          }
        } catch {}
      }, 700);
    } catch (err) {
      toast(err?.message || 'Failed starting recorder', { tone: 'danger' });
    }
  };

  const finishAddToFlow = async () => {
    await stopRecordingScript();
    dlg?.close();
    const steps = normalizeRecordedEvents(capturedRaw, targetSvc);
    if (!steps.length) {
      toast(t('No recorded steps to add'), { tone: 'warn' });
      return;
    }
    let wf = activeWorkflow();
    if (!wf) {
      wf = createBlankWorkflow(targetSvc, t('Recorded Flow'));
      wf.steps = [];
      state.workflows.unshift(wf);
      state.activeWorkflowId = wf.id;
    }
    wf.steps.push(...steps);
    state.selectedKey = `step:${wf.steps.length - 1}`;
    state.view = 'workflows';
    toast(t('Added {n} recorded steps', { n: steps.length }));
    render();
  };

  const finishSaveSiteAction = async () => {
    await stopRecordingScript();
    dlg?.close();
    const steps = normalizeRecordedEvents(capturedRaw, targetSvc);
    if (!steps.length) {
      toast(t('No recorded steps to save'), { tone: 'warn' });
      return;
    }
    const actionId = sanitizeId(`rec-${targetSvc}-${Date.now().toString(36).slice(-4)}`, 'rec-action').replace(/-/g, '.');
    const lastExtract = [...steps].reverse().find((s) => s.save_as)?.save_as || 'recorded_result';
    const def = {
      id: actionId,
      name: `${targetSvc}: Recorded (${steps.length} steps)`,
      serviceId: targetSvc,
      category: 'act',
      defaultSaveAs: lastExtract,
      description: 'Recorded visually in Flow Studio',
      fields: [],
      steps: steps.map(({ kind, serviceId, runWhen, retryCount, retryDelayMs, onError, requireApproval, ...clean }) => clean),
    };
    state.customActions = [def, ...state.customActions.filter((c) => c.id !== def.id)];
    await saveCustomActionsKV();
    toast(t('Custom site action saved'));
    render();
  };

  renderCaptured();
  dlg = openModal({
    title: t('Visual Action Recorder'),
    body: el(
      'div.row-list',
      {},
      el('div.field', {}, el('label.field-label', {}, t('Target Service')), svcSelect),
      statusBanner,
      listBox
    ),
    foot: el(
      'div.chip-row',
      {},
      el('button.btn.btn-sm', { type: 'button', onclick: async () => { await stopRecordingScript(); dlg.close(); } }, t('Cancel')),
      el('button.btn.btn-sm.btn-primary', { type: 'button', onclick: startRec }, el('span.rec-dot'), t('Start Recording')),
      asSiteAction
        ? el('button.btn.btn-sm.btn-good', { type: 'button', onclick: finishSaveSiteAction }, t('Stop & Save Site Action'))
        : el('button.btn.btn-sm.btn-good', { type: 'button', onclick: finishAddToFlow }, t('Stop & Add to Flow')),
      !asSiteAction
        ? el('button.btn.btn-sm', { type: 'button', onclick: finishSaveSiteAction }, t('Save as Site Action'))
        : null
    ),
  });
}

function cancelActiveRun() {
  if (state.authPause?.resolve) state.authPause.resolve(false);
  if (state.activeRunAbort) state.activeRunAbort.abort();
  if (state.activeNativeRunId) {
    bridge()?.flows?.cancelRun(state.activeNativeRunId).catch(() => {});
  }
}

async function drainRunQueue() {
  if (drainingQueue) return;
  drainingQueue = true;
  try {
    while (state.runQueue.length > 0) {
      const job = state.runQueue.shift();
      const wf = state.workflows.find((w) => w.id === job.wfId) || job.wfSnapshot;
      const b = bridge();
      if (!wf || !b?.tabs) {
        job.resolve(null);
        continue;
      }

      const ac = new AbortController();
      state.activeRunAbort = ac;
      state.activeNativeRunId = null;
      state.running = true;
      render();

      try {
        const rec = await runPipelineWorkflow(wf, {
          bridge: b,
          customActions: state.customActions,
          ensureServiceTabReady,
          initialVars: job.initialVars || {},
          signal: ac.signal,
          startFromStep: job.startFromStep || 0,
          seedOutputs: job.seedOutputs || {},
          onAuthRequired: handleAuthRequired,
          onApprovalRequired: handleApprovalRequired,
          resolveSecrets: () => (secretStore ? secretStore.resolveMap() : Promise.resolve({})),
          onNativeRunStart: (nativeId) => { state.activeNativeRunId = nativeId; },
          onProgress: (snap) => {
            if (state.activeWorkflowId === wf.id) {
              state.latestRun = snap;
              renderEditorOnly();
            }
          },
        });
        if (state.activeWorkflowId === wf.id) state.latestRun = rec;
        await savePipelineRun(rec);
        await refreshRuns();
        await refreshServices();
        if (job.source === 'manual') {
          if (rec.status === 'failed') toast(t('Run failed: {error}', { error: rec.error || 'error' }), { tone: 'danger' });
          else if (rec.status === 'cancelled') toast(t('Run cancelled'), { tone: 'warn' });
          else toast(t('Run finished ({status})', { status: rec.status }));
        }
        job.resolve(rec);
      } catch (err) {
        if (job.source === 'manual') toast(t('Run failed: {error}', { error: err?.message || 'error' }), { tone: 'danger' });
        job.reject(err);
      } finally {
        state.activeRunAbort = null;
        state.activeNativeRunId = null;
        state.running = false;
        render();
      }
    }
  } finally {
    drainingQueue = false;
  }
}

function enqueueWorkflowRun(wf, opts = {}) {
  if (!wf) return Promise.resolve(null);
  const source = opts.source || 'trigger';
  if (source === 'trigger') {
    const dup = state.runQueue.find((j) => j.wfId === wf.id && j.source === 'trigger');
    if (dup) return dup.promise;
  }
  if (state.runQueue.length >= 20) {
    toast(t('Run queue limit reached (20)'), { tone: 'warn' });
    return Promise.resolve(null);
  }
  let resolveJob, rejectJob;
  const promise = new Promise((res, rej) => {
    resolveJob = res;
    rejectJob = rej;
  });
  state.runQueue.push({
    wfId: wf.id,
    wfSnapshot: wf,
    source,
    initialVars: opts.initialVars || {},
    startFromStep: opts.startFromStep || 0,
    seedOutputs: opts.seedOutputs || {},
    promise,
    resolve: resolveJob,
    reject: rejectJob,
  });
  if (state.running && source === 'manual') {
    toast(t('Added to run queue (#{n})', { n: state.runQueue.length }));
    renderEditorOnly();
  }
  drainRunQueue();
  return promise;
}

async function fireClientWorkflowTrigger(ct, wf, vars) {
  const now = Date.now();
  const lastAt = clientCooldowns.get(ct.id) || 0;
  if (now - lastAt < (ct.cooldownMs || 5000)) return;
  clientCooldowns.set(ct.id, now);
  return enqueueWorkflowRun(wf, { source: 'trigger', initialVars: vars });
}

function evaluateClientEventTriggers(ev) {
  const fp = `${ev.type}|${ev.serviceId}|${ev.data?.triggerId || ''}|${ev.type === 'webhook' || ev.type === 'cron' ? (ev.seq || '') : ''}|${ev.data?.url || ''}|${ev.data?.title || ''}|${ev.data?.statusCode || ''}`;
  const now = Date.now();
  const prev = eventDedupe.get(fp) || 0;
  if (now - prev < 1500) return;
  eventDedupe.set(fp, now);
  if (eventDedupe.size > 200) {
    for (const [k, ts] of eventDedupe) {
      if (now - ts > 10000) eventDedupe.delete(k);
    }
  }

  if (ev.type === 'webhook' || ev.type === 'cron') {
    const wf = state.workflows.find((w) => w.trigger?.type === ev.type && (w.trigger?.id || triggerIdForFlow(w.id)) === ev.data?.triggerId);
    if (wf && wf.trigger?.enabled !== false && !isSingleServiceNativeFlow(wf, state.customActions)) {
      enqueueWorkflowRun(wf, {
        source: 'trigger',
        initialVars: ev.type === 'webhook'
          ? {
              trigger_event: 'webhook',
              webhook_payload: ev.data?.payload ?? ev.data?.raw ?? {},
            }
          : {
              trigger_event: 'cron',
              cron_slot: ev.data?.slot || '',
              trigger_service: ev.serviceId,
            },
      });
    }
    return;
  }

  for (const ct of state.clientTriggers) {
    if (!ct.enabled || ct.type !== ev.type || ct.serviceId !== ev.serviceId) continue;
    const d = ev.data || {};
    if (ct.urlContains && !String(d.url || '').includes(ct.urlContains)) continue;
    if (ct.method && !String(d.method || '').toUpperCase().includes(ct.method.toUpperCase())) continue;
    if (ct.statusCode && Number(d.statusCode) !== Number(ct.statusCode)) continue;
    if (ct.titleContains && !String(d.title || '').includes(ct.titleContains)) continue;
    if (ct.bodyContains && !String(d.body || '').includes(ct.bodyContains)) continue;
    const wf = state.workflows.find((w) => w.id === ct.flowId);
    if (wf) {
      fireClientWorkflowTrigger(ct, wf, {
        trigger_event: ev.type,
        trigger_service: ev.serviceId,
        network_url: d.url || '',
        notification_title: d.title || '',
        notification_body: d.body || '',
        navigation_url: d.url || '',
      });
    }
  }
}

async function persistWorkflow(wf) {
  const b = bridge();
  if (!b?.flows) return false;
  const errs = validateWorkflow(wf, state.customActions);
  if (errs.length) {
    toast(errs[0], { tone: 'warn' });
    return false;
  }
  const prevId = wf._savedId || wf.id;
  const nativeEligible = isSingleServiceNativeFlow(wf, state.customActions);
  const flowDef = buildFlowDefinition(wf, state.customActions);
  wf.id = flowDef.id;
  wf._savedId = flowDef.id;
  wf._isNew = false;

  try {
    if (nativeEligible) {
      await b.flows.save(flowDef);
      if (prevId && prevId !== flowDef.id) {
        await b.flows.delete(prevId).catch(() => {});
        await b.triggers?.delete(triggerIdForFlow(prevId)).catch(() => {});
      }
      const trigPayload = buildTriggerPayload(wf, { clientHandled: false });
      if (trigPayload && b.triggers) {
        const savedTrig = await b.triggers.save(trigPayload);
        wf.trigger = { ...wf.trigger, ...(savedTrig?.trigger || trigPayload) };
      } else if (b.triggers) {
        const tid = wf.trigger?.id || triggerIdForFlow(flowDef.id);
        await b.triggers.delete(tid).catch(() => {});
      }
    } else if (b.triggers) {
      if (wf.trigger?.type === 'webhook' && wf.trigger.enabled !== false) {
        const trigPayload = buildTriggerPayload(wf, { clientHandled: true });
        const savedTrig = await b.triggers.save(trigPayload);
        wf.trigger = { ...wf.trigger, ...(savedTrig?.trigger || trigPayload) };
      } else {
        const tid = wf.trigger?.id || triggerIdForFlow(flowDef.id);
        await b.triggers.delete(tid).catch(() => {});
      }
    }

    state.meta.workflows[flowDef.id] = {
      serviceId: wf.serviceId,
      nodePositions: wf.nodePositions || {},
    };
    await saveWorkflowsKV();
    await saveMeta();
    await refreshTriggers();
    state.activeWorkflowId = flowDef.id;
    toast(!nativeEligible ? t('Saved locally (multi-service/hybrid workflow)') : buildTriggerPayload(wf) ? t('Workflow saved + trigger active') : t('Workflow saved'));
    render();
    return true;
  } catch (err) {
    toast(err?.message || t('Failed saving workflow'), { tone: 'danger' });
    return false;
  }
}

async function runActiveWorkflow(startFromStep = 0) {
  const wf = activeWorkflow();
  const b = bridge();
  if (!wf || !b?.tabs) return;
  const ok = await persistWorkflow(wf);
  if (!ok) return;
  const seedOutputs = startFromStep > 0 ? (state.latestRun?.outputs || {}) : {};
  return enqueueWorkflowRun(wf, { source: 'manual', startFromStep, seedOutputs });
}

async function deleteWorkflow(wf) {
  if (!wf) return;
  let dlg = null;
  dlg = openModal({
    title: t('Delete "{name}"?', { name: wf.name || wf.id }),
    body: el('p.muted', {}, wf.id),
    foot: el(
      'div.chip-row',
      {},
      el('button.btn', { type: 'button', onclick: () => dlg.close() }, t('Cancel')),
      el('button.btn.btn-danger', {
        type: 'button',
        onclick: async () => {
          dlg.close();
          const b = bridge();
          if (b?.flows) await b.flows.delete(wf.id).catch(() => {});
          if (b?.triggers) await b.triggers.delete(wf.trigger?.id || triggerIdForFlow(wf.id)).catch(() => {});
          delete state.meta.workflows[wf.id];
          state.workflows = state.workflows.filter((w) => w.id !== wf.id);
          await saveWorkflowsKV();
          await saveMeta();
          if (state.activeWorkflowId === wf.id) {
            state.activeWorkflowId = state.workflows[0]?.id || null;
            state.selectedKey = 'trigger';
          }
          await refreshTriggers();
          toast(t('Workflow deleted'));
          render();
        },
      }, t('Confirm'))
    ),
  });
}

async function installTemplate(templateId, openImmediately = true) {
  const wf = cloneTemplate(templateId);
  if (!wf) return null;
  const existingIdx = state.workflows.findIndex((w) => w.id === wf.id);
  if (existingIdx >= 0) {
    state.workflows[existingIdx] = wf;
  } else {
    state.workflows.unshift(wf);
  }
  await persistWorkflow(wf);
  if (openImmediately) {
    state.activeWorkflowId = wf.id;
    state.selectedKey = 'trigger';
    state.view = 'workflows';
    render();
  }
  toast(t('Installed "{name}"', { name: t(wf.name) }));
  return wf;
}

function createTriggerFromLiveEvent(ev) {
  const wf = createBlankWorkflow(ev.serviceId || 'mail', `${ev.serviceId}-${ev.type}`);
  wf._isNew = true;
  wf.trigger = createBlankTrigger(ev.serviceId || 'mail');
  wf.trigger.type = ev.type;
  wf.trigger.enabled = true;
  if (ev.type === 'network') {
    const rawUrl = String(ev.data?.url || '');
    try { wf.trigger.urlContains = new URL(rawUrl).pathname; } catch { wf.trigger.urlContains = rawUrl.slice(0, 64); }
    wf.trigger.method = ev.data?.method || 'POST';
    wf.trigger.statusCode = ev.data?.statusCode || '';
  } else if (ev.type === 'notification') {
    wf.trigger.titleContains = String(ev.data?.title || '').slice(0, 40);
  } else if (ev.type === 'navigation') {
    wf.trigger.urlContains = String(ev.data?.url || '').slice(0, 64);
  }
  state.workflows.unshift(wf);
  state.activeWorkflowId = wf.id;
  state.selectedKey = 'trigger';
  state.view = 'workflows';
  render();
}

async function openOrFocusService(serviceId) {
  const b = bridge();
  if (!b?.tabs) return;
  const svc = state.services.find((s) => s.id === serviceId);
  try {
    if (svc?.open) await b.tabs.focus(serviceId);
    else await b.tabs.open(serviceId);
    await refreshServices();
  } catch (err) {
    toast(err?.message || 'Tab unavailable', { tone: 'warn' });
  }
}

async function exportActiveWorkflowJson() {
  const wf = activeWorkflow();
  const b = bridge();
  if (!wf || !b?.fs) return;
  const path = `exports/${sanitizeId(wf.id, 'flow')}.json`;
  try {
    const secretMap = secretStore ? await secretStore.resolveMap() : {};
    const safePayload = stripSecretsFromExport(
      { workflow: wf, nativeFlow: buildFlowDefinition(wf, state.customActions), trigger: buildTriggerPayload(wf) },
      secretMap
    );
    await b.fs.writeText(path, JSON.stringify(safePayload, null, 2));
    toast(t('Exported to app workspace: {path}', { path }));
  } catch (err) {
    toast(err?.message || 'Export failed', { tone: 'danger' });
  }
}

function openAddServiceModal() {
  const labelInp = el('input.input', { placeholder: 'Slack Work / Notion / ERP' });
  const idInp = el('input.input.mono', { placeholder: 'slack-work' });
  let idEdited = false;
  labelInp.addEventListener('input', () => {
    if (!idEdited) idInp.value = sanitizeId(labelInp.value, '');
  });
  idInp.addEventListener('input', () => { idEdited = Boolean(idInp.value.trim()); });
  const urlInp = el('input.input.mono', { placeholder: 'https://app.slack.com/client' });
  const containerInp = el('input.input.mono', { value: 'personal', placeholder: 'personal / work' });
  const originsInp = el('input.input.mono', { placeholder: 'https://slack.com (optional comma-separated)' });

  let dlg = null;
  dlg = openModal({
    title: t('+ Add Service'),
    body: el(
      'div.row-list',
      {},
      el('div.field', {}, el('label.field-label', {}, t('Name')), labelInp),
      el('div.field', {}, el('label.field-label', {}, t('Service ID')), idInp),
      el('div.field', {}, el('label.field-label', {}, t('Default URL')), urlInp),
      el('div.field', {}, el('label.field-label', {}, t('Container / Account')), containerInp),
      el('div.field', {}, el('label.field-label', {}, t('Extra Allowed Origins (optional)')), originsInp)
    ),
    foot: el(
      'div.chip-row',
      {},
      el('button.btn', { type: 'button', onclick: () => dlg.close() }, t('Cancel')),
      el('button.btn.btn-primary', {
        type: 'button',
        onclick: async () => {
          const b = bridge();
          if (!b?.tabs?.registerService) return;
          const label = labelInp.value.trim();
          const defaultUrl = urlInp.value.trim();
          const id = sanitizeId(idInp.value.trim() || label, '');
          const container = containerInp.value.trim() || 'personal';
          if (!label || !id || !defaultUrl) {
            toast(t('Name, ID, and valid URL are required'), { tone: 'warn' });
            return;
          }
          let baseOrigin = '';
          try { baseOrigin = new URL(defaultUrl).origin; } catch {
            toast(t('Invalid URL format'), { tone: 'warn' });
            return;
          }
          const origins = [baseOrigin];
          for (const extra of originsInp.value.split(',').map((s) => s.trim()).filter(Boolean)) {
            try {
              const o = new URL(extra).origin;
              if (!origins.includes(o)) origins.push(o);
            } catch {}
          }
          try {
            await b.tabs.registerService({
              id,
              label,
              defaultUrl,
              origins,
              container,
              permissions: ['open', 'navigate', 'notifications', 'eval', 'flows', 'network'],
            });
            dlg.close();
            await refreshServices();
            toast(t('Service "{name}" added', { name: label }));
          } catch (err) {
            toast(err?.message || 'Failed registering service', { tone: 'danger' });
          }
        },
      }, t('Save Service'))
    ),
  });
}

function removeDynamicService(svc) {
  let dlg = null;
  dlg = openModal({
    title: t('Remove service "{name}"?', { name: svc.label || svc.id }),
    body: el('p.muted', {}, svc.id),
    foot: el(
      'div.chip-row',
      {},
      el('button.btn', { type: 'button', onclick: () => dlg.close() }, t('Cancel')),
      el('button.btn.btn-danger', {
        type: 'button',
        onclick: async () => {
          dlg.close();
          try {
            await bridge()?.tabs?.removeService?.(svc.id);
            await refreshServices();
            toast(t('Service removed'));
          } catch (err) {
            toast(err?.message || 'Failed removing service', { tone: 'danger' });
          }
        },
      }, t('Confirm'))
    ),
  });
}

async function refreshWorkspacePacks() {
  const b = bridge();
  if (!b?.fs?.listDir) return;
  try {
    await b.fs.mkdir('packs').catch(() => {});
    const res = await b.fs.listDir('packs');
    const entries = res?.entries || [];
    state.workspacePacks = entries
      .filter((e) => !e.isDirectory && String(e.name || '').endsWith('.flowpack.json'))
      .map((e) => ({ name: e.name, path: `packs/${e.name}` }));
  } catch {}
}

async function testWebhookTrigger(wf) {
  if (!wf) return;
  const ok = await persistWorkflow(wf);
  if (!ok) return;
  const trigId = wf.trigger?.id || triggerIdForFlow(wf.id);
  const secret = wf.trigger?.webhookSecret;
  if (!secret) {
    toast(t('Save workflow as Webhook trigger first'), { tone: 'warn' });
    return;
  }
  try {
    const resp = await fetch(`/__ladybird_webhook/${encodeURIComponent(trigId)}`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'X-Webhook-Token': secret,
      },
      body: JSON.stringify({ test: true, source: 'flow-studio-ui', sentAt: new Date().toISOString() }),
    });
    const data = await resp.json().catch(() => ({}));
    if (!resp.ok) throw new Error(data?.error || `HTTP ${resp.status}`);
    toast(t('Webhook accepted (seq #{seq})', { seq: data.seq || 0 }));
  } catch (err) {
    toast(err?.message || 'Webhook test failed', { tone: 'danger' });
  }
}

async function applyHealSuggestion(stepIndex, locator) {
  const wf = activeWorkflow();
  if (!wf || !wf.steps?.[stepIndex]) return;
  wf.steps[stepIndex].locator = JSON.parse(JSON.stringify(locator));
  await persistWorkflow(wf);
  toast(t('Applied healed locator to Step {n}', { n: stepIndex + 1 }));
  return runActiveWorkflow(stepIndex);
}

async function exportFlowPack(wfOverride = null) {
  const b = bridge();
  if (!b?.fs) return null;
  const targetWf = wfOverride || activeWorkflow();
  const workflows = targetWf ? [targetWf] : state.workflows;
  const usedActions = new Set();
  for (const w of workflows) {
    for (const s of w.steps || []) {
      if (s.kind === 'site_action' && s.siteActionId) usedActions.add(s.siteActionId);
      if (s.bodyStep?.kind === 'site_action' && s.bodyStep.siteActionId) usedActions.add(s.bodyStep.siteActionId);
    }
  }
  const secretMap = secretStore ? await secretStore.resolveMap() : {};
  const pack = await buildFlowPack({
    name: targetWf ? `${targetWf.name} Pack` : 'Flow Studio Workspace Pack',
    packId: targetWf ? targetWf.id : `workspace-${Date.now().toString(36)}`,
    description: targetWf?.description || 'Portable sanitized Flow Studio bundle (.flowpack.json)',
    workflows,
    customActions: state.customActions.filter((a) => !targetWf || usedActions.has(a.id)),
    services: state.services,
    secretMap,
  });
  const relPath = `packs/${pack.packId}.flowpack.json`;
  await b.fs.writeText(relPath, JSON.stringify(pack, null, 2));
  await refreshWorkspacePacks();
  toast(t('Exported FlowPack: {path}', { path: relPath }));
  render();
  return { path: relPath, pack };
}

async function openImportPackModal(initialJson = '') {
  const jsonArea = el('textarea.textarea', {
    style: 'min-height:170px',
    value: initialJson,
    placeholder: '{"schema_version":1,"packId":"...","workflows":[...]}',
  });
  const fileInp = el('input', {
    type: 'file',
    accept: '.json,.flowpack.json',
    onchange: async (e) => {
      const f = e.target.files?.[0];
      if (!f) return;
      jsonArea.value = await f.text();
    },
  });
  const reviewBox = el('div.row-list', {});

  const inspectJson = async () => {
    let pack;
    try {
      pack = JSON.parse(jsonArea.value);
    } catch {
      reviewBox.replaceChildren(el('div', { style: 'color:var(--danger)' }, t('Invalid JSON format')));
      return null;
    }
    const val = await validateFlowPack(pack);
    if (!val.valid) {
      reviewBox.replaceChildren(el('div', { style: 'color:var(--danger)' }, val.errors[0] || 'Invalid FlowPack'));
      return null;
    }
    const diff = diffFlowPackInstall(pack, {
      existingServices: state.services,
      existingSecrets: state.secrets,
      existingWorkflows: state.workflows,
      existingActions: state.customActions,
    });
    reviewBox.replaceChildren(
      el('div.chip-row', {},
        el('span.pill.pill-good', {}, val.integrityValid ? t('SHA-256 Verified') : t('Schema Valid')),
        el('span.pill.pill-accent', {}, `${t('Workflows')}: ${pack.workflows.length}`),
        el('span.pill', {}, `${t('Site Actions')}: ${pack.siteActions.length}`),
        diff.newServices.length ? el('span.pill.pill-warn', {}, t('New services: {n}', { n: diff.newServices.length })) : null,
        diff.missingSecrets.length ? el('span.pill.pill-danger', {}, t('Missing secrets: {list}', { list: diff.missingSecrets.join(', ') })) : null
      )
    );
    return { pack, diff };
  };

  if (initialJson) await inspectJson();

  let dlg = null;
  dlg = openModal({
    title: t('Import FlowPack (.flowpack.json)'),
    body: el(
      'div.row-list',
      {},
      el('div.field', {}, el('label.field-label', {}, t('Load .flowpack.json file')), fileInp),
      el('div.field', {}, el('label.field-label', {}, t('Or paste FlowPack JSON')), jsonArea),
      reviewBox
    ),
    foot: el(
      'div.chip-row',
      {},
      el('button.btn.btn-sm', { type: 'button', onclick: inspectJson }, t('Verify Pack')),
      el('button.btn.btn-sm', { type: 'button', onclick: () => dlg.close() }, t('Cancel')),
      el('button.btn.btn-sm.btn-primary', {
        type: 'button',
        onclick: async () => {
          const inspected = await inspectJson();
          if (!inspected) return;
          const { pack, diff } = inspected;
          const b = bridge();
          for (const svc of diff.newServices) {
            await b?.tabs?.registerService?.(svc).catch(() => {});
          }
          for (const act of pack.siteActions || []) {
            state.customActions = [act, ...state.customActions.filter((a) => a.id !== act.id)];
          }
          await saveCustomActionsKV();
          for (const rawWf of pack.workflows || []) {
            const existingIdx = state.workflows.findIndex((w) => w.id === rawWf.id);
            const wf = { ...rawWf, nodePositions: rawWf.nodePositions || {}, pinnedOutputs: rawWf.pinnedOutputs || {} };
            if (existingIdx >= 0) state.workflows[existingIdx] = wf;
            else state.workflows.unshift(wf);
          }
          await saveWorkflowsKV();
          await refreshServices();
          syncClientTriggers();
          state.activeWorkflowId = pack.workflows[0]?.id || state.activeWorkflowId;
          state.view = 'workflows';
          dlg.close();
          toast(t('Imported "{name}" ({n} workflows)', { name: pack.name, n: pack.workflows.length }));
          if (diff.missingSecrets.length) {
            toast(t('Configure missing secrets in Secrets tab: {list}', { list: diff.missingSecrets.join(', ') }), { tone: 'warn', duration: 6000 });
          }
          render();
        },
      }, t('Install FlowPack'))
    ),
  });
}

let canvasEl = null;
let inspectorEl = null;

function buildAddNode(payload) {
  if (payload.kind === 'site_action') return createSiteActionNode(payload.siteActionId, state.customActions);
  if (['transform', 'condition', 'foreach', 'http', 'save_file'].includes(payload.kind)) return createLocalNode(payload.kind);
  return createBlankStep(payload.action || 'eval');
}

function renderEditorOnly() {
  const wf = activeWorkflow();
  const svc = state.services.find((s) => s.id === wf?.serviceId) || null;
  if (canvasEl) {
    renderWorkflowCanvas(canvasEl, {
      workflow: wf,
      selectedKey: state.selectedKey,
      serviceState: svc,
      customActions: state.customActions,
      latestRun: state.latestRun && state.latestRun.flow_id === wf?.id ? state.latestRun : null,
      running: state.running,
      queueLength: state.runQueue.length,
      authPause: state.authPause,
      onboarding: {
        dismissed: Boolean(state.meta.onboardingDismissed),
        serviceOpen: Boolean(svc?.open),
        hasRun: state.runs.length > 0,
      },
      onSelectNode: (key) => { state.selectedKey = key; renderEditorOnly(); },
      onAddNode: (payload) => {
        if (!wf) return;
        wf.steps.push(buildAddNode(payload));
        state.selectedKey = `step:${wf.steps.length - 1}`;
        renderEditorOnly();
      },
      onMoveNode: (key, pos) => {
        if (!wf) return;
        wf.nodePositions = { ...(wf.nodePositions || {}), [key]: pos };
        state.meta.workflows[wf.id] = { serviceId: wf.serviceId, nodePositions: wf.nodePositions };
        saveWorkflowsKV();
        saveMeta();
      },
      onSave: () => persistWorkflow(wf),
      onRun: () => runActiveWorkflow(0),
      onCancelRun: () => cancelActiveRun(),
      onStartRecording: () => openRecorderModal({ asSiteAction: false }),
      onDismissOnboarding: async () => {
        state.meta.onboardingDismissed = true;
        await saveMeta();
        renderEditorOnly();
      },
      onApplyHealSuggestion: (stepIdx, loc) => applyHealSuggestion(stepIdx, loc),
      onFocusAuthTab: (sid) => openOrFocusService(sid),
      onOpenService: (sid) => openOrFocusService(sid),
      onExportJson: () => exportActiveWorkflowJson(),
    });
  }
  if (inspectorEl) {
    renderInspector(inspectorEl, {
      workflow: wf,
      selectedKey: state.selectedKey,
      services: state.services,
      customActions: state.customActions,
      secretNames: (state.secrets || []).map((s) => s.name),
      latestRun: state.latestRun && state.latestRun.flow_id === wf?.id ? state.latestRun : null,
      running: state.running,
      onMutate: (fn, opts = {}) => {
        if (!wf) return;
        fn(wf);
        if (opts.selectKey) state.selectedKey = opts.selectKey;
        if (opts.silent) return;
        if (opts.renderCanvasOnly) {
          renderEditorOnly();
          return;
        }
        render();
      },
      onDeleteWorkflow: (w) => deleteWorkflow(w),
      onTestWebhook: (w) => testWebhookTrigger(w),
      onRunFromStep: (stepIdx) => runActiveWorkflow(stepIdx),
      onPinStepOutput: async (stepIdx, data) => {
        if (!wf) return;
        wf.pinnedOutputs = { ...(wf.pinnedOutputs || {}), [String(stepIdx)]: data };
        await saveWorkflowsKV();
        toast(t('Output pinned for Step {n}', { n: stepIdx + 1 }));
        renderEditorOnly();
      },
      onUnpinStepOutput: async (stepIdx) => {
        if (!wf || !wf.pinnedOutputs) return;
        delete wf.pinnedOutputs[String(stepIdx)];
        await saveWorkflowsKV();
        toast(t('Unpinned Step {n}', { n: stepIdx + 1 }));
        renderEditorOnly();
      },
    });
  }
}

function render() {
  const root = document.getElementById('app');
  if (!root) return;

  const combinedTriggers = allCombinedTriggers();
  const navItems = [
    { id: 'workflows', icon: 'flow', label: 'Workflows', count: state.workflows.length },
    { id: 'triggers', icon: 'bolt', label: 'Triggers', count: combinedTriggers.length },
    { id: 'events', icon: 'activity', label: 'Live Events', count: state.events.length },
    { id: 'runs', icon: 'play', label: 'Runs', count: state.runs.length },
    { id: 'templates', icon: 'pack', label: 'Session Packs' },
    { id: 'secrets', icon: 'shield', label: 'Secrets', count: state.secrets.length },
  ];

  const sidebar = el(
    'aside.sidebar',
    {},
    el(
      'div.brand',
      {},
      el('img', { src: '/icon.svg', width: 28, height: 28, alt: '' }),
      el('div', {}, el('div.brand-title', {}, 'Flow Studio'), el('div.brand-sub', {}, t('Browser Automation Studio')))
    ),
    el(
      'div.sidebar-list',
      {},
      ...navItems.map((n) =>
        el(
          'button',
          {
            type: 'button',
            class: `nav-btn${state.view === n.id ? ' active' : ''}`,
            onclick: () => {
              state.view = n.id;
              if (n.id === 'runs') refreshRuns();
              if (n.id === 'triggers') refreshTriggers();
              render();
            },
          },
          icon(n.icon, 15),
          el('span', { style: 'flex:1' }, t(n.label)),
          n.count ? el('span.pill', {}, String(n.count)) : null
        )
      )
    ),
    el(
      'div.nav-section-label',
      {},
      el('span', {}, t('Workflows')),
      el('button.btn.btn-sm', {
        type: 'button',
        onclick: () => {
          const nw = createBlankWorkflow(state.services[0]?.id || 'mail', t('New Flow'));
          nw._isNew = true;
          state.workflows.unshift(nw);
          state.activeWorkflowId = nw.id;
          state.selectedKey = 'trigger';
          state.view = 'workflows';
          render();
        },
      }, icon('plus', 12), t('New Flow'))
    ),
    el(
      'div.sidebar-list',
      {},
      ...state.workflows.map((w) =>
        el(
          'button',
          {
            type: 'button',
            class: `wf-item${state.view === 'workflows' && state.activeWorkflowId === w.id ? ' active' : ''}`,
            onclick: () => {
              state.activeWorkflowId = w.id;
              state.selectedKey = 'trigger';
              state.view = 'workflows';
              state.latestRun = state.runs.find((r) => r.flow_id === w.id) || null;
              render();
            },
          },
          el('span.truncate', { style: 'flex:1' }, w.name || w.id),
          w.trigger?.type && w.trigger.type !== 'manual' ? el('span.pill.pill-warn', {}, w.trigger.type) : null
        )
      )
    ),
    el(
      'div.nav-section-label',
      {},
      el('span', {}, t('Connected Services')),
      el('button.btn.btn-sm', { type: 'button', title: t('+ Add Service'), onclick: openAddServiceModal }, icon('plus', 12))
    ),
    el(
      'div.sidebar-list',
      {},
      ...state.services.map((s) =>
        el(
          'div.svc-row',
          {},
          el(
            'button.svc-item',
            { type: 'button', onclick: () => openOrFocusService(s.id) },
            el('span', { class: `dot${s.open ? ' open' : ''}` }),
            el('span.truncate', { style: 'flex:1' }, t(s.label)),
            s.configuredContainer && s.configuredContainer !== 'personal'
              ? el('span.pill', { style: 'font-size:10px;padding:1px 5px' }, s.configuredContainer)
              : null,
            el('span.muted', { style: 'font-size:11px' }, s.open ? t('Open') : t('Closed'))
          ),
          s.dynamic
            ? el('button.btn.btn-sm.btn-danger', { type: 'button', title: t('Remove service'), onclick: () => removeDynamicService(s) }, icon('x', 11))
            : null
        )
      )
    ),
    el(
      'div.sidebar-foot',
      {},
      el(
        'select.select',
        {
          style: 'flex:1;min-width:110px',
          'aria-label': t('Language'),
          onchange: async (e) => {
            state.meta.language = e.target.value;
            setLanguage(e.target.value);
            await saveMeta();
            render();
          },
        },
        ...SUPPORTED_LANGUAGES.map((l) => el('option', { value: l.id, selected: l.id === getLanguagePreference() }, l.id === 'system' ? t('System') : l.label))
      ),
      el(
        'button.btn.btn-sm',
        {
          type: 'button',
          onclick: async () => {
            state.meta.theme = state.meta.theme === 'dark' ? 'light' : 'dark';
            applyTheme(state.meta.theme);
            await saveMeta();
          },
        },
        t('Theme')
      )
    )
  );

  const main = el('main.main-pane');
  if (state.view === 'workflows') {
    canvasEl = el('div.canvas-col');
    inspectorEl = el('aside.inspector');
    main.appendChild(el('div.editor-body', {}, canvasEl, inspectorEl));
    renderEditorOnly();
  } else {
    canvasEl = null;
    inspectorEl = null;
    if (state.view === 'triggers') {
      renderTriggersView(main, {
        triggers: combinedTriggers,
        onOpenFlow: (flowId) => {
          state.activeWorkflowId = flowId;
          state.view = 'workflows';
          state.selectedKey = 'trigger';
          render();
        },
        onToggleTrigger: async (id, enabled) => {
          const isClient = state.clientTriggers.some((ct) => ct.id === id);
          if (isClient) {
            const wf = state.workflows.find((w) => triggerIdForFlow(w.id) === id || w.trigger?.id === id);
            if (wf) {
              wf.trigger.enabled = enabled;
              await persistWorkflow(wf);
            }
          } else {
            await bridge()?.triggers?.setEnabled(id, enabled);
            await refreshTriggers();
          }
        },
        onDeleteTrigger: async (id) => {
          const isClient = state.clientTriggers.some((ct) => ct.id === id);
          if (isClient) {
            const wf = state.workflows.find((w) => triggerIdForFlow(w.id) === id || w.trigger?.id === id);
            if (wf) {
              wf.trigger.type = 'manual';
              await persistWorkflow(wf);
            }
          } else {
            await bridge()?.triggers?.delete(id);
            await refreshTriggers();
          }
        },
      });
    } else if (state.view === 'events') {
      renderEventsView(main, {
        events: state.events,
        filterType: state.filterEventType,
        onFilterType: (tp) => { state.filterEventType = tp; render(); },
        onCreateTriggerFromEvent: createTriggerFromLiveEvent,
      });
    } else if (state.view === 'runs') {
      renderRunsView(main, {
        runs: state.runs,
        onRefreshRuns: refreshRuns,
        onCancelRun: async (runId) => {
          if (String(runId || '').startsWith('pipe-')) cancelActiveRun();
          else await bridge()?.flows?.cancelRun(runId);
          await refreshRuns();
        },
        onOpenFlow: (flowId) => { state.activeWorkflowId = flowId; state.view = 'workflows'; render(); },
      });
    } else if (state.view === 'templates') {
      renderTemplatesView(main, {
        services: state.services,
        customActions: state.customActions,
        workspacePacks: state.workspacePacks,
        onInstallTemplate: (tid) => installTemplate(tid, true),
        onRecordAction: () => openRecorderModal({ asSiteAction: true }),
        onExportPack: () => exportFlowPack(null),
        onImportPack: () => openImportPackModal(''),
        onOpenWorkspacePack: async (relPath) => {
          try {
            const res = await bridge()?.fs?.readText(relPath);
            openImportPackModal(res?.content || '');
          } catch (err) {
            toast(err?.message || 'Failed reading pack file', { tone: 'danger' });
          }
        },
        onSaveCustomAction: async (def) => {
          state.customActions = [def, ...state.customActions.filter((c) => c.id !== def.id)];
          await saveCustomActionsKV();
          toast(t('Custom site action saved'));
          render();
        },
        onDeleteCustomAction: async (id) => {
          state.customActions = state.customActions.filter((c) => c.id !== id);
          await saveCustomActionsKV();
          toast(t('Custom site action deleted'));
          render();
        },
      });
    } else if (state.view === 'secrets') {
      renderSecretsView(main, {
        secrets: state.secrets,
        onSaveSecret: async (name, value) => {
          if (!secretStore) return;
          try {
            await secretStore.set(name, value);
            state.secrets = await secretStore.list();
            toast(t('Secret "{name}" saved', { name }));
            render();
          } catch (err) {
            toast(err?.message || 'Failed saving secret', { tone: 'danger' });
          }
        },
        onDeleteSecret: async (name) => {
          if (!secretStore) return;
          await secretStore.remove(name);
          state.secrets = await secretStore.list();
          toast(t('Secret deleted'));
          render();
        },
      });
    }
  }

  root.replaceChildren(el('div.studio-shell', {}, sidebar, main));
}

function registerWebMCPTools() {
  const mc = typeof navigator !== 'undefined' ? navigator.modelContext : null;
  if (!mc || typeof mc.registerTool !== 'function') return;
  const tools = [
    {
      name: 'flowstudio_list_workflows',
      description: 'List configured browser automation workflows in Flow Studio.',
      inputSchema: { type: 'object', properties: {}, additionalProperties: false },
      execute: async () => ({ workflows: state.workflows.map((w) => ({ id: w.id, name: w.name, serviceId: w.serviceId, triggerType: w.trigger?.type || 'manual', steps: w.steps?.length || 0 })) }),
    },
    {
      name: 'flowstudio_list_site_actions',
      description: 'List built-in and custom Site Actions available across configured services.',
      inputSchema: { type: 'object', properties: {}, additionalProperties: false },
      execute: async () => ({ siteActions: getAllSiteActions(state.customActions).map((a) => ({ id: a.id, name: a.name, serviceId: a.serviceId, builtin: a.builtin })) }),
    },
    {
      name: 'flowstudio_list_triggers',
      description: 'List active browser event and interval triggers.',
      inputSchema: { type: 'object', properties: {}, additionalProperties: false },
      execute: async () => {
        await refreshTriggers();
        return { triggers: allCombinedTriggers() };
      },
    },
    {
      name: 'flowstudio_list_runs',
      description: 'List recent automation workflow runs.',
      inputSchema: { type: 'object', properties: {}, additionalProperties: false },
      execute: async () => ({ runs: state.runs }),
    },
    {
      name: 'flowstudio_list_services',
      description: 'List built-in and dynamic services with tab status and containers.',
      inputSchema: { type: 'object', properties: {}, additionalProperties: false },
      execute: async () => {
        await refreshServices();
        return { services: state.services };
      },
    },
    {
      name: 'flowstudio_list_secrets',
      description: 'List stored secret names and metadata (secret values are never exposed).',
      inputSchema: { type: 'object', properties: {}, additionalProperties: false },
      execute: async () => ({ secrets: state.secrets.map(({ name, updatedAt, scheme }) => ({ name, updatedAt, scheme })) }),
    },
    {
      name: 'flowstudio_list_packs',
      description: 'List exported .flowpack.json bundles inside packs/.',
      inputSchema: { type: 'object', properties: {}, additionalProperties: false },
      execute: async () => {
        await refreshWorkspacePacks();
        return { packs: state.workspacePacks };
      },
    },
    {
      name: 'flowstudio_export_pack',
      description: 'Export a sanitized portable .flowpack.json bundle with SHA-256 digest to packs/.',
      inputSchema: {
        type: 'object',
        properties: { flowId: { type: 'string' } },
        additionalProperties: false,
      },
      execute: async ({ flowId } = {}) => {
        const wf = flowId ? state.workflows.find((w) => w.id === flowId) : null;
        if (flowId && !wf) throw new Error(`Workflow '${flowId}' not found`);
        const res = await exportFlowPack(wf);
        return { path: res?.path, packId: res?.pack?.packId, integrity: res?.pack?.integrity };
      },
    },
    {
      name: 'flowstudio_run_workflow',
      description: 'Queue and execute a saved workflow across its configured service tabs and local steps.',
      inputSchema: {
        type: 'object',
        properties: { flowId: { type: 'string' }, startFromStep: { type: 'number' } },
        required: ['flowId'],
        additionalProperties: false,
      },
      execute: async ({ flowId, startFromStep = 0 }) => {
        const wf = state.workflows.find((w) => w.id === flowId);
        if (!wf) throw new Error(`Workflow '${flowId}' not found`);
        const run = await enqueueWorkflowRun(wf, {
          source: 'webmcp',
          startFromStep: Math.max(0, Number(startFromStep) || 0),
          seedOutputs: state.latestRun?.flow_id === flowId ? (state.latestRun?.outputs || {}) : {},
        });
        return { run };
      },
    },
  ];
  for (const tool of tools) {
    try { mc.registerTool(tool); } catch {}
  }
}

async function boot() {
  const b = bridge();
  secretStore = b ? createSecretStore(b) : null;
  let kvWorkflows = [];
  if (b?.kv) {
    try {
      const [metaRes, wfRes, caRes, runsRes, secList] = await Promise.all([
        b.kv.get(META_KV_KEY),
        b.kv.get(WORKFLOWS_KV_KEY),
        b.kv.get(CUSTOM_ACTIONS_KV_KEY),
        b.kv.get(PIPELINE_RUNS_KV_KEY),
        secretStore ? secretStore.list() : Promise.resolve([]),
      ]);
      if (metaRes?.found && metaRes.value) state.meta = { ...state.meta, ...metaRes.value };
      if (wfRes?.found && Array.isArray(wfRes.value)) kvWorkflows = wfRes.value;
      if (caRes?.found && Array.isArray(caRes.value)) state.customActions = caRes.value;
      if (runsRes?.found && Array.isArray(runsRes.value)) state.pipelineRuns = runsRes.value;
      if (Array.isArray(secList)) state.secrets = secList;
    } catch {}
  }
  applyTheme(state.meta.theme || 'dark');
  setLanguage(state.meta.language || 'system');

  if (b) {
    const [svcRes, listRes, trigRes, runRes] = await Promise.all([
      b.tabs?.list().catch(() => ({ services: [] })),
      b.flows?.list().catch(() => ({ flows: [] })),
      b.triggers?.list().catch(() => ({ triggers: [] })),
      b.flows?.listRuns({ limit: 25 }).catch(() => ({ runs: [] })),
    ]);

    state.services = svcRes?.services || [];
    state.triggers = trigRes?.triggers || [];
    state.runs = mergedRuns(runRes?.runs || []);

    const fullFlows = [];
    for (const s of listRes?.flows || []) {
      const full = await b.flows.get(s.id).catch(() => null);
      if (full?.flow) fullFlows.push(full.flow);
    }
    state.workflows = mergeLoadedState(fullFlows, state.triggers, state.meta.workflows, kvWorkflows);

    if (!state.meta.bootstrapped) {
      state.meta.bootstrapped = true;
      await saveMeta();
      await installTemplate('gmail-inbox-digest', false);
      await installTemplate('gmail-inbox-to-export', false);
      await installTemplate('gmail-sync-watcher', false);
    }
    syncClientTriggers();
    await refreshWorkspacePacks();
    state.activeWorkflowId = state.workflows[0]?.id || null;
    state.latestRun = state.runs.find((r) => r.flow_id === state.activeWorkflowId) || null;

    b.tabs?.onEvent((ev) => {
      state.events = [ev, ...state.events].slice(0, 120);
      evaluateClientEventTriggers(ev);
      if (state.view === 'events') render();
    });
    window.addEventListener('focus', refreshServices);
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible') refreshServices();
    });
  }

  window.addEventListener('ladybird:webmcp-ready', registerWebMCPTools);
  registerWebMCPTools();
  render();
}

boot();
