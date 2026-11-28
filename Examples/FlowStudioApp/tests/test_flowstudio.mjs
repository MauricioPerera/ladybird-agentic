/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import {
  sanitizeId,
  createBlankWorkflow,
  createBlankStep,
  createSiteActionNode,
  createLocalNode,
  buildFlowDefinition,
  buildTriggerPayload,
  extractTemplateVars,
  validateWorkflow,
  mergeLoadedState,
  resolveDotPath,
  interpolateContext,
  isSingleServiceNativeFlow,
} from '../src/lib/workflow.js';
import {
  BUILTIN_SITE_ACTIONS,
  compileSiteAction,
  createCustomSiteActionTemplate,
  validateCustomSiteAction,
} from '../src/lib/site-packs.js';
import { runPipelineWorkflow } from '../src/lib/runner.js';
import {
  runSandboxedTransform,
  TRANSFORM_FRAME_CSP,
  TRANSFORM_FRAME_SANDBOX,
} from '../src/lib/sandbox.js';
import { createSecretStore, redactSecrets, stripSecretsFromExport } from '../src/lib/secrets.js';
import { parseCronExpression, matchesCron, nextCronRuns, cronSlotKey } from '../src/lib/cron.js';
import { normalizeRecordedEvents } from '../src/lib/recorder.js';
import { buildFlowPack, validateFlowPack, diffFlowPackInstall } from '../src/lib/packs.js';
import { rankHealCandidates } from '../src/lib/healing.js';
import { SESSION_PACK_TEMPLATES, cloneTemplate } from '../src/lib/templates.js';
import { setLanguage, getLanguage, t } from '../src/lib/i18n.js';

let passed = 0;
async function test(name, fn) {
  await fn();
  passed++;
  console.log(`[PASS] ${name}`);
}

await test('sanitizeId & createBlankWorkflow', () => {
  assert.equal(sanitizeId('My Cool Flow!!'), 'my-cool-flow');
  const wf = createBlankWorkflow('mail', 'Daily Scan');
  assert.equal(wf.serviceId, 'mail');
  assert.equal(wf.steps.length, 1);
  assert.deepEqual(validateWorkflow(wf), []);
});

await test('site-packs compile built-in and custom site actions with return eval', () => {
  assert.ok(BUILTIN_SITE_ACTIONS.length >= 9);
  for (const act of BUILTIN_SITE_ACTIONS) {
    const compiled = compileSiteAction(act.id, {}, act.defaultSaveAs);
    assert.ok(compiled && compiled.steps.length >= 1, act.id);
    for (const s of compiled.steps) {
      if (s.action === 'eval') {
        assert.ok(s.script.trim().startsWith('return '), `Eval script in ${act.id} must start with return`);
      }
    }
  }

  const custom = createCustomSiteActionTemplate('mail');
  assert.deepEqual(validateCustomSiteAction(custom), []);
  const compiledCustom = compileSiteAction(custom.id, { query: 'vip@acme.com' }, 'custom_out', [custom]);
  assert.ok(compiledCustom.steps[0].script.includes('vip@acme.com'));
  assert.equal(compiledCustom.steps[0].save_as, 'custom_out');
});

await test('resolveDotPath & interpolateContext resolve nested outputs and vars', () => {
  const ctx = {
    vars: { account: 'personal' },
    outputs: { inbox: { count: 3, rows: [{ subject: 'Q4 Demo' }] } },
    last: { url: 'https://docs.google.com/d/123' },
  };
  assert.equal(resolveDotPath(ctx, 'inbox.count'), 3);
  assert.equal(resolveDotPath(ctx, 'inbox.rows.0.subject'), 'Q4 Demo');
  assert.equal(resolveDotPath(ctx, 'last.url'), 'https://docs.google.com/d/123');
  assert.equal(interpolateContext('Re: {{inbox.rows.0.subject}} ({{vars.account}})', ctx), 'Re: Q4 Demo (personal)');
});

