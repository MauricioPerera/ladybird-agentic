/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { parseCronExpression } from './cron.js';
import { compileSiteAction, findSiteAction } from './site-packs.js';

export const STEP_ACTIONS = [
  { id: 'open', label: 'Open URL', category: 'nav', needsUrl: true },
  { id: 'wait_for', label: 'Wait for element', category: 'nav', needsLocator: true },
  { id: 'reload', label: 'Reload tab', category: 'nav' },
  { id: 'sleep', label: 'Delay (ms)', category: 'nav', needsMs: true },
  { id: 'click', label: 'Click element', category: 'act', needsLocator: true },
  { id: 'fill', label: 'Fill input', category: 'act', needsLocator: true, needsText: true },
  { id: 'press', label: 'Press key', category: 'act', needsKey: true },
  { id: 'extract', label: 'Extract text/attr', category: 'data', needsLocator: true, supportsSaveAs: true },
  { id: 'eval', label: 'Run JS (tab)', category: 'data', needsScript: true, supportsSaveAs: true },
  { id: 'assert_text', label: 'Assert text', category: 'data', needsLocator: true, needsMatch: true },
  { id: 'assert_url', label: 'Assert URL', category: 'data', needsMatch: true },
  { id: 'assert_visible', label: 'Assert visible', category: 'data', needsLocator: true },
  { id: 'screenshot', label: 'Screenshot', category: 'data', supportsSaveAs: true },
];

export const LOCAL_NODE_KINDS = [
  { id: 'transform', label: 'Transform JS (Local)', category: 'local' },
  { id: 'condition', label: 'Condition / Filter (IF)', category: 'local' },
  { id: 'foreach', label: 'For Each Loop', category: 'local' },
  { id: 'http', label: 'HTTP / Webhook Request', category: 'local' },
  { id: 'save_file', label: 'Save Workspace File', category: 'local' },
];

export const RUN_WHEN_MODES = [
  { id: 'always', label: 'Always (Main / Merge)' },
  { id: 'if_true', label: 'Only if IF = True' },
  { id: 'if_false', label: 'Only if IF = False' },
];

export const ON_ERROR_MODES = [
  { id: 'stop', label: 'Stop workflow' },
  { id: 'continue', label: 'Continue on error' },
];

export const CONDITION_FALSE_MODES = [
  { id: 'stop', label: 'Stop workflow' },
  { id: 'branch', label: 'Branch (IF / ELSE)' },
];

export const LOCATOR_KINDS = [
  { id: 'css', label: 'CSS selector' },
  { id: 'role', label: 'ARIA role' },
  { id: 'label', label: 'Form label' },
  { id: 'placeholder', label: 'Placeholder' },
  { id: 'text', label: 'Visible text' },
  { id: 'test_id', label: 'data-testid' },
  { id: 'xpath', label: 'XPath' },
];

export const TRIGGER_TYPES = [
  { id: 'manual', label: 'Manual run' },
  { id: 'interval', label: 'Every interval (cron)' },
  { id: 'cron', label: 'Cron schedule (5-field)' },
  { id: 'webhook', label: 'Incoming Webhook (HTTP POST)' },
  { id: 'network', label: 'On service network call' },
  { id: 'notification', label: 'On browser notification' },
  { id: 'navigation', label: 'On tab navigation' },
];

export const CONDITION_OPERATORS = [
  { id: 'not_empty', label: 'Is not empty' },
  { id: 'contains', label: 'Contains text' },
  { id: 'equals', label: 'Equals' },
  { id: 'gt', label: 'Greater than (>)' },
  { id: 'lt', label: 'Less than (<)' },
  { id: 'regex', label: 'Matches RegExp' },
];

export const ACTION_META_MAP = Object.fromEntries(STEP_ACTIONS.map((a) => [a.id, a]));
export const LOCAL_META_MAP = Object.fromEntries(LOCAL_NODE_KINDS.map((k) => [k.id, k]));

