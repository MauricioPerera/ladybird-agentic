/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import assert from 'node:assert/strict';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { test } from 'node:test';
import { spawn } from 'node:child_process';
import { pathToFileURL } from 'node:url';
import { LADYBIRD_MANIFEST, SERVER_PATH, createLadybirdEngine } from '../index.mjs';

const TEST_TOKEN = 'test-ladybird-token';
const SAMPLE_PNG_BASE64 = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==';

function startMockMcpServer() {
  const state = {
    tabs: new Map(),
    nextTabId: 100,
    calls: [],
    currentUrl: 'about:blank',
    title: 'Initial',
    inputValue: '',
    uploadedFiles: [],
  };

  const server = http.createServer(async (req, res) => {
    if (req.headers.authorization !== `Bearer ${TEST_TOKEN}`) {
      res.writeHead(401, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: 'Unauthorized' }));
      return;
    }
    if (req.url !== '/mcp' || req.method !== 'POST') {
      res.writeHead(404);
      res.end();
      return;
    }

    const chunks = [];
    for await (const c of req) chunks.push(c);
    const rpc = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    state.calls.push({ method: rpc.method, params: rpc.params });

    if (rpc.method !== 'tools/call') {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ jsonrpc: '2.0', id: rpc.id, result: {} }));
      return;
    }

    const { name, arguments: args = {} } = rpc.params;
    let payload;
    switch (name) {
      case 'list_tabs':
        payload = { tabs: Array.from(state.tabs.values()) };
        break;
      case 'open_tab': {
        const id = state.nextTabId++;
        state.tabs.set(id, { id, title: 'New Tab', url: args.url || 'about:blank', is_loading: false, is_active: true });
        payload = { tab_id: id, url: args.url || 'about:blank', mcp_access_allowed: true };
        break;
      }
      case 'close_tab':
        state.tabs.delete(args.tab_id);
        payload = { closed: true, tab_id: args.tab_id };
        break;
      case 'navigate':
        state.currentUrl = args.url;
        state.title = 'Example Domain';
        payload = { tab_id: args.tab_id, url: args.url, title: state.title };
        break;
      case 'go_back':
        state.currentUrl = 'about:blank';
        payload = { ok: true, tab_id: args.tab_id };
        break;
      case 'eval_js': {
        const script = String(args.script || '');
        if (script.includes('data-e2e-ref') && script.includes('viewport')) {
          payload = {
            value: {
              location: state.currentUrl,
              root: {
                ref: { id: 'root', revision: '1' },
                role: 'WebArea',
                name: state.title,
                children: [
                  {
                    ref: { id: 'n1', revision: '1' },
                    role: 'heading',
                    name: 'Example Domain',
                    text: 'Example Domain',
                    level: 1,
                    states: { hidden: false, disabled: false, focused: false, secure: false },
                    rect: { x: 10, y: 10, width: 300, height: 40 },
                  },
                  {
                    ref: { id: 'n2', revision: '1' },
                    role: 'textbox',
                    name: 'Email',
                    value: state.inputValue,
                    states: { hidden: false, disabled: false, focused: false, secure: false },
                    rect: { x: 10, y: 60, width: 200, height: 32 },
                  },
                ],
              },
              viewport: { width: 1280, height: 720 },
            },
          };
        } else if (script.includes('setNativeValue')) {
          state.inputValue = 'user@example.com';
          payload = { value: { ok: true } };
        } else {
          payload = { value: { ok: true } };
        }
        break;
      }
      case 'take_screenshot':
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(
          JSON.stringify({
            jsonrpc: '2.0',
            id: rpc.id,
            result: {
              content: [{ type: 'image', data: SAMPLE_PNG_BASE64, mimeType: 'image/png' }],
              isError: false,
            },
          }),
        );
        return;
      case 'upload_file':
        state.uploadedFiles.push({ selector: args.selector, paths: args.paths });
        payload = { uploaded: true, file_count: Array.isArray(args.paths) ? args.paths.length : 0, paths: args.paths || [] };
        break;
      default:
        payload = { ok: true };
    }

    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(
      JSON.stringify({
        jsonrpc: '2.0',
        id: rpc.id,
        result: {
          content: [{ type: 'text', text: JSON.stringify(payload) }],
          isError: false,
        },
      }),
    );
  });

  return new Promise(resolve => {
    server.listen(0, '127.0.0.1', () => {
      const addr = server.address();
      resolve({
        url: `http://127.0.0.1:${addr.port}/mcp`,
        state,
        close: () => new Promise(r => server.close(() => r())),
      });
    });
  });
}