await test('buildFlowDefinition compiles site_action into native FlowDefinition', () => {
  const wf = createBlankWorkflow('mail', 'Digest');
  wf.steps = [createSiteActionNode('gmail.read_inbox')];
  assert.equal(isSingleServiceNativeFlow(wf), true);
  const def = buildFlowDefinition(wf);
  assert.equal(def.schema_version, 1);
  assert.equal(def.steps.length, 2);
  assert.equal(def.steps[0].action, 'wait_for');
  assert.equal(def.steps[1].action, 'eval');
  assert.equal(def.steps[1].save_as, 'inbox');

  wf.steps.push(createLocalNode('transform'));
  assert.equal(isSingleServiceNativeFlow(wf), false);
});

await test('runPipelineWorkflow executes multi-service + local transform + condition + save_file', async () => {
  const writtenFiles = [];
  const runLog = [];
  const fakeBridge = {
    tabs: {
      runFlow: async (serviceId, opts) => {
        runLog.push({ serviceId, steps: opts.flow.steps.length });
        return {
          run: {
            status: 'passed',
            outputs: { inbox: { count: 2, scannedAt: '2026-11-28', rows: [{ subject: 'Invoice', sender: 'a@b.com' }] } },
            steps: [{ index: 0, status: 'passed', output: null }],
          },
        };
      },
    },
    fs: {
      writeText: async (filePath, content) => {
        writtenFiles.push({ path: filePath, content });
        return { path: filePath };
      },
    },
  };

  const wf = cloneTemplate('gmail-inbox-to-export');
  const rec = await runPipelineWorkflow(wf, { bridge: fakeBridge });
  assert.equal(rec.status, 'passed');
  assert.equal(runLog.length, 1);
  assert.equal(writtenFiles.length, 1);
  assert.equal(writtenFiles[0].path, 'exports/gmail-digest.json');
  assert.ok(writtenFiles[0].content.includes('"total": 1'));
  assert.ok(writtenFiles[0].content.includes('"a@b.com": 1'));

  const wfCond = createBlankWorkflow('mail', 'Cond Test');
  wfCond.steps = [
    createLocalNode('condition'),
    createLocalNode('save_file'),
  ];
  wfCond.steps[0].path = 'inbox.count';
  wfCond.steps[0].operator = 'gt';
  wfCond.steps[0].expected = '10';
  const stoppedRec = await runPipelineWorkflow(wfCond, { bridge: fakeBridge });
  assert.equal(stoppedRec.status, 'stopped');
  assert.equal(stoppedRec.steps[0].status, 'skipped');
  assert.equal(stoppedRec.steps[1].status, 'skipped');
  assert.equal(writtenFiles.length, 1);
});

await test('all SESSION_PACK_TEMPLATES validate cleanly', () => {
  assert.equal(SESSION_PACK_TEMPLATES.length, 7);
  for (const tpl of SESSION_PACK_TEMPLATES) {
    const wf = cloneTemplate(tpl.id);
    assert.deepEqual(validateWorkflow(wf), [], `Template ${tpl.id} validation failed`);
  }
});

await test('IF / ELSE branching & Merge node execution', async () => {
  const wf = createBlankWorkflow('mail', 'Branch Merge Test');
  wf.steps = [
    { kind: 'transform', serviceId: 'local', label: 'Init', code: 'return { score: 2 };', save_as: 'init' },
    { kind: 'condition', serviceId: 'local', label: 'IF > 5', path: 'init.score', operator: 'gt', expected: '5', onFalse: 'branch', save_as: 'cond' },
    { kind: 'transform', serviceId: 'local', label: 'True Branch', runWhen: 'if_true', code: 'return "was-true";', save_as: 'branch_out' },
    { kind: 'transform', serviceId: 'local', label: 'False Branch', runWhen: 'if_false', code: 'return "was-false";', save_as: 'branch_out' },
    { kind: 'transform', serviceId: 'local', label: 'Merge', runWhen: 'always', code: 'return { merged: outputs.branch_out };', save_as: 'final' },
  ];
  const rec = await runPipelineWorkflow(wf, { bridge: {} });
  assert.equal(rec.status, 'passed');
  assert.equal(rec.steps[1].status, 'passed');
  assert.equal(rec.steps[2].status, 'skipped');
  assert.equal(rec.steps[3].status, 'passed');
  assert.equal(rec.steps[4].status, 'passed');
  assert.deepEqual(rec.outputs.final, { merged: 'was-false' });
});

