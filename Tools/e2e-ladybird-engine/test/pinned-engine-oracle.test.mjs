/*
 * Copyright (c) 2026-present, the Ladybird developers.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { copyFileSync, mkdirSync, mkdtempDisposableSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';

const engine = fileURLToPath(new URL('../', import.meta.url));
const suite = 'test/engine-protocol.test.mjs';
const reporter = new URL('./engine-case-reporter.mjs', import.meta.url).href;
const caseNames = [
  'LSP Engine Protocol v1 round-trip against mock Ladybird MCP server',
  'e2e connectEngine + spawnTransport integration check',
];

function runSuite(root, sourceCheckout) {
  const env = { ...process.env, E2E_REPO_PATH: sourceCheckout };
  for (const name of ['NODE_OPTIONS', 'NODE_PATH', 'NODE_TEST_CONTEXT', 'LADYBIRD_MCP_URL', 'LADYBIRD_MCP_TOKEN']) {
    delete env[name];
  }
  const result = spawnSync(process.execPath, ['--test', `--test-reporter=${reporter}`, suite], {
    cwd: root, env, encoding: 'utf8', timeout: 15_000,
  });
  assert.ifError(result.error);
  assert.equal(result.signal, null);
  const events = result.stdout.trim().split(/\r?\n/).filter(Boolean).map(line => JSON.parse(line));
  return { status: result.status, events, diagnostics: result.stderr + events.map(row => row.diagnostic ?? '').join('') };
}

test('the pinned public e2e client runs both existing cases without a source checkout', t => {
  assert.equal(process.version, 'v24.16.0');
  const installed = JSON.parse(readFileSync(path.join(engine, 'node_modules', 'e2e', 'package.json'), 'utf8'));
  assert.equal(installed.version, '0.24.0');
  const temporary = mkdtempDisposableSync(path.join(tmpdir(), 'ladybird-e2e-oracle-'));
  t.after(temporary.remove);
  const result = runSuite(engine, path.join(temporary.path, 'absent-source'));
  assert.equal(result.status, 0, result.diagnostics);
  const expected = caseNames.map(name => ({ name, passed: true, skipped: false, todo: false }));
  assert.deepEqual(result.events.filter(row => row.name), expected);
});

test('the absent dependency is a failure instead of a skipped case', t => {
  const temporary = mkdtempDisposableSync(path.join(tmpdir(), 'ladybird-e2e-oracle-'));
  t.after(temporary.remove);
  mkdirSync(path.join(temporary.path, 'test'));
  for (const file of ['package.json', 'index.mjs', 'server.mjs', suite]) {
    copyFileSync(path.join(engine, file), path.join(temporary.path, file));
  }
  const result = runSuite(temporary.path, path.join(temporary.path, 'absent-source'));
  assert.notEqual(result.status, 0, 'The test process passed without the locked e2e client');
  assert.match(result.diagnostics, /ERR_MODULE_NOT_FOUND/);
  assert.match(result.diagnostics, /Cannot find package ['"]e2e['"]/);
});