function createLspClient(child) {
  let buf = Buffer.alloc(0);
  let bodyLen = undefined;
  let nextId = 1;
  const pending = new Map();

  child.stdout.on('data', chunk => {
    buf = Buffer.concat([buf, chunk]);
    while (true) {
      if (bodyLen === undefined) {
        const idx = buf.indexOf(Buffer.from('\r\n\r\n'));
        if (idx === -1) return;
        const m = /Content-Length:\s*(\d+)/i.exec(buf.subarray(0, idx).toString('ascii'));
        bodyLen = Number(m[1]);
        buf = buf.subarray(idx + 4);
      }
      if (buf.length < bodyLen) return;
      const msg = JSON.parse(buf.subarray(0, bodyLen).toString('utf8'));
      buf = buf.subarray(bodyLen);
      bodyLen = undefined;
      const waiter = pending.get(msg.id);
      if (waiter) {
        pending.delete(msg.id);
        if (msg.error) waiter.reject(new Error(`${msg.error.code}: ${msg.error.message}`));
        else waiter.resolve(msg.result);
      }
    }
  });

  function sendRaw(obj) {
    const json = Buffer.from(JSON.stringify(obj), 'utf8');
    child.stdin.write(Buffer.concat([Buffer.from(`Content-Length: ${json.length}\r\n\r\n`, 'ascii'), json]));
  }

  return {
    request(method, params) {
      const id = nextId++;
      return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
        sendRaw({ jsonrpc: '2.0', id, method, params });
      });
    },
    notify(method, params) {
      sendRaw({ jsonrpc: '2.0', method, params });
    },
  };
}

test('LSP Engine Protocol v1 round-trip against mock Ladybird MCP server', async () => {
  const mock = await startMockMcpServer();
  const child = spawn(process.execPath, [SERVER_PATH], { stdio: ['pipe', 'pipe', 'pipe'] });
  const rpc = createLspClient(child);

  try {
    const initRes = await rpc.request('initialize', {
      protocolVersion: 1,
      clientInfo: { name: 'test-harness', version: '0.1.0' },
      role: 'worker',
      initializationOptions: { mcpUrl: mock.url, mcpToken: TEST_TOKEN },
    });
    assert.equal(initRes.protocolVersion, 1);
    assert.deepEqual(initRes.manifest, LADYBIRD_MANIFEST);
    rpc.notify('initialized', {});

    const doctor = await rpc.request('lifecycle/doctor', { targetName: 'ladybird', projectRoot: process.cwd(), app: {}, env: {}, headed: true, signal: { id: 1 } });
    assert.equal(doctor[0].status, 'pass');

    await rpc.request('lifecycle/init', { app: { origin: 'https://example.com' }, signal: { id: 2 } });
    await rpc.request('session/open', { url: '/login', operation: { signal: { id: 3 }, timeoutMs: 5000, runId: 'r1', attemptId: 'a1', origin: 'test' } });
    assert.equal(mock.state.currentUrl, 'https://example.com/login');

    const snapshot = await rpc.request('engine/observe', { operation: { signal: { id: 4 }, timeoutMs: 5000, runId: 'r1', attemptId: 'a1', origin: 'test' } });
    assert.equal(snapshot.location, 'https://example.com/login');
    assert.equal(snapshot.root.children.length, 2);

    await rpc.request('engine/perform', {
      ref: snapshot.root.children[1].ref,
      action: { kind: 'fill', value: 'user@example.com', sensitive: false },
      operation: { signal: { id: 5 }, timeoutMs: 5000, runId: 'r1', attemptId: 'a1', origin: 'test' },
    });
    assert.equal(mock.state.inputValue, 'user@example.com');

    await rpc.request('engine/perform', {
      ref: snapshot.root.children[1].ref,
      action: { kind: 'setInputFiles', paths: ['demo.mp4'] },
      operation: { signal: { id: 51 }, timeoutMs: 5000, runId: 'r1', attemptId: 'a1', origin: 'test' },
    });
    assert.deepEqual(mock.state.uploadedFiles, [{ selector: '[data-e2e-ref="1:n2"]', paths: ['demo.mp4'] }]);

    const shot = await rpc.request('artifacts/screenshot', { operation: { signal: { id: 6 }, timeoutMs: 5000, runId: 'r1', attemptId: 'a1', origin: 'test' } });
    assert.equal(shot.mediaType, 'image/png');
    assert.equal(shot.data, SAMPLE_PNG_BASE64);

    await rpc.request('lifecycle/dispose', { signal: { id: 7 }, timeoutMs: 2000 });
    assert.equal(mock.state.tabs.size, 0);

    await rpc.request('shutdown', null);
    rpc.notify('exit', null);
  } finally {
    child.kill();
    await mock.close();
  }
});