await test('For Each loop processes items and respects maxItems cap', async () => {
  const wf = createBlankWorkflow('mail', 'Loop Test');
  wf.steps = [
    { kind: 'transform', serviceId: 'local', code: 'return [{ s: "A" }, { s: "B" }, { s: "C" }];', save_as: 'list' },
    {
      kind: 'foreach',
      serviceId: 'local',
      itemsPath: 'list',
      maxItems: 2,
      save_as: 'looped',
      bodyStep: { kind: 'transform', serviceId: 'local', code: 'return `#${outputs.index}:${input.s}`;', save_as: 'it' },
    },
  ];
  const rec = await runPipelineWorkflow(wf, { bridge: {} });
  assert.equal(rec.status, 'passed');
  assert.equal(rec.outputs.looped.total, 3);
  assert.equal(rec.outputs.looped.processed, 2);
  assert.equal(rec.outputs.looped.succeeded, 2);
  assert.deepEqual(rec.outputs.looped.results.map((r) => r.output), ['#0:A', '#1:B']);
});

await test('Retry with backoff, Continue on Error, Cancellation & Pin Data', async () => {
  const wfRel = createBlankWorkflow('mail', 'Reliability');
  wfRel.steps = [
    {
      kind: 'transform',
      serviceId: 'local',
      retryCount: 1,
      retryDelayMs: 250,
      code: 'throw new Error("boom-once");',
      onError: 'continue',
      save_as: 'may_fail',
    },
    { kind: 'transform', serviceId: 'local', code: 'return { ok: true, after: outputs.may_fail };', save_as: 'next' },
  ];
  const relRec = await runPipelineWorkflow(wfRel, { bridge: {} });
  assert.equal(relRec.status, 'partial');
  assert.equal(relRec.steps[0].status, 'failed');
  assert.equal(relRec.steps[0].attempts, 2);
  assert.equal(relRec.steps[1].status, 'passed');
  assert.equal(relRec.outputs.next.ok, true);

  let liveCalls = 0;
  const wfPin = cloneTemplate('gmail-inbox-to-export');
  wfPin.pinnedOutputs = {
    '0': { count: 1, scannedAt: 'pinned-now', rows: [{ subject: 'Pinned', sender: 'pin@acme.com' }] },
  };
  const files = [];
  const pinRec = await runPipelineWorkflow(wfPin, {
    bridge: {
      tabs: { runFlow: async () => { liveCalls++; return {}; } },
      fs: { writeText: async (filePath, content) => { files.push({ path: filePath, content }); return { path: filePath }; } },
    },
  });
  assert.equal(pinRec.status, 'passed');
  assert.equal(liveCalls, 0);
  assert.equal(pinRec.steps[0].status, 'pinned');
  assert.ok(files[0].content.includes('"pin@acme.com": 1'));

  const seedRec = await runPipelineWorkflow(wfPin, {
    bridge: {
      tabs: { runFlow: async () => { liveCalls++; return {}; } },
      fs: { writeText: async (filePath, content) => { files.push({ path: filePath, content }); return { path: filePath }; } },
    },
    startFromStep: 2,
    usePinnedData: false,
    seedOutputs: { digest: { fromSeed: 42 } },
  });
  assert.equal(seedRec.status, 'passed');
  assert.equal(seedRec.steps[0].status, 'cached');
  assert.equal(seedRec.steps[1].status, 'cached');
  assert.equal(seedRec.steps[2].status, 'passed');
  assert.ok(files[1].content.includes('"fromSeed": 42'));

  const ac = new AbortController();
  const wfAbort = createBlankWorkflow('mail', 'Abort');
  wfAbort.steps = [
    { kind: 'transform', serviceId: 'local', code: 'return 1;' },
    { kind: 'transform', serviceId: 'local', code: 'return 2;' },
  ];
  ac.abort();
  const cancelRec = await runPipelineWorkflow(wfAbort, { bridge: {}, signal: ac.signal });
  assert.equal(cancelRec.status, 'cancelled');
  assert.equal(cancelRec.steps[0].status, 'cancelled');
});

