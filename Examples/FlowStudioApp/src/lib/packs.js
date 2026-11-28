/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { validateCustomSiteAction } from './site-packs.js';
import { stripSecretsFromExport } from './secrets.js';
import { extractTemplateVars, sanitizeId, validateWorkflow } from './workflow.js';

export const FLOWPACK_SCHEMA_VERSION = 1;

function canonicalJson(value) {
  if (Array.isArray(value)) return `[${value.map(canonicalJson).join(',')}]`;
  if (value && typeof value === 'object') {
    const keys = Object.keys(value).sort();
    return `{${keys.map((k) => `${JSON.stringify(k)}:${canonicalJson(value[k])}`).join(',')}}`;
  }
  return JSON.stringify(value);
}

function bytesToHex(u8) {
  return Array.from(u8, (b) => b.toString(16).padStart(2, '0')).join('');
}

export async function computePackDigest(pack) {
  const { integrity, ...body } = pack || {};
  const text = canonicalJson(body);
  const encoded = new TextEncoder().encode(text);
  if (globalThis.crypto?.subtle?.digest) {
    const hash = await globalThis.crypto.subtle.digest('SHA-256', encoded);
    return `sha256-${bytesToHex(new Uint8Array(hash))}`;
  }
  let h = 2166136261;
  for (let i = 0; i < encoded.length; i++) {
    h ^= encoded[i];
    h = Math.imul(h, 16777619);
  }
  return `fnv1a-${(h >>> 0).toString(16).padStart(8, '0')}`;
}

export async function buildFlowPack({
  name = 'Flow Studio Pack',
  packId = '',
  version = '1.0.0',
  author = 'Flow Studio User',
  description = '',
  workflows = [],
  customActions = [],
  services = [],
  secretMap = {},
} = {}) {
  const cleanWorkflows = stripSecretsFromExport(
    (workflows || []).map(({ _isNew, _savedId, trigger, ...rest }) => ({
      ...rest,
      trigger: trigger
        ? {
            ...trigger,
            webhookSecret: undefined,
            lastFiredAtMs: 0,
            lastRunId: null,
            fireCount: 0,
          }
        : undefined,
    })),
    secretMap
  );
  const cleanActions = stripSecretsFromExport(
    (customActions || []).filter((a) => !a.builtin),
    secretMap
  );

  const reqServiceIds = new Set();
  const reqSecrets = new Set();
  for (const wf of cleanWorkflows) {
    if (wf.serviceId && wf.serviceId !== 'local') reqServiceIds.add(wf.serviceId);
    for (const s of wf.steps || []) {
      if (s.serviceId && s.serviceId !== 'local') reqServiceIds.add(s.serviceId);
      if (s.bodyStep?.serviceId && s.bodyStep.serviceId !== 'local') reqServiceIds.add(s.bodyStep.serviceId);
    }
    for (const v of extractTemplateVars(wf)) {
      if (v.startsWith('secrets.')) reqSecrets.add(v.slice('secrets.'.length));
    }
  }
  for (const act of cleanActions) {
    if (act.serviceId && act.serviceId !== 'local') reqServiceIds.add(act.serviceId);
  }

  const svcMap = new Map((services || []).map((s) => [s.id, s]));
  const requiredServices = [];
  for (const sid of [...reqServiceIds].sort()) {
    const svc = svcMap.get(sid);
    if (!svc) continue;
    requiredServices.push({
      id: svc.id,
      label: svc.label || svc.id,
      defaultUrl: svc.defaultUrl,
      origins: Array.isArray(svc.origins) ? [...svc.origins] : [],
      container: svc.configuredContainer || svc.container || 'personal',
      permissions: ['open', 'navigate', 'notifications', 'eval', 'flows', 'network'],
    });
  }

  const body = {
    schema_version: FLOWPACK_SCHEMA_VERSION,
    packId: sanitizeId(packId || name, 'flow-pack'),
    name: String(name || 'Flow Studio Pack').trim(),
    version: String(version || '1.0.0').trim(),
    author: String(author || 'Flow Studio User').trim(),
    description: String(description || '').trim(),
    exportedAt: new Date().toISOString(),
    requiredServices,
    requiredSecrets: [...reqSecrets].sort(),
    siteActions: cleanActions,
    workflows: cleanWorkflows,
  };
  const integrity = await computePackDigest(body);
  return { ...body, integrity };
}

