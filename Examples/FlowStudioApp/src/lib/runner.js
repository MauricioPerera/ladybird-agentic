/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { HEAL_INSPECT_SCRIPT, rankHealCandidates } from './healing.js';
import { runSandboxedTransform } from './sandbox.js';
import { redactSecrets } from './secrets.js';
import { compileSiteAction } from './site-packs.js';
import {
  compilePrimitiveStep,
  interpolateContext,
  isLocalNode,
  normalizeStep,
  resolveDotPath,
  sanitizeId,
} from './workflow.js';

export function isLikelyAuthRedirect(url) {
  const s = String(url || '').toLowerCase();
  if (!s) return false;
  return [
    'accounts.google.com',
    'login.microsoftonline.com',
    'auth0.com',
    '/login',
    '/signin',
    '/sign-in',
    '/oauth',
    '/session',
    'sso.',
  ].some((pat) => s.includes(pat));
}

function abortableSleep(ms, signal) {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      const e = new Error('Run cancelled');
      e.name = 'AbortError';
      reject(e);
      return;
    }
    const timer = setTimeout(() => {
      if (signal) signal.removeEventListener('abort', onAbort);
      resolve();
    }, ms);
    function onAbort() {
      clearTimeout(timer);
      const e = new Error('Run cancelled');
      e.name = 'AbortError';
      reject(e);
    }
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

function evaluateCondition(step, ctx) {
  const actual = resolveDotPath(ctx, step.path);
  const op = step.operator || 'not_empty';
  const expected = step.expected ?? '';
  if (op === 'not_empty') {
    if (Array.isArray(actual)) return actual.length > 0;
    if (actual && typeof actual === 'object') return Object.keys(actual).length > 0;
    return actual !== undefined && actual !== null && String(actual).trim() !== '';
  }
  if (op === 'gt') return Number(actual) > Number(expected);
  if (op === 'lt') return Number(actual) < Number(expected);
  const strActual = actual === undefined || actual === null ? '' : String(actual);
  if (op === 'equals') return strActual === String(expected);
  if (op === 'contains') return strActual.toLowerCase().includes(String(expected).toLowerCase());
  if (op === 'regex') {
    try { return new RegExp(String(expected), 'i').test(strActual); } catch { return false; }
  }
  return false;
}

function resolveParamsWithContext(params, ctx) {
  const out = {};
  for (const [k, v] of Object.entries(params || {})) {
    out[k] = typeof v === 'string' && v.includes('{{') ? interpolateContext(v, ctx) : v;
  }
  return out;
}

function interpolatePrimitiveStep(step, ctx) {
  const copy = compilePrimitiveStep(step);
  if (copy.url) copy.url = interpolateContext(copy.url, ctx);
  if (copy.text) copy.text = interpolateContext(copy.text, ctx);
  if (copy.contains) copy.contains = interpolateContext(copy.contains, ctx);
  if (copy.equals) copy.equals = interpolateContext(copy.equals, ctx);
  if (copy.locator) {
    if (copy.locator.value) copy.locator.value = interpolateContext(copy.locator.value, ctx);
    if (copy.locator.name) copy.locator.name = interpolateContext(copy.locator.name, ctx);
  }
  return copy;
}

function pickStepOutput(run, saveAs) {
  if (saveAs && run?.outputs && Object.prototype.hasOwnProperty.call(run.outputs, saveAs)) {
    return run.outputs[saveAs];
  }
  const steps = run?.steps || [];
  for (let i = steps.length - 1; i >= 0; i--) {
    if (steps[i]?.output !== undefined && steps[i]?.output !== null) return steps[i].output;
  }
  return run?.outputs || null;
}

function cloneJson(val) {
  if (val === undefined) return undefined;
  return JSON.parse(JSON.stringify(val));
}