await test('Auth required pause & auto-resume handles AUTH_REQUIRED cleanly', async () => {
  let authed = false;
  let authModalCalls = 0;
  const wf = createBlankWorkflow('mail', 'Auth');
  wf.steps = [createSiteActionNode('gmail.read_inbox')];
  const rec = await runPipelineWorkflow(wf, {
    bridge: {
      tabs: {
        runFlow: async () => ({ run: { status: 'passed', outputs: { inbox: { count: 1, rows: [] } } } }),
      },
    },
    ensureServiceTabReady: async () => {
      if (!authed) {
        const err = new Error('Login required');
        err.code = 'AUTH_REQUIRED';
        err.tabUrl = 'https://accounts.google.com/';
        throw err;
      }
      return { open: true, originAllowed: true };
    },
    onAuthRequired: async () => {
      authModalCalls++;
      authed = true;
      return true;
    },
  });
  assert.equal(authModalCalls, 1);
  assert.equal(rec.status, 'passed');
  assert.deepEqual(rec.outputs.inbox, { count: 1, rows: [] });
});

await test('Sandbox isolates globals, contains constructor escapes, and terminates infinite loops', async () => {
  const ok = await runSandboxedTransform('return { doubled: (input?.n || 0) * 2 };', { input: { n: 21 } });
  assert.deepEqual(ok, { doubled: 42 });

  assert.equal(TRANSFORM_FRAME_SANDBOX, 'allow-scripts');
  assert.ok(!TRANSFORM_FRAME_SANDBOX.includes('allow-same-origin'));
  assert.match(TRANSFORM_FRAME_CSP, /connect-src 'none'/);

  const leakCheck = await runSandboxedTransform(
    'return { hasBridge: typeof ladybirdApp, hasFetch: typeof fetch, hasProcess: typeof process };',
    {}
  );
  assert.equal(leakCheck.hasBridge, 'undefined');
  assert.equal(leakCheck.hasFetch, 'undefined');
  assert.equal(leakCheck.hasProcess, 'undefined');

  await assert.rejects(
    () => runSandboxedTransform('return ({}).constructor.constructor("return process")();', {}),
    /Code generation from strings disallowed/i
  );

  await assert.rejects(
    () => runSandboxedTransform('while (true) {}', { timeoutMs: 120 }),
    /timed out|Script execution timed out/i
  );
});

