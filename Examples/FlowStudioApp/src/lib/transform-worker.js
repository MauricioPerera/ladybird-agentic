/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// This source is instantiated only by the opaque-origin frame in sandbox.js.
// Keeping it as data prevents callers from accidentally creating a same-origin
// worker and treating identifier shadowing as a security boundary.
export const TRANSFORM_WORKER_SOURCE = String.raw`
'use strict';

self.onmessage = async (event) => {
  const { code, input, outputs, vars } = event.data || {};
  try {
    const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
    const fn = new AsyncFunction(
      'input',
      'outputs',
      'vars',
      'fetch',
      'XMLHttpRequest',
      'WebSocket',
      'EventSource',
      'importScripts',
      'self',
      'globalThis',
      'navigator',
      'indexedDB',
      'caches',
      'Worker',
      'SharedWorker',
      'BroadcastChannel',
      '"use strict";\n' + String(code || 'return input;')
    );
    const result = await fn(input, outputs, vars);
    self.postMessage({ ok: true, result });
  } catch (error) {
    self.postMessage({ ok: false, error: error?.message || String(error) });
  }
};
`;