async function tryCaptureFailureScreenshot(bridge, targetService) {
  if (!bridge?.tabs?.runFlow || !targetService || targetService === 'local') return null;
  try {
    const shotRes = await bridge.tabs.runFlow(targetService, {
      flow: {
        schema_version: 1,
        id: 'fail-shot',
        name: 'Failure Screenshot',
        source: 'user',
        default_timeout_ms: 5000,
        variables: {},
        steps: [{ action: 'screenshot', save_as: 'shot' }],
      },
      openIfClosed: false,
      activate: false,
      closeOnFinish: false,
      wait: true,
    });
    const out = pickStepOutput(shotRes?.run, 'shot');
    if (typeof out === 'string' && out.trim()) return out;
    if (out && typeof out === 'object') {
      const candidate = out.data_url || out.dataUrl || out.path || out.file || null;
      if (typeof candidate === 'string' && candidate.trim()) return candidate;
    }
  } catch {}
  return null;
}

async function executeBrowserStep(s, wf, ctx, deps, stepIdx, sr) {
  const {
    bridge,
    customActions = [],
    ensureServiceTabReady,
    onAuthRequired,
    onNativeRunStart,
    signal,
  } = deps;

  if (!bridge?.tabs) throw new Error('Service Tabs API unavailable');

  let targetService = s.serviceId || wf.serviceId || 'mail';
  let nativeSteps = [];
  if (s.kind === 'site_action') {
    const resolvedParams = resolveParamsWithContext(s.params, ctx);
    const compiled = compileSiteAction(s.siteActionId, resolvedParams, s.save_as || '', customActions);
    if (!compiled) throw new Error(`Unknown site action: ${s.siteActionId}`);
    targetService = s.serviceId || compiled.serviceId || targetService;
    nativeSteps = compiled.steps.map(compilePrimitiveStep);
  } else {
    nativeSteps = [interpolatePrimitiveStep(s, ctx)];
  }

  sr.serviceId = targetService;
  const firstIsOpen = nativeSteps[0]?.action === 'open';
  const openIfClosed = wf.trigger?.openIfClosed !== false;
  const activate = Boolean(wf.trigger?.activate);

  if (openIfClosed && ensureServiceTabReady) {
    try {
      await ensureServiceTabReady(targetService, activate, !firstIsOpen);
    } catch (err) {
      if (err?.code === 'AUTH_REQUIRED' && onAuthRequired) {
        const resumed = await onAuthRequired({
          serviceId: targetService,
          url: err.tabUrl || '',
          stepIndex: stepIdx,
          signal,
        });
        if (!resumed || signal?.aborted) {
          const cancelErr = new Error('Run cancelled');
          cancelErr.name = 'AbortError';
          throw cancelErr;
        }
        await ensureServiceTabReady(targetService, activate, !firstIsOpen);
      } else {
        throw err;
      }
    }
  }

  const inlineFlow = {
    schema_version: 1,
    id: sanitizeId(`${wf.id}-s${stepIdx}`, 'step-flow'),
    name: sr.label || s.label || targetService,
    source: 'user',
    default_timeout_ms: wf.default_timeout_ms || 15000,
    variables: {},
    steps: nativeSteps,
  };
  const flatVars = {};
  for (const [k, v] of Object.entries(ctx.vars || {})) {
    if (v !== undefined && v !== null && typeof v !== 'object') flatVars[k] = String(v);
  }
  if (onNativeRunStart) onNativeRunStart(inlineFlow.id);
  const res = await bridge.tabs.runFlow(targetService, {
    flow: inlineFlow,
    variables: flatVars,
    openIfClosed,
    activate,
    closeOnFinish: Boolean(wf.trigger?.closeOnFinish),
    wait: true,
  });
  const subRun = res?.run;
  if (!subRun || subRun.status === 'failed' || subRun.status === 'cancelled') {
    const err = new Error(subRun?.error || 'Step flow failed');
    err.targetService = targetService;
    throw err;
  }
  return pickStepOutput(subRun, s.save_as);
}