await test('Secrets Vault encrypts, injects into HTTP, redacts outputs, and strips exports', async () => {
  const mem = new Map();
  const fakeBridge = {
    kv: {
      get: async (k) => (mem.has(k) ? { found: true, value: mem.get(k) } : { found: false }),
      set: async (k, v) => { mem.set(k, v); },
    },
  };
  const store = createSecretStore(fakeBridge);
  await store.set('stripe_key', 'sk_live_secret_987654');
  const list = await store.list();
  assert.equal(list.length, 1);
  assert.equal(list[0].name, 'STRIPE_KEY');
  assert.ok(!JSON.stringify(mem.get('flowstudio:secrets:v1')).includes('sk_live_secret_987654'));

  let seenAuthHeader = null;
  const wf = createBlankWorkflow('mail', 'Secret HTTP');
  wf.steps = [
    {
      kind: 'http',
      serviceId: 'local',
      method: 'POST',
      url: 'https://api.example.com/hook',
      headersJson: '{"Authorization":"Bearer {{secrets.STRIPE_KEY}}"}',
      body: '{"ping":true}',
      save_as: 'hook',
    },
  ];
  const rec = await runPipelineWorkflow(wf, {
    bridge: fakeBridge,
    resolveSecrets: () => store.resolveMap(),
    fetchFn: async (_url, opts) => {
      seenAuthHeader = opts.headers.Authorization;
      return {
        status: 200,
        ok: true,
        text: async () => JSON.stringify({ echoedToken: 'Bearer sk_live_secret_987654' }),
      };
    },
  });
  assert.equal(rec.status, 'passed');
  assert.equal(seenAuthHeader, 'Bearer sk_live_secret_987654');
  assert.ok(!JSON.stringify(rec).includes('sk_live_secret_987654'));
  assert.equal(rec.outputs.hook.data.echoedToken, 'Bearer ••••••••');

  const cleaned = stripSecretsFromExport({ token: 'sk_live_secret_987654' }, await store.resolveMap());
  assert.equal(cleaned.token, '••••••••');
  assert.equal(redactSecrets('token=sk_live_secret_987654', await store.resolveMap()), 'token=••••••••');
});

await test('Human-in-the-loop approval pauses, runs on approve, cancels on reject', async () => {
  const wf = createBlankWorkflow('mail', 'Approval Test');
  wf.steps = [
    { kind: 'transform', serviceId: 'local', label: 'Sensitive Action', code: 'return "sent";', requireApproval: true, save_as: 'res' },
    { kind: 'transform', serviceId: 'local', label: 'After', code: 'return "done";', save_as: 'after' },
  ];

  const rejectedRec = await runPipelineWorkflow(wf, {
    bridge: {},
    onApprovalRequired: async (info) => {
      assert.equal(info.stepIndex, 0);
      assert.equal(info.label, 'Sensitive Action');
      return false;
    },
  });
  assert.equal(rejectedRec.status, 'cancelled');
  assert.equal(rejectedRec.steps[0].status, 'rejected');
  assert.equal(rejectedRec.steps[1].status, 'cancelled');

  const approvedRec = await runPipelineWorkflow(wf, {
    bridge: {},
    onApprovalRequired: async () => true,
  });
  assert.equal(approvedRec.status, 'passed');
  assert.equal(approvedRec.outputs.res, 'sent');
  assert.equal(approvedRec.outputs.after, 'done');
});

await test('Cron 5-field parser, matching & nextRuns work accurately', () => {
  assert.equal(parseCronExpression('bad cron').ok, false);
  const parsed = parseCronExpression('30 9 * * 1-5');
  assert.equal(parsed.ok, true);
  const mon = new Date(2026, 10, 30, 9, 30, 0);
  const sun = new Date(2026, 10, 29, 9, 30, 0);
  assert.equal(matchesCron('30 9 * * 1-5', mon), true);
  assert.equal(matchesCron('30 9 * * 1-5', sun), false);
  const upcoming = nextCronRuns('*/15 * * * *', 3, new Date(2026, 10, 28, 10, 2, 0));
  assert.equal(upcoming.length, 3);
  assert.equal(upcoming[0].getMinutes(), 15);
  assert.equal(upcoming[1].getMinutes(), 30);
  assert.equal(upcoming[2].getMinutes(), 45);
  assert.ok(/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}$/.test(cronSlotKey(mon)));
});

await test('Visual Recorder normalizes semantic clicks, collapsed fills & extract', () => {
  const steps = normalizeRecordedEvents([
    { type: 'fill', locator: { kind: 'placeholder', value: 'Search' }, text: 'inv' },
    { type: 'fill', locator: { kind: 'placeholder', value: 'Search' }, text: 'invoice' },
    { type: 'click', locator: { kind: 'role', role: 'button', name: 'Go' } },
    { type: 'extract', locator: { kind: 'css', value: '.total' }, preview: '$500' },
    { type: 'press', key: 'Enter' },
  ], 'mail');
  assert.equal(steps.length, 4);
  assert.equal(steps[0].action, 'fill');
  assert.equal(steps[0].text, 'invoice');
  assert.equal(steps[1].action, 'click');
  assert.equal(steps[2].action, 'extract');
  assert.equal(steps[2].save_as, 'extracted_1');
  assert.equal(steps[3].action, 'press');
});