export function sanitizeId(raw, fallback = 'flow-1') {
  const clean = String(raw || '')
    .toLowerCase()
    .replace(/[^a-z0-9_-]+/g, '-')
    .replace(/^[^a-z0-9]+/, '')
    .replace(/-+/g, '-')
    .replace(/[-_]+$/, '')
    .slice(0, 56);
  return clean || fallback;
}

export function createBlankStep(action = 'eval', serviceId = '') {
  const meta = ACTION_META_MAP[action] || ACTION_META_MAP.eval;
  const base = { kind: 'step', action, label: meta.label, save_as: '', ...(serviceId ? { serviceId } : {}) };
  if (action === 'open') return { ...base, url: 'https://mail.google.com/mail/u/0/#inbox', wait_for_load: true };
  if (action === 'wait_for' || action === 'assert_visible' || action === 'click' || action === 'extract') {
    return { ...base, locator: { kind: 'css', value: 'body', exact: false }, ...(action === 'extract' ? { save_as: 'extracted' } : {}) };
  }
  if (action === 'fill') return { ...base, locator: { kind: 'css', value: 'input', exact: false }, text: '', clear: true };
  if (action === 'press') return { ...base, key: 'Enter' };
  if (action === 'assert_text') return { ...base, locator: { kind: 'css', value: 'body', exact: false }, contains: '' };
  if (action === 'assert_url') return { ...base, contains: 'https://' };
  if (action === 'sleep') return { ...base, ms: 1500 };
  if (action === 'eval') {
    return {
      ...base,
      script: 'return {\n  url: location.href,\n  title: document.title\n};',
      save_as: 'result',
    };
  }
  return base;
}

export function createSiteActionNode(actionId, customActions = []) {
  const def = findSiteAction(actionId, customActions);
  const params = {};
  for (const f of def?.fields || []) params[f.key] = f.default ?? '';
  return {
    kind: 'site_action',
    siteActionId: actionId,
    serviceId: def?.serviceId || 'mail',
    label: def?.name || actionId,
    params,
    save_as: def?.defaultSaveAs || 'result',
  };
}

export function createLocalNode(kind = 'transform') {
  const reliability = { runWhen: 'always', retryCount: 0, retryDelayMs: 1500, onError: 'stop' };
  if (kind === 'condition') {
    return {
      kind: 'condition',
      serviceId: 'local',
      label: 'Check condition',
      path: 'inbox.count',
      operator: 'gt',
      expected: '0',
      onFalse: 'stop',
      save_as: 'check',
      ...reliability,
    };
  }
  if (kind === 'foreach') {
    return {
      kind: 'foreach',
      serviceId: 'local',
      label: 'For Each Item',
      itemsPath: 'inbox.rows',
      maxItems: 10,
      save_as: 'loop_results',
      bodyStep: createLocalNode('transform'),
      ...reliability,
    };
  }
  if (kind === 'http') {
    return {
      kind: 'http',
      serviceId: 'local',
      label: 'HTTP Request',
      method: 'POST',
      url: 'http://127.0.0.1:8080/webhook',
      headersJson: '{"Content-Type":"application/json"}',
      body: '{"source":"flow-studio","data":{{inbox}}}',
      save_as: 'http_res',
      ...reliability,
    };
  }
  if (kind === 'save_file') {
    return {
      kind: 'save_file',
      serviceId: 'local',
      label: 'Save File',
      path: 'exports/output.json',
      content: '{{last}}',
      save_as: 'saved_file',
      ...reliability,
    };
  }
  return {
    kind: 'transform',
    serviceId: 'local',
    label: 'Transform JS',
    code: 'return {\n  count: (input && input.count) || 0,\n  processedAt: new Date().toISOString(),\n  summary: input\n};',
    save_as: 'transformed',
    ...reliability,
  };
}

export function createBlankTrigger(serviceId = 'mail') {
  return {
    type: 'manual',
    enabled: true,
    intervalMs: 60000,
    cronExpr: '0 9 * * 1-5',
    webhookSecret: '',
    cooldownMs: 5000,
    urlContains: '',
    method: '',
    statusCode: '',
    titleContains: '',
    bodyContains: '',
    openIfClosed: true,
    activate: false,
    closeOnFinish: false,
    lastFiredAtMs: 0,
    lastRunId: null,
    fireCount: 0,
    serviceId,
  };
}