const e2eRepoPath = process.env.E2E_REPO_PATH;
const e2eClientPath = e2eRepoPath ? path.join(e2eRepoPath, 'packages/core/dist/engine/protocol/client.js') : '';
const e2eTransportPath = e2eRepoPath ? path.join(e2eRepoPath, 'packages/core/dist/engine/protocol/transport.js') : '';

test('e2e connectEngine + spawnTransport integration check', { skip: !e2eClientPath || !fs.existsSync(e2eClientPath) || !fs.existsSync(e2eTransportPath) }, async () => {
  const { connectEngine } = await import(pathToFileURL(e2eClientPath).href);
  const { spawnTransport } = await import(pathToFileURL(e2eTransportPath).href);

  const mock = await startMockMcpServer();
  const controller = new AbortController();
  let engine = null;
  try {
    const handle = createLadybirdEngine({
      connectEngine,
      spawnTransport,
      mcpUrl: mock.url,
      mcpToken: TEST_TOKEN,
    });

    assert.equal(handle.name, 'ladybird');
    assert.equal(handle.platform, 'web');
    engine = handle;

    const checks = await engine.doctor({
      targetName: 'ladybird-desktop',
      projectRoot: process.cwd(),
      app: { origin: 'https://example.com', site: 'example.com' },
      env: {},
      headed: true,
      signal: controller.signal,
      log: () => {},
    });
    assert.equal(checks[0].status, 'pass');

    await engine.init({
      runId: 'run-1',
      targetName: 'ladybird-desktop',
      projectRoot: process.cwd(),
      app: { origin: 'https://example.com', site: 'example.com' },
      env: {},
      headed: true,
      workerSlot: 0,
      hostId: 'local',
      hostName: 'localhost',
      hostPlatform: process.platform,
      signal: controller.signal,
      log: () => {},
    });

    const op = { signal: controller.signal, timeoutMs: 5000, runId: 'run-1', attemptId: 'attempt-1', origin: 'test' };
    await engine.session.open('/', op);
    assert.equal(mock.state.currentUrl, 'https://example.com/');

    const matches = await engine.locate({ kind: 'query', query: { kind: 'role', value: { kind: 'string', value: 'heading', exact: true }, name: { kind: 'string', value: 'Example Domain', exact: false } } }, op);
    assert.equal(matches.length, 1);
    assert.equal(matches[0].role, 'heading');
  } finally {
    if (engine) {
      await engine.dispose({ signal: controller.signal, timeoutMs: 2000 }).catch(() => {});
    }
    assert.equal(mock.state.tabs.size, 0);
    await mock.close();
  }
});