async function executeSingleStep(s, wf, ctx, deps, stepIdx, sr) {
  const { bridge, fetchFn = typeof fetch === 'function' ? fetch : null, signal, transformTimeoutMs = 3000 } = deps;

  if (signal?.aborted) {
    const cancelErr = new Error('Run cancelled');
    cancelErr.name = 'AbortError';
    throw cancelErr;
  }

  if (s.kind === 'transform') {
    return await runSandboxedTransform(String(s.code || 'return input;'), {
      input: ctx.last,
      outputs: ctx.outputs,
      vars: ctx.vars,
      timeoutMs: transformTimeoutMs,
      signal,
    });
  }
  if (s.kind === 'condition') {
    const matched = evaluateCondition(s, ctx);
    return { matched, path: s.path, actual: resolveDotPath(ctx, s.path) };
  }
  if (s.kind === 'http') {
    if (!fetchFn) throw new Error('fetch is unavailable');
    const targetUrl = interpolateContext(s.url, ctx).trim();
    const method = String(s.method || 'GET').toUpperCase();
    let headers = {};
    if (s.headersJson && String(s.headersJson).trim()) {
      headers = JSON.parse(interpolateContext(s.headersJson, ctx));
    }
    const opts = { method, headers, ...(signal ? { signal } : {}) };
    if (method !== 'GET' && method !== 'HEAD' && s.body !== undefined && s.body !== '') {
      opts.body = interpolateContext(s.body, ctx);
    }
    const resp = await fetchFn(targetUrl, opts);
    const text = await resp.text();
    let data = text;
    try { data = JSON.parse(text); } catch {}
    const stepOut = { status: resp.status, ok: resp.ok, data };
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    return stepOut;
  }
  if (s.kind === 'save_file') {
    if (!bridge?.fs) throw new Error('App FS bridge unavailable');
    let relPath = interpolateContext(s.path || 'exports/output.json', ctx).trim();
    if (!relPath.startsWith('exports/') && !relPath.startsWith('packs/')) relPath = `exports/${relPath.replace(/^\/+/, '')}`;
    const rawContent = s.content ? interpolateContext(s.content, ctx, { jsonStrings: true }) : JSON.stringify(ctx.last ?? ctx.outputs, null, 2);
    const res = await bridge.fs.writeText(relPath, rawContent);
    return { path: res?.path || relPath, bytes: rawContent.length };
  }
  return await executeBrowserStep(s, wf, ctx, deps, stepIdx, sr);
}

function buildApprovalPreview(s, wf, ctx, secretMap) {
  const raw = {
    action: s.kind === 'site_action' ? s.siteActionId : s.kind === 'step' ? s.action : s.kind,
    serviceId: isLocalNode(s) ? 'local' : (s.serviceId || wf.serviceId || 'mail'),
  };
  if (s.kind === 'site_action') raw.params = resolveParamsWithContext(s.params, ctx);
  else if (s.kind === 'http') {
    raw.method = s.method || 'GET';
    raw.url = interpolateContext(s.url, ctx);
    if (s.body) raw.body = interpolateContext(s.body, ctx);
  } else if (s.kind === 'save_file') {
    raw.path = interpolateContext(s.path, ctx);
  } else if (s.kind === 'step') {
    if (s.url) raw.url = interpolateContext(s.url, ctx);
    if (s.text) raw.text = interpolateContext(s.text, ctx);
    if (s.locator) raw.locator = s.locator;
  } else if (s.kind === 'foreach') {
    raw.itemsPath = s.itemsPath;
    raw.maxItems = s.maxItems;
  }
  return redactSecrets(raw, secretMap);
}

