/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

const isNodeRuntime = typeof process !== 'undefined' && Boolean(process.versions?.node) && typeof Worker !== 'function';

function cloneData(v) {
  if (v === undefined || v === null) return v;
  try {
    return JSON.parse(JSON.stringify(v));
  } catch {
    return null;
  }
}

async function runNodeVmSandbox(code, { input, outputs, vars, timeoutMs }) {
  const vm = await import('node:vm');
  const wrapped = `(async () => {\n"use strict";\n${String(code || 'return input;')}\n})()`;
  const context = vm.createContext({
    input: cloneData(input),
    outputs: cloneData(outputs || {}),
    vars: cloneData(vars || {}),
    JSON,
    Math,
    Date,
    RegExp,
    Array,
    Object,
    String,
    Number,
    Boolean,
    Map,
    Set,
    ladybirdApp: undefined,
    fetch: undefined,
    process: undefined,
    require: undefined,
    globalThis: undefined,
    global: undefined,
  });
  const raw = await vm.runInContext(wrapped, context, { timeout: timeoutMs });
  return raw === undefined ? undefined : cloneData(raw);
}

async function runWorkerSandbox(code, { input, outputs, vars, timeoutMs, signal }) {
  return new Promise((resolve, reject) => {
    let settled = false;
    let worker = null;
    let timer = null;

    const cleanup = () => {
      if (timer) clearTimeout(timer);
      if (signal) signal.removeEventListener('abort', onAbort);
      try { worker?.terminate(); } catch {}
    };

    const fail = (err) => {
      if (settled) return;
      settled = true;
      cleanup();
      reject(err);
    };

    const onAbort = () => {
      const e = new Error('Run cancelled');
      e.name = 'AbortError';
      fail(e);
    };

    try {
      worker = new Worker('/src/lib/transform-worker.js');
    } catch (err) {
      reject(err);
      return;
    }

    if (signal?.aborted) {
      onAbort();
      return;
    }
    signal?.addEventListener('abort', onAbort, { once: true });

    timer = setTimeout(() => {
      fail(new Error(`Transform timed out after ${timeoutMs}ms`));
    }, timeoutMs);

    worker.onmessage = (ev) => {
      const msg = ev.data || {};
      if (settled) return;
      settled = true;
      cleanup();
      if (msg.ok) resolve(msg.result);
      else fail(new Error(msg.error || 'Transform failed in sandbox'));
    };

    worker.onerror = (ev) => {
      const err = new Error(ev?.message || 'Worker execution failed');
      err._workerInitFailed = true;
      fail(err);
    };

    try {
      worker.postMessage({
        id: 1,
        code: String(code || 'return input;'),
        input: cloneData(input),
        outputs: cloneData(outputs || {}),
        vars: cloneData(vars || {}),
      });
    } catch (err) {
      fail(err);
    }
  });
}

async function runShadowedFallback(code, { input, outputs, vars }) {
  const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
  const fn = new AsyncFunction(
    'input',
    'outputs',
    'vars',
    'window',
    'document',
    'ladybirdApp',
    'fetch',
    'XMLHttpRequest',
    'WebSocket',
    'localStorage',
    'sessionStorage',
    'indexedDB',
    'globalThis',
    `"use strict";\n${String(code || 'return input;')}`
  );
  return await fn(cloneData(input), cloneData(outputs || {}), cloneData(vars || {}));
}

export async function runSandboxedTransform(code, opts = {}) {
  const { input = null, outputs = {}, vars = {}, timeoutMs = 3000, signal = null } = opts;
  if (signal?.aborted) {
    const e = new Error('Run cancelled');
    e.name = 'AbortError';
    throw e;
  }

  if (isNodeRuntime) {
    return await runNodeVmSandbox(code, { input, outputs, vars, timeoutMs });
  }

  if (typeof Worker === 'function') {
    try {
      return await runWorkerSandbox(code, { input, outputs, vars, timeoutMs, signal });
    } catch (err) {
      if (err?.name === 'AbortError' || !err?._workerInitFailed) throw err;
    }
  }

  return await runShadowedFallback(code, { input, outputs, vars });
}
