/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { TRANSFORM_WORKER_SOURCE } from './transform-worker.js';

const isNodeRuntime = typeof process !== 'undefined' && Boolean(process.versions?.node) && typeof Worker !== 'function';

// Omitting allow-same-origin is the primary boundary: even a recovered global
// remains in an opaque origin instead of the App Mode server's origin.
export const TRANSFORM_FRAME_SANDBOX = 'allow-scripts';
export const TRANSFORM_FRAME_CSP = [
  "default-src 'none'",
  "script-src 'unsafe-inline' blob:",
  'worker-src blob:',
  'child-src blob:',
  "connect-src 'none'",
  "img-src 'none'",
  "media-src 'none'",
  "font-src 'none'",
  "style-src 'none'",
  "frame-src 'none'",
  "object-src 'none'",
  "base-uri 'none'",
  "form-action 'none'",
].join('; ');

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
  const encodeData = (value) => {
    const cloned = cloneData(value);
    return JSON.stringify({ defined: cloned !== undefined, value: cloned });
  };
  const contextData = Object.assign(Object.create(null), {
    __inputJson: encodeData(input),
    __outputsJson: encodeData(outputs || {}),
    __varsJson: encodeData(vars || {}),
  });
  const context = vm.createContext(contextData, {
    codeGeneration: { strings: false, wasm: false },
  });
  const wrapped = `(async () => {
"use strict";
const decode = (encoded) => {
  const parsed = JSON.parse(encoded);
  return parsed.defined ? parsed.value : undefined;
};
const input = decode(__inputJson);
const outputs = decode(__outputsJson);
const vars = decode(__varsJson);
const result = await (async () => {
${String(code || 'return input;')}
})();
try {
  return JSON.stringify({ defined: result !== undefined, value: result });
} catch {
  return JSON.stringify({ defined: true, value: null });
}
})()`;
  const serialized = await vm.runInContext(wrapped, context, { timeout: timeoutMs });
  const result = JSON.parse(serialized);
  return result.defined ? result.value : undefined;
}

function buildTransformFrameDocument() {
  const escapedCsp = TRANSFORM_FRAME_CSP.replaceAll('&', '&amp;').replaceAll('"', '&quot;');
  const workerSource = JSON.stringify(TRANSFORM_WORKER_SOURCE).replaceAll('<', '\\u003c');
  return `<!doctype html>
<meta http-equiv="Content-Security-Policy" content="${escapedCsp}">
<script>
'use strict';
const workerSource = ${workerSource};
let worker = null;

addEventListener('message', (event) => {
  if (event.source !== parent || worker !== null) return;
  const request = event.data;
  if (!request || request.type !== 'ladybird-transform-run') return;

  let workerUrl = null;
  try {
    workerUrl = URL.createObjectURL(new Blob([workerSource], { type: 'text/javascript' }));
    worker = new Worker(workerUrl);
    URL.revokeObjectURL(workerUrl);
  } catch (error) {
    if (workerUrl) URL.revokeObjectURL(workerUrl);
    parent.postMessage({
      type: 'ladybird-transform-result',
      id: request.id,
      ok: false,
      error: error?.message || String(error),
    }, '*');
    return;
  }

  worker.onmessage = (workerEvent) => {
    const reply = workerEvent.data || {};
    parent.postMessage({
      type: 'ladybird-transform-result',
      id: request.id,
      ok: reply.ok === true,
      result: reply.ok === true ? reply.result : undefined,
      error: reply.ok === true ? undefined : (reply.error || 'Transform failed in sandbox'),
    }, '*');
  };
  worker.onerror = (workerEvent) => {
    parent.postMessage({
      type: 'ladybird-transform-result',
      id: request.id,
      ok: false,
      error: workerEvent?.message || 'Transform worker failed',
    }, '*');
  };
  worker.postMessage({
    code: String(request.code || 'return input;'),
    input: request.input,
    outputs: request.outputs,
    vars: request.vars,
  });
});

parent.postMessage({ type: 'ladybird-transform-ready' }, '*');
</script>`;
}

async function runBrowserSandbox(code, { input, outputs, vars, timeoutMs, signal }) {
  return new Promise((resolve, reject) => {
    let settled = false;
    let frame = null;
    let frameUrl = null;
    let timer = null;
    let onMessage = null;

    const cleanup = () => {
      if (timer) clearTimeout(timer);
      if (signal) signal.removeEventListener('abort', onAbort);
      if (typeof window !== 'undefined' && onMessage) window.removeEventListener('message', onMessage);
      try { frame?.remove(); } catch {}
      if (frameUrl) URL.revokeObjectURL(frameUrl);
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

    if (signal?.aborted) {
      onAbort();
      return;
    }
    signal?.addEventListener('abort', onAbort, { once: true });

    timer = setTimeout(() => {
      fail(new Error(`Transform timed out after ${timeoutMs}ms`));
    }, timeoutMs);

    onMessage = (event) => {
      // The opaque child cannot use an origin target, so authenticate it by its
      // WindowProxy and accept only the fixed transform protocol.
      if (event.source !== frame?.contentWindow) return;
      const msg = event.data || {};
      if (settled) return;
      if (msg.type === 'ladybird-transform-ready') {
        try {
          frame.contentWindow.postMessage({
            type: 'ladybird-transform-run',
            id: 1,
            code: String(code || 'return input;'),
            input: cloneData(input),
            outputs: cloneData(outputs || {}),
            vars: cloneData(vars || {}),
          }, '*');
        } catch (err) {
          fail(err);
        }
        return;
      }
      if (msg.type !== 'ladybird-transform-result' || msg.id !== 1) return;
      if (!msg.ok) {
        fail(new Error(msg.error || 'Transform failed in sandbox'));
        return;
      }
      settled = true;
      cleanup();
      resolve(msg.result);
    };

    try {
      if (typeof document === 'undefined' || typeof window === 'undefined') {
        throw new Error('Transform sandbox is unavailable');
      }
      frame = document.createElement('iframe');
      frame.setAttribute('sandbox', TRANSFORM_FRAME_SANDBOX);
      frame.setAttribute('aria-hidden', 'true');
      frame.style.display = 'none';
      frameUrl = URL.createObjectURL(new Blob([buildTransformFrameDocument()], { type: 'text/html' }));
      frame.src = frameUrl;
      window.addEventListener('message', onMessage);
      (document.body || document.documentElement).append(frame);
    } catch (err) {
      fail(err);
    }
  });
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

  return await runBrowserSandbox(code, { input, outputs, vars, timeoutMs, signal });
}