export async function runPipelineWorkflow(wf, deps = {}) {
  const {
    bridge,
    customActions = [],
    initialVars = {},
    signal = null,
    startFromStep = 0,
    seedOutputs = {},
    usePinnedData = true,
    onAuthRequired = null,
    onApprovalRequired = null,
    resolveSecrets = null,
    captureFailureScreenshot = true,
    onProgress = null,
  } = deps;

  const secretMap = resolveSecrets ? (await resolveSecrets().catch(() => ({})) || {}) : {};
  const startedAt = Date.now();
  const runId = `pipe-${startedAt}-${Math.floor(Math.random() * 1e6)}`;
  const ctx = {
    vars: { ...(wf.variables || {}), ...(initialVars || {}) },
    outputs: cloneJson(seedOutputs || {}),
    secrets: secretMap,
    last: null,
  };

  const stepRecords = (wf.steps || []).map((raw, i) => {
    const s = normalizeStep(raw);
    return {
      index: i,
      action: s.kind === 'site_action' ? s.siteActionId : s.kind === 'step' ? s.action : s.kind,
      label: s.label || s.siteActionId || s.action || s.kind,
      serviceId: isLocalNode(s) ? 'local' : (s.serviceId || wf.serviceId || 'mail'),
      status: 'pending',
      duration_ms: 0,
      attempts: 0,
      output: null,
      error: null,
      failureScreenshot: null,
      healSuggestions: [],
    };
  });

  const record = {
    run_id: runId,
    flow_id: wf.id,
    flow_name: wf.name || wf.id,
    status: 'running',
    started_at_ms: startedAt,
    finished_at_ms: null,
    duration_ms: 0,
    error: null,
    outputs: {},
    steps: stepRecords,
  };

  const safeMask = (v) => redactSecrets(v, secretMap);
  const notify = () => {
    if (onProgress) {
      onProgress({
        ...record,
        error: record.error ? safeMask(record.error) : null,
        outputs: safeMask({ ...ctx.outputs }),
        steps: stepRecords.map((x) => safeMask({ ...x })),
      });
    }
  };
  const cancelRemaining = (fromIdx, reason = 'Run cancelled') => {
    for (let j = fromIdx; j < stepRecords.length; j++) {
      if (stepRecords[j].status === 'pending' || stepRecords[j].status === 'running' || stepRecords[j].status === 'paused_auth' || stepRecords[j].status === 'paused_approval') {
        stepRecords[j].status = 'cancelled';
      }
    }
    record.status = 'cancelled';
    record.error = safeMask(reason);
    record.finished_at_ms = Date.now();
    record.duration_ms = record.finished_at_ms - startedAt;
    record.outputs = safeMask({ ...ctx.outputs });
    notify();
    return record;
  };

  notify();

  let branchState = null;
  let hadStepErrors = false;
  const startAt = Math.max(0, Math.floor(Number(startFromStep) || 0));

  for (let i = 0; i < (wf.steps || []).length; i++) {
    if (signal?.aborted) return cancelRemaining(i);

    const s = normalizeStep(wf.steps[i]);
    const sr = stepRecords[i];
    const t0 = Date.now();

    if (i < startAt) {
      const pinnedVal = (usePinnedData && wf.pinnedOutputs && Object.prototype.hasOwnProperty.call(wf.pinnedOutputs, String(i)))
        ? cloneJson(wf.pinnedOutputs[String(i)])
        : (s.save_as && ctx.outputs[s.save_as] !== undefined ? ctx.outputs[s.save_as] : null);
      if (s.save_as && pinnedVal !== null && pinnedVal !== undefined) ctx.outputs[s.save_as] = pinnedVal;
      if (pinnedVal !== null && pinnedVal !== undefined) ctx.last = pinnedVal;
      sr.output = pinnedVal;
      sr.status = 'cached';
      if (s.kind === 'condition' && s.onFalse === 'branch' && pinnedVal && typeof pinnedVal === 'object' && 'matched' in pinnedVal) {
        branchState = pinnedVal.matched ? 'true' : 'false';
      }
      notify();
      continue;
    }

    if (s.runWhen === 'if_true' && branchState !== 'true') {
      sr.status = 'skipped';
      sr.output = { skippedReason: 'branch_false' };
      notify();
      continue;
    }
    if (s.runWhen === 'if_false' && branchState !== 'false') {
      sr.status = 'skipped';
      sr.output = { skippedReason: 'branch_true' };
      notify();
      continue;
    }
    if (s.runWhen === 'always' && s.kind !== 'condition' && branchState !== null) {
      branchState = null;
    }

    if (usePinnedData && wf.pinnedOutputs && Object.prototype.hasOwnProperty.call(wf.pinnedOutputs, String(i))) {
      const pinnedVal = cloneJson(wf.pinnedOutputs[String(i)]);
      if (s.save_as) ctx.outputs[s.save_as] = pinnedVal;
      ctx.last = pinnedVal;
      sr.output = safeMask(pinnedVal);
      sr.status = 'pinned';
      sr.duration_ms = 0;
      if (s.kind === 'condition' && s.onFalse === 'branch' && pinnedVal && typeof pinnedVal === 'object' && 'matched' in pinnedVal) {
        branchState = pinnedVal.matched ? 'true' : 'false';
      }
      notify();
      continue;
    }

    if (s.requireApproval && onApprovalRequired) {
      sr.status = 'paused_approval';
      record.status = 'paused_approval';
      notify();
      const preview = buildApprovalPreview(s, wf, ctx, secretMap);
      const approved = await onApprovalRequired({
        stepIndex: i,
        label: sr.label,
        serviceId: sr.serviceId,
        action: sr.action,
        preview,
        signal,
      });
      if (!approved || signal?.aborted) {
        sr.status = 'rejected';
        return cancelRemaining(i + 1, 'Step rejected by user');
      }
      record.status = 'running';
    }

    sr.status = 'running';
    notify();

    const wrappedDeps = {
      ...deps,
      customActions,
      onAuthRequired: onAuthRequired
        ? async (info) => {
            sr.status = 'paused_auth';
            record.status = 'paused_auth';
            notify();
            const ok = await onAuthRequired(info);
            if (ok && !signal?.aborted) {
              sr.status = 'running';
              record.status = 'running';
              notify();
            }
            return ok;
          }
        : null,
    };

    const maxAttempts = 1 + (s.retryCount || 0);
    let lastErr = null;
    let stepOut = null;
    let succeeded = false;

    for (let attempt = 0; attempt < maxAttempts; attempt++) {
      sr.attempts = attempt + 1;
      try {
        if (signal?.aborted) {
          const cancelErr = new Error('Run cancelled');
          cancelErr.name = 'AbortError';
          throw cancelErr;
        }

        if (s.kind === 'condition') {
          const matched = evaluateCondition(s, ctx);
          if (s.onFalse === 'branch') {
            branchState = matched ? 'true' : 'false';
            stepOut = { matched, branch: branchState, path: s.path, actual: resolveDotPath(ctx, s.path) };
            succeeded = true;
            break;
          }
          stepOut = { matched, path: s.path, actual: resolveDotPath(ctx, s.path) };
          if (s.save_as) ctx.outputs[s.save_as] = stepOut;
          ctx.last = stepOut;
          sr.output = stepOut;
          sr.duration_ms = Date.now() - t0;
          if (!matched) {
            sr.status = 'skipped';
            for (let j = i + 1; j < stepRecords.length; j++) stepRecords[j].status = 'skipped';
            record.status = 'stopped';
            record.finished_at_ms = Date.now();
            record.duration_ms = record.finished_at_ms - startedAt;
            record.outputs = { ...ctx.outputs };
            notify();
            return record;
          }
          succeeded = true;
          break;
        }

        if (s.kind === 'foreach') {
          const rawItems = resolveDotPath(ctx, s.itemsPath);
          if (!Array.isArray(rawItems)) {
            throw new Error(`For Each path '${s.itemsPath}' did not resolve to an array`);
          }
          const slice = rawItems.slice(0, s.maxItems || 10);
          const innerStep = normalizeStep(s.bodyStep);
          const results = [];
          let loopOk = 0;
          let loopFail = 0;

          for (let k = 0; k < slice.length; k++) {
            if (signal?.aborted) {
              const cancelErr = new Error('Run cancelled');
              cancelErr.name = 'AbortError';
              throw cancelErr;
            }
            const item = slice[k];
            const iterCtx = {
              vars: { ...ctx.vars, item, index: k },
              outputs: { ...ctx.outputs, item, index: k },
              last: item,
            };
            try {
              const iterOut = await executeSingleStep(innerStep, wf, iterCtx, wrappedDeps, `${i}_it${k}`, sr);
              loopOk++;
              results.push({ index: k, ok: true, output: iterOut });
            } catch (iterErr) {
              if (iterErr?.name === 'AbortError' || signal?.aborted) throw iterErr;
              loopFail++;
              if (innerStep.onError === 'continue' || s.onError === 'continue') {
                results.push({ index: k, ok: false, error: iterErr?.message || String(iterErr) });
              } else {
                throw iterErr;
              }
            }
          }
          stepOut = { total: rawItems.length, processed: slice.length, succeeded: loopOk, failed: loopFail, results };
          succeeded = true;
          break;
        }

        stepOut = await executeSingleStep(s, wf, ctx, wrappedDeps, i, sr);
        succeeded = true;
        break;
      } catch (err) {
        if (err?.name === 'AbortError' || signal?.aborted) {
          return cancelRemaining(i);
        }
        lastErr = err;
        if (attempt < maxAttempts - 1) {
          notify();
          try {
            await abortableSleep((s.retryDelayMs || 1500) * (attempt + 1), signal);
          } catch {
            return cancelRemaining(i);
          }
        }
      }
    }

    if (succeeded) {
      const maskedOut = safeMask(stepOut);
      if (s.save_as) ctx.outputs[s.save_as] = maskedOut;
      ctx.last = maskedOut;
      sr.output = maskedOut;
      sr.status = 'passed';
      sr.duration_ms = Date.now() - t0;
      notify();
      continue;
    }

    const errMsg = safeMask(lastErr?.message || String(lastErr || 'Step failed'));
    sr.status = 'failed';
    sr.error = errMsg;
    sr.duration_ms = Date.now() - t0;
    if (captureFailureScreenshot && (s.kind === 'step' || s.kind === 'site_action')) {
      sr.failureScreenshot = await tryCaptureFailureScreenshot(bridge, sr.serviceId || s.serviceId || wf.serviceId);
    }
    if (deps.captureHealSuggestions !== false && s.kind === 'step' && s.locator && bridge?.tabs?.eval) {
      try {
        const snap = await bridge.tabs.eval(sr.serviceId || s.serviceId || wf.serviceId, HEAL_INSPECT_SCRIPT);
        if (Array.isArray(snap?.value)) sr.healSuggestions = rankHealCandidates(s, snap.value);
      } catch {}
    }

    if (s.onError === 'continue') {
      hadStepErrors = true;
      const errObj = { __error: errMsg };
      if (s.save_as) ctx.outputs[s.save_as] = errObj;
      ctx.last = errObj;
      notify();
      continue;
    }

    record.status = 'failed';
    record.error = errMsg;
    record.finished_at_ms = Date.now();
    record.duration_ms = record.finished_at_ms - startedAt;
    record.outputs = safeMask({ ...ctx.outputs });
    notify();
    return record;
  }

  record.status = hadStepErrors ? 'partial' : 'passed';
  record.error = hadStepErrors ? 'Completed with step errors' : null;
  record.finished_at_ms = Date.now();
  record.duration_ms = record.finished_at_ms - startedAt;
  record.outputs = safeMask({ ...ctx.outputs });
  notify();
  return record;
}