export function createBlankWorkflow(serviceId = 'mail', name = 'New Automation') {
  const slug = sanitizeId(`${serviceId}-${Date.now().toString(36)}`, 'workflow-1');
  return {
    id: slug,
    name,
    description: '',
    serviceId,
    default_timeout_ms: 12000,
    variables: {},
    trigger: createBlankTrigger(serviceId),
    steps: [createBlankStep('eval')],
    nodePositions: {},
    pinnedOutputs: {},
  };
}

export function normalizeStep(step) {
  const s = { ...(step || {}) };
  if (!s.kind) {
    if (s.siteActionId) s.kind = 'site_action';
    else if (['transform', 'condition', 'foreach', 'http', 'save_file'].includes(s.action)) s.kind = s.action;
    else s.kind = 'step';
  }
  s.runWhen = s.runWhen === 'if_true' || s.runWhen === 'if_false' ? s.runWhen : 'always';
  s.retryCount = Math.max(0, Math.min(3, Math.floor(Number(s.retryCount) || 0)));
  s.retryDelayMs = Math.max(250, Math.min(15000, Number(s.retryDelayMs) || 1500));
  s.onError = s.onError === 'continue' ? 'continue' : 'stop';
  s.requireApproval = Boolean(s.requireApproval);

  if (s.kind === 'step') {
    s.action = s.action || 'eval';
    if (s.locator) {
      const loc = {
        kind: s.locator.kind || 'css',
        value: s.locator.value || '',
        role: s.locator.role || '',
        name: s.locator.name || '',
        exact: Boolean(s.locator.exact),
      };
      if (s.locator.nth !== undefined && s.locator.nth !== null && s.locator.nth !== '') {
        loc.nth = Number(s.locator.nth);
      }
      s.locator = loc;
    }
  } else if (s.kind === 'site_action') {
    s.params = s.params && typeof s.params === 'object' ? { ...s.params } : {};
  } else if (s.kind === 'condition') {
    s.onFalse = s.onFalse === 'branch' ? 'branch' : 'stop';
  } else if (s.kind === 'foreach') {
    s.maxItems = Math.max(1, Math.min(50, Math.floor(Number(s.maxItems) || 10)));
    const inner = normalizeStep(s.bodyStep || createLocalNode('transform'));
    if (inner.kind === 'foreach') {
      s.bodyStep = createLocalNode('transform');
    } else {
      s.bodyStep = inner;
    }
  }
  return s;
}

export function isLocalNode(step) {
  return ['transform', 'condition', 'foreach', 'http', 'save_file'].includes(step?.kind);
}

export function isSingleServiceNativeFlow(wf, customActions = []) {
  if (!wf?.steps?.length) return false;
  if (wf.trigger?.type === 'cron') return false;
  if (wf.pinnedOutputs && Object.keys(wf.pinnedOutputs).length > 0) return false;
  if (extractTemplateVars(wf).some((v) => v.startsWith('secrets.'))) return false;
  for (const raw of wf.steps) {
    const s = normalizeStep(raw);
    if (isLocalNode(s)) return false;
    if (s.retryCount > 0 || s.onError === 'continue' || s.runWhen !== 'always' || s.requireApproval) return false;
    const nodeService = s.serviceId || wf.serviceId;
    if (s.kind === 'site_action') {
      const def = findSiteAction(s.siteActionId, customActions);
      if (!def || (def.serviceId && def.serviceId !== wf.serviceId)) return false;
    } else if (nodeService !== wf.serviceId) {
      return false;
    }
  }
  return true;
}

