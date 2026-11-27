/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

export const SERVER_PATH = path.join(__dirname, 'server.mjs');

export const LADYBIRD_MANIFEST = Object.freeze({
  name: 'ladybird',
  version: '0.1.0',
  spiVersion: 1,
  platform: 'web',
  methods: [
    'engine/observe',
    'engine/perform',
    'keyboard/type',
    'keyboard/press',
    'artifacts/screenshot',
    'session/open',
    'session/back',
    'session/restart',
    'lifecycle/doctor',
    'lifecycle/init',
    'lifecycle/startAttempt',
    'lifecycle/endAttempt',
    'lifecycle/dispose',
  ],
  actions: ['tap', 'fill', 'clear', 'check', 'uncheck', 'focus', 'scrollIntoView', 'swipe', 'setInputFiles'],
});

export function ladybirdEngineOptions({
  spawnTransport,
  mcpUrl = process.env.LADYBIRD_MCP_URL || 'http://127.0.0.1:7331/mcp',
  mcpToken = process.env.LADYBIRD_MCP_TOKEN || '',
  nodePath = process.execPath,
} = {}) {
  if (typeof spawnTransport !== 'function') {
    throw new TypeError('ladybirdEngineOptions requires spawnTransport from e2e/engine');
  }
  return {
    manifest: LADYBIRD_MANIFEST,
    transport: spawnTransport({
      command: nodePath,
      args: [SERVER_PATH],
    }),
    initializationOptions: {
      mcpUrl,
      mcpToken,
    },
    resolveLocators: true,
  };
}

export function createLadybirdEngine({ connectEngine, spawnTransport, mcpUrl, mcpToken, nodePath }) {
  if (typeof connectEngine !== 'function') {
    throw new TypeError('createLadybirdEngine requires connectEngine from e2e/engine');
  }
  return connectEngine(ladybirdEngineOptions({ spawnTransport, mcpUrl, mcpToken, nodePath }));
}