await test('FlowPack build, SHA-256 integrity, tamper rejection & install diff', async () => {
  const wf = cloneTemplate('gmail-inbox-to-export');
  wf.trigger.webhookSecret = 'super-hook-secret';
  wf.steps.push({ kind: 'http', serviceId: 'local', method: 'POST', url: 'https://api.example.com', headersJson: '{"X-Key":"{{secrets.CRM_TOKEN}}"}', body: 'ok', save_as: 'h' });
  const customAct = createCustomSiteActionTemplate('slack-work');
  const services = [
    { id: 'mail', label: 'Gmail', defaultUrl: 'https://mail.google.com/', origins: ['https://mail.google.com'], configuredContainer: 'personal' },
    { id: 'slack-work', label: 'Slack', defaultUrl: 'https://app.slack.com/', origins: ['https://app.slack.com'], configuredContainer: 'work' },
  ];
  const pack = await buildFlowPack({
    name: 'CRM Bridge Pack',
    workflows: [wf],
    customActions: [customAct],
    services,
    secretMap: { CRM_TOKEN: 'raw_secret_val_999' },
  });
  assert.ok(pack.integrity.startsWith('sha256-'));
  assert.ok(!JSON.stringify(pack).includes('raw_secret_val_999'));
  assert.ok(!JSON.stringify(pack).includes('super-hook-secret'));
  assert.deepEqual(pack.requiredSecrets, ['CRM_TOKEN']);
  assert.equal(pack.requiredServices.length, 2);

  const okVal = await validateFlowPack(pack);
  assert.equal(okVal.valid, true);
  assert.equal(okVal.integrityValid, true);

  const tampered = { ...pack, name: 'Hacked Pack' };
  const badVal = await validateFlowPack(tampered);
  assert.equal(badVal.valid, false);
  assert.equal(badVal.integrityValid, false);

  const diff = diffFlowPackInstall(pack, {
    existingServices: [{ id: 'mail' }],
    existingSecrets: [],
    existingWorkflows: [],
    existingActions: [],
  });
  assert.equal(diff.newServices.length, 1);
  assert.equal(diff.newServices[0].id, 'slack-work');
  assert.deepEqual(diff.missingSecrets, ['CRM_TOKEN']);
});

await test('Auto-Heal ranks semantic candidates & runner attaches healSuggestions on locator failure', async () => {
  const brokenStep = { kind: 'step', action: 'click', label: 'Submit Invoice Button', locator: { kind: 'css', value: '.old-submit-invoice', role: 'button' } };
  const candidates = [
    { tag: 'div', role: '', ariaLabel: '', labelText: '', placeholder: '', testId: '', id: 'hdr', name: '', text: 'Dashboard Header', css: '#hdr' },
    { tag: 'button', role: 'button', ariaLabel: 'Submit Invoice', labelText: '', placeholder: '', testId: 'submit-invoice-btn', id: 'btn1', name: '', text: 'Submit Invoice', css: 'button.primary' },
  ];
  const ranked = rankHealCandidates(brokenStep, candidates);
  assert.ok(ranked.length >= 1);
  assert.equal(ranked[0].locator.kind, 'test_id');
  assert.equal(ranked[0].locator.value, 'submit-invoice-btn');

  const wf = createBlankWorkflow('mail', 'Heal Test');
  wf.steps = [brokenStep];
  const rec = await runPipelineWorkflow(wf, {
    bridge: {
      tabs: {
        runFlow: async () => ({ run: { status: 'failed', error: 'Locator timed out' } }),
        eval: async () => ({ value: candidates }),
      },
    },
    ensureServiceTabReady: async () => ({ open: true, originAllowed: true }),
    captureFailureScreenshot: false,
  });
  assert.equal(rec.status, 'failed');
  assert.ok(Array.isArray(rec.steps[0].healSuggestions) && rec.steps[0].healSuggestions.length >= 1);
  assert.equal(rec.steps[0].healSuggestions[0].locator.value, 'submit-invoice-btn');
});