export function compilePrimitiveStep(raw) {
  const s = normalizeStep(raw);
  const out = { action: s.action };
  if (s.label) out.label = String(s.label).trim();
  if (s.url !== undefined && s.url !== null && String(s.url).trim() !== '') out.url = String(s.url).trim();
  if (s.locator) {
    const loc = { kind: s.locator.kind || 'css', exact: Boolean(s.locator.exact) };
    if (loc.kind === 'role') {
      loc.role = String(s.locator.role || 'button').trim() || 'button';
      if (s.locator.name) loc.name = String(s.locator.name);
    } else {
      loc.value = String(s.locator.value || 'body').trim() || 'body';
    }
    if (s.locator.nth !== undefined && s.locator.nth !== null && !Number.isNaN(Number(s.locator.nth))) {
      loc.nth = Math.max(0, Math.floor(Number(s.locator.nth)));
    }
    out.locator = loc;
  }
  if (s.text !== undefined && s.action === 'fill') out.text = String(s.text ?? '');
  if (s.clear !== undefined && s.action === 'fill') out.clear = Boolean(s.clear);
  if (s.key && s.action === 'press') out.key = String(s.key);
  if (s.contains && (s.action === 'assert_text' || s.action === 'assert_url')) out.contains = String(s.contains);
  if (s.equals && (s.action === 'assert_text' || s.action === 'assert_url')) out.equals = String(s.equals);
  if (s.wait_for_load !== undefined && (s.action === 'open' || s.action === 'reload')) out.wait_for_load = Boolean(s.wait_for_load);
  if (s.script && s.action === 'eval') out.script = String(s.script);
  if (s.save_as) out.save_as = String(s.save_as).trim();
  if (s.timeout_ms) out.timeout_ms = Math.max(500, Math.min(60000, Number(s.timeout_ms)));
  if (s.action === 'sleep') out.ms = Math.max(50, Math.min(30000, Number(s.ms) || 1000));
  return out;
}

export function buildFlowDefinition(wf, customActions = []) {
  const id = sanitizeId(wf?.id || wf?.name, 'flow-1');
  const compiledSteps = [];
  for (const raw of wf?.steps || []) {
    const s = normalizeStep(raw);
    if (isLocalNode(s)) continue;
    if (s.kind === 'site_action') {
      const compiled = compileSiteAction(s.siteActionId, s.params || {}, s.save_as || '', customActions);
      if (compiled?.steps?.length) {
        for (const inner of compiled.steps) compiledSteps.push(compilePrimitiveStep(inner));
      }
    } else {
      compiledSteps.push(compilePrimitiveStep(s));
    }
  }
  if (!compiledSteps.length) compiledSteps.push({ action: 'sleep', ms: 50 });

  return {
    schema_version: 1,
    id,
    name: String(wf?.name || id).trim() || id,
    description: String(wf?.description || '').trim(),
    source: 'user',
    variables: wf?.variables && typeof wf.variables === 'object' ? wf.variables : {},
    default_timeout_ms: Math.max(1000, Math.min(60000, Number(wf?.default_timeout_ms) || 12000)),
    steps: compiledSteps,
  };
}

export function triggerIdForFlow(flowId) {
  return sanitizeId(`trig-${flowId}`, 'trig-flow');
}

export function buildTriggerPayload(wf, { clientHandled = false } = {}) {
  const tCfg = wf?.trigger;
  if (!tCfg || !tCfg.type || tCfg.type === 'manual') return null;
  const flowId = sanitizeId(wf.id || wf.name, 'flow-1');
  const payload = {
    id: tCfg.id || triggerIdForFlow(flowId),
    name: String(wf.name || flowId).trim(),
    enabled: tCfg.enabled !== false,
    type: tCfg.type,
    serviceId: wf.serviceId || 'mail',
    flowId,
    intervalMs: Math.max(5000, Number(tCfg.intervalMs) || 60000),
    cronExpr: tCfg.cronExpr ? String(tCfg.cronExpr).trim() : '0 9 * * 1-5',
    cooldownMs: Math.max(500, Number(tCfg.cooldownMs) || 5000),
    openIfClosed: Boolean(tCfg.openIfClosed),
    activate: Boolean(tCfg.activate),
    closeOnFinish: Boolean(tCfg.closeOnFinish),
    clientHandled: Boolean(clientHandled || tCfg.clientHandled),
  };
  if (tCfg.webhookSecret) payload.webhookSecret = String(tCfg.webhookSecret);
  if (tCfg.urlContains) payload.urlContains = String(tCfg.urlContains);
  if (tCfg.method) payload.method = String(tCfg.method).toUpperCase();
  if (tCfg.statusCode !== '' && tCfg.statusCode !== undefined && tCfg.statusCode !== null) {
    const code = Number(tCfg.statusCode);
    if (code >= 100 && code <= 599) payload.statusCode = code;
  }
  if (tCfg.titleContains) payload.titleContains = String(tCfg.titleContains);
  if (tCfg.bodyContains) payload.bodyContains = String(tCfg.bodyContains);
  return payload;
}

