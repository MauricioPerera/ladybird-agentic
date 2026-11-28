/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

'use strict';

self.onmessage = async (ev) => {
  const { id, code, input, outputs, vars } = ev.data || {};
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
      `"use strict";\n${String(code || 'return input;')}`
    );
    const result = await fn(input, outputs, vars);
    self.postMessage({ id, ok: true, result });
  } catch (err) {
    self.postMessage({ id, ok: false, error: err?.message || String(err) });
  }
};