await test('mergeLoadedState & i18n', () => {
  const flows = [{ id: 'f1', name: 'F1', steps: [createBlankStep('sleep')] }];
  const triggers = [{ id: 'trig-f1', flowId: 'f1', serviceId: 'docs', type: 'interval', intervalMs: 15000, fireCount: 4 }];
  const merged = mergeLoadedState(flows, triggers, { f1: { nodePositions: { trigger: { x: 20, y: 30 } } } });
  assert.equal(merged[0].serviceId, 'docs');
  assert.equal(merged[0].trigger.fireCount, 4);

  setLanguage('es');
  assert.equal(getLanguage(), 'es');
  assert.equal(t('Run Now'), 'Ejecutar ahora');
  setLanguage('pt');
  assert.equal(getLanguage(), 'pt');
  assert.equal(t('Run Now'), 'Executar agora');
  setLanguage('en');
  assert.equal(t('Run Now'), 'Run Now');
});

await test('Static hygiene: zero innerHTML, zero missing/circular imports & ES/PT i18n parity', async () => {
  const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../src');
  const files = [];
  const walk = (d) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name);
      if (e.isDirectory()) walk(p);
      else if (p.endsWith('.js')) files.push(p);
    }
  };
  walk(srcRoot);

  let innerHtmlHits = 0;
  let missingImports = 0;
  const adj = new Map();
  const usedKeys = new Set();

  for (const f of files) {
    const txt = fs.readFileSync(f, 'utf8');
    if (txt.includes('innerHTML')) innerHtmlHits++;
    const deps = [];
    for (const m of txt.matchAll(/from\s+['"](\.[^'"]+)['"]/g)) {
      const target = path.resolve(path.dirname(f), m[1]);
      if (!fs.existsSync(target)) missingImports++;
      deps.push(target);
    }
    if (!f.includes(`${path.sep}locales${path.sep}`)) {
      for (const m of txt.matchAll(/\bt\(\s*(['"])((?:\\.|(?!\1).)+)\1\s*(?:,|\))/g)) usedKeys.add(m[2].replace(/\\'/g, "'"));
    }
    adj.set(path.resolve(f), deps);
  }

  let cycles = 0;
  const state = new Map();
  const dfs = (u) => {
    state.set(u, 1);
    for (const v of adj.get(u) || []) {
      if (state.get(v) === 1) cycles++;
      else if (!state.get(v)) dfs(v);
    }
    state.set(u, 2);
  };
  for (const u of adj.keys()) if (!state.get(u)) dfs(u);

  const { default: es } = await import(pathToFileURL(path.join(srcRoot, 'lib', 'locales', 'es.js')).href);
  const { default: pt } = await import(pathToFileURL(path.join(srcRoot, 'lib', 'locales', 'pt.js')).href);
  const missingEs = [...usedKeys].filter((k) => !(k in es));
  const missingPt = [...usedKeys].filter((k) => !(k in pt));
  const esOnly = Object.keys(es).filter((k) => !(k in pt));
  const ptOnly = Object.keys(pt).filter((k) => !(k in es));

  assert.equal(innerHtmlHits, 0);
  assert.equal(missingImports, 0);
  assert.equal(cycles, 0);
  assert.deepEqual(missingEs, []);
  assert.deepEqual(missingPt, []);
  assert.deepEqual(esOnly, []);
  assert.deepEqual(ptOnly, []);
});

console.log(`FLOWSTUDIO_TEST_SUMMARY passed=${passed} failed=0 total=${passed}`);