function getByPath(root, parts) {
  let cur = root;
  for (const p of parts) {
    if (cur === null || cur === undefined) return undefined;
    cur = cur[p];
  }
  return cur;
}

export function resolveDotPath(ctx, rawPath) {
  const path = String(rawPath || '').trim();
  if (!path) return undefined;
  const parts = path.split('.');
  if (parts[0] === 'secrets') return parts.length === 2 ? ctx?.secrets?.[parts[1]] : undefined;
  if (parts[0] === 'last') return parts.length === 1 ? ctx?.last : getByPath(ctx?.last, parts.slice(1));
  if (parts[0] === 'vars') return parts.length === 1 ? ctx?.vars : getByPath(ctx?.vars, parts.slice(1));
  if (parts[0] === 'outputs') return parts.length === 1 ? ctx?.outputs : getByPath(ctx?.outputs, parts.slice(1));
  const fromOutputs = getByPath(ctx?.outputs, parts);
  if (fromOutputs !== undefined) return fromOutputs;
  const fromVars = getByPath(ctx?.vars, parts);
  if (fromVars !== undefined) return fromVars;
  return getByPath(ctx?.last, parts);
}

export function interpolateContext(template, ctx, { jsonStrings = false } = {}) {
  if (template === null || template === undefined) return '';
  return String(template).replace(/\{\{\s*([a-zA-Z0-9_.-]+)\s*\}\}/g, (_, expr) => {
    const val = resolveDotPath(ctx, expr);
    if (val === undefined || val === null) return '';
    if (typeof val === 'object') return jsonStrings ? JSON.stringify(val, null, 2) : JSON.stringify(val);
    return String(val);
  });
}

export function extractTemplateVars(wf) {
  const vars = new Set();
  const re = /\{\{\s*([a-zA-Z0-9_.-]+)\s*\}\}/g;
  const scanStr = (v) => {
    if (typeof v !== 'string') return;
    for (const m of v.matchAll(re)) vars.add(m[1]);
  };
  const scanStep = (step) => {
    if (!step) return;
    for (const field of [step.url, step.text, step.contains, step.equals, step.body, step.content, step.path, step.itemsPath, step.headersJson, step.script, step.locator?.value, step.locator?.name]) {
      scanStr(field);
    }
    if (step.params) Object.values(step.params).forEach(scanStr);
    if (step.bodyStep) scanStep(step.bodyStep);
  };
  for (const step of wf?.steps || []) scanStep(step);
  return [...vars];
}