export async function validateFlowPack(pack) {
  const errors = [];
  if (!pack || typeof pack !== 'object') {
    return { valid: false, integrityValid: false, errors: ['Pack must be a JSON object'] };
  }
  if (pack.schema_version !== FLOWPACK_SCHEMA_VERSION) errors.push(`Unsupported schema_version: ${pack.schema_version}`);
  if (!pack.name || typeof pack.name !== 'string') errors.push('Pack requires a name');
  if (!Array.isArray(pack.siteActions)) errors.push('Pack siteActions must be an array');
  if (!Array.isArray(pack.workflows)) errors.push('Pack workflows must be an array');

  for (const svc of pack.requiredServices || []) {
    if (!svc?.id || !svc?.defaultUrl || !Array.isArray(svc.origins) || !svc.origins.length) {
      errors.push(`Invalid requiredService entry: ${svc?.id || 'unknown'}`);
      continue;
    }
    try {
      const u = new URL(svc.defaultUrl);
      if (u.protocol !== 'http:' && u.protocol !== 'https:') errors.push(`Service ${svc.id} URL must be http/https`);
    } catch {
      errors.push(`Service ${svc.id} has invalid defaultUrl`);
    }
  }

  const packActions = Array.isArray(pack.siteActions) ? pack.siteActions : [];
  for (const act of packActions) {
    for (const err of validateCustomSiteAction(act)) errors.push(`Action '${act?.id || '?'}': ${err}`);
  }
  for (const wf of Array.isArray(pack.workflows) ? pack.workflows : []) {
    for (const err of validateWorkflow(wf, packActions)) errors.push(`Workflow '${wf?.name || wf?.id || '?'}': ${err}`);
  }

  let integrityValid = null;
  if (pack.integrity) {
    const expected = await computePackDigest(pack);
    integrityValid = expected === pack.integrity;
    if (!integrityValid) errors.push('Pack integrity digest mismatch (file modified or corrupted)');
  }

  return { valid: errors.length === 0, integrityValid, errors };
}

export function diffFlowPackInstall(pack, {
  existingServices = [],
  existingSecrets = [],
  existingWorkflows = [],
  existingActions = [],
} = {}) {
  const svcIds = new Set((existingServices || []).map((s) => s.id));
  const secNames = new Set((existingSecrets || []).map((s) => typeof s === 'string' ? s : s.name));
  const wfIds = new Set((existingWorkflows || []).map((w) => w.id));
  const actIds = new Set((existingActions || []).map((a) => a.id));

  const newServices = [];
  const existingServiceIds = [];
  for (const svc of pack?.requiredServices || []) {
    if (svcIds.has(svc.id)) existingServiceIds.push(svc.id);
    else newServices.push(svc);
  }

  const missingSecrets = (pack?.requiredSecrets || []).filter((n) => !secNames.has(n));
  const newWorkflows = [];
  const overwriteWorkflows = [];
  for (const wf of pack?.workflows || []) {
    if (wfIds.has(wf.id)) overwriteWorkflows.push(wf.id);
    else newWorkflows.push(wf.id);
  }

  const newActions = [];
  const overwriteActions = [];
  for (const act of pack?.siteActions || []) {
    if (actIds.has(act.id)) overwriteActions.push(act.id);
    else newActions.push(act.id);
  }

  return {
    newServices,
    existingServiceIds,
    missingSecrets,
    newWorkflows,
    overwriteWorkflows,
    newActions,
    overwriteActions,
  };
}