function validateSingleStep(raw, num, customActions, errors, allowForeach = true) {
  const step = normalizeStep(raw);
  if (step.kind === 'site_action') {
    if (!findSiteAction(step.siteActionId, customActions)) errors.push(`Step ${num}: unknown site action '${step.siteActionId}'`);
    return;
  }
  if (step.kind === 'transform') {
    if (!String(step.code || '').trim()) errors.push(`Step ${num}: transform code is required`);
    return;
  }
  if (step.kind === 'condition') {
    if (!String(step.path || '').trim()) errors.push(`Step ${num}: condition path is required`);
    return;
  }
  if (step.kind === 'foreach') {
    if (!allowForeach) {
      errors.push(`Step ${num}: nested For Each is not supported`);
      return;
    }
    if (!String(step.itemsPath || '').trim()) errors.push(`Step ${num}: For Each items path is required`);
    if (!raw?.bodyStep) errors.push(`Step ${num}: For Each inner step is required`);
    else validateSingleStep(step.bodyStep, `${num}.1`, customActions, errors, false);
    return;
  }
  if (step.kind === 'http') {
    if (!String(step.url || '').trim()) errors.push(`Step ${num}: HTTP URL is required`);
    return;
  }
  if (step.kind === 'save_file') {
    if (!String(step.path || '').trim()) errors.push(`Step ${num}: file path is required`);
    return;
  }
  if (step.action === 'open' && !String(step.url || '').trim()) errors.push(`Step ${num}: URL is required`);
  if (step.action === 'eval' && !String(step.script || '').trim()) errors.push(`Step ${num}: script is required`);
  if (step.action === 'press' && !String(step.key || '').trim()) errors.push(`Step ${num}: key is required`);
  if (['click', 'wait_for', 'assert_visible', 'extract', 'fill', 'assert_text'].includes(step.action)) {
    if (step.locator?.kind === 'role' && !String(step.locator?.role || '').trim()) errors.push(`Step ${num}: ARIA role is required`);
    else if (step.locator?.kind !== 'role' && !String(step.locator?.value || '').trim()) errors.push(`Step ${num}: locator value is required`);
  }
  if ((step.action === 'assert_text' || step.action === 'assert_url') && !String(step.contains || '').trim() && !String(step.equals || '').trim()) {
    errors.push(`Step ${num}: contains or equals pattern is required`);
  }
}

export function validateWorkflow(wf, customActions = []) {
  const errors = [];
  if (!wf?.name?.trim()) errors.push('Workflow name is required');
  if (!wf?.serviceId) errors.push('Target service is required');
  if (wf?.trigger?.type === 'cron') {
    const parsedCron = parseCronExpression(wf.trigger.cronExpr || '');
    if (!parsedCron.ok) errors.push(parsedCron.error || 'Invalid cron expression');
  }
  if (!Array.isArray(wf?.steps) || wf.steps.length === 0) errors.push('At least one step is required');
  (wf?.steps || []).forEach((raw, idx) => validateSingleStep(raw, idx + 1, customActions, errors, true));
  return errors;
}

export function mergeLoadedState(flowDefs = [], triggers = [], metaMap = {}, kvWorkflows = []) {
  const trigByFlow = new Map();
  for (const tr of triggers || []) {
    if (tr?.flowId && !trigByFlow.has(tr.flowId)) trigByFlow.set(tr.flowId, tr);
  }
  const byId = new Map();

  for (const kw of kvWorkflows || []) {
    if (!kw?.id) continue;
    const tr = trigByFlow.get(kw.id);
    const serviceId = tr?.serviceId || kw.serviceId || 'mail';
    byId.set(kw.id, {
      ...kw,
      serviceId,
      steps: (kw.steps || []).map(normalizeStep),
      nodePositions: kw.nodePositions || {},
      pinnedOutputs: kw.pinnedOutputs && typeof kw.pinnedOutputs === 'object' ? { ...kw.pinnedOutputs } : {},
      trigger: tr ? { ...(kw.trigger || createBlankTrigger(serviceId)), ...tr } : (kw.trigger || createBlankTrigger(serviceId)),
    });
  }

  for (const f of flowDefs || []) {
    if (byId.has(f.id)) continue;
    const meta = metaMap[f.id] || {};
    const tr = trigByFlow.get(f.id);
    const serviceId = tr?.serviceId || meta.serviceId || 'mail';
    byId.set(f.id, {
      id: f.id,
      name: f.name || f.id,
      description: f.description || '',
      serviceId,
      default_timeout_ms: f.default_timeout_ms || 12000,
      variables: f.variables || {},
      steps: (f.steps || []).map(normalizeStep),
      nodePositions: meta.nodePositions || {},
      pinnedOutputs: {},
      trigger: tr ? { ...createBlankTrigger(serviceId), ...tr } : createBlankTrigger(serviceId),
    });
  }

  return [...byId.values()];
}
