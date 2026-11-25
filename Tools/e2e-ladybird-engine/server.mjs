#!/usr/bin/env node
/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { LADYBIRD_MANIFEST } from './index.mjs';

const ENGINE_PROTOCOL_VERSION = 1;
const RPC_ENGINE_ERROR = -32000;
const RPC_INVALID_REQUEST = -32600;
const RPC_METHOD_NOT_FOUND = -32601;
const RPC_INVALID_PARAMS = -32602;
const RPC_INTERNAL_ERROR = -32603;

class LadybirdEngineError extends Error {
  constructor(code, message, retryable = false) {
    super(message);
    this.name = 'LadybirdEngineError';
    this.code = code;
    this.retryable = retryable;
  }
}

class McpClient {
  #url;
  #token;
  #nextId = 1;

  constructor(url, token) {
    this.#url = url;
    this.#token = token;
  }

  configure(url, token) {
    if (url) this.#url = url;
    if (token !== undefined) this.#token = token;
  }

  get url() {
    return this.#url;
  }

  get hasToken() {
    return Boolean(this.#token);
  }

  async rpc(method, params) {
    if (!this.#url) {
      throw new LadybirdEngineError('UNAVAILABLE', 'Ladybird MCP URL is not configured');
    }
    if (!this.#token) {
      throw new LadybirdEngineError('UNAVAILABLE', 'Ladybird MCP bearer token is missing (set mcpToken or LADYBIRD_MCP_TOKEN)');
    }

    const res = await fetch(this.#url, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Authorization: `Bearer ${this.#token}`,
      },
      body: JSON.stringify({
        jsonrpc: '2.0',
        id: this.#nextId++,
        method,
        params,
      }),
    });

    if (!res.ok) {
      const body = await res.text().catch(() => '');
      throw new LadybirdEngineError('UNAVAILABLE', `Ladybird MCP HTTP ${res.status}: ${body || res.statusText}`);
    }

    const payload = await res.json();
    if (payload.error) {
      throw new LadybirdEngineError('ACTION_FAILED', payload.error.message || 'MCP RPC error');
    }
    return payload.result;
  }

  async callTool(name, args = {}) {
    const result = await this.rpc('tools/call', { name, arguments: args });
    const first = Array.isArray(result?.content) ? result.content[0] : undefined;
    if (result?.isError) {
      throw new LadybirdEngineError('ACTION_FAILED', first?.text || `MCP tool ${name} failed`);
    }
    if (first?.type === 'image') {
      return first;
    }
    if (first?.type === 'text') {
      try {
        return JSON.parse(first.text);
      } catch {
        return { text: first.text };
      }
    }
    return result;
  }
}

const OBSERVE_SCRIPT = `
return (function(cfg) {
  var rev = String(cfg.revision);
  var nextSeq = 1;
  function norm(s) {
    return String(s == null ? '' : s).replace(/\\s+/g, ' ').trim();
  }
  function clip(s, maxLen) {
    return s && s.length > maxLen ? s.slice(0, maxLen) : s;
  }
  function isHidden(el) {
    if (!el || !el.isConnected) return true;
    var style = window.getComputedStyle(el);
    if (!style || style.display === 'none' || style.visibility === 'hidden' || parseFloat(style.opacity || '1') === 0) return true;
    var r = el.getBoundingClientRect();
    return r.width <= 0 || r.height <= 0;
  }
  function implicitRole(el) {
    var tag = el.tagName ? el.tagName.toLowerCase() : '';
    if (tag === 'button') return 'button';
    if (tag === 'a' && el.hasAttribute('href')) return 'link';
    if (tag === 'input') {
      var t = (el.getAttribute('type') || 'text').toLowerCase();
      if (t === 'button' || t === 'submit' || t === 'reset') return 'button';
      if (t === 'checkbox') return 'checkbox';
      if (t === 'radio') return 'radio';
      if (t === 'search') return 'searchbox';
      if (t !== 'hidden') return 'textbox';
    }
    if (tag === 'textarea') return 'textbox';
    if (tag === 'select') return 'combobox';
    if (tag === 'img') return 'img';
    if (/^h[1-6]$/.test(tag)) return 'heading';
    if (tag === 'status' || tag === 'output') return 'status';
    return '';
  }
  function labelTextFor(el) {
    if (el.getAttribute) {
      var al = el.getAttribute('aria-label');
      if (al) return al;
      var alb = el.getAttribute('aria-labelledby');
      if (alb) {
        var parts = alb.trim().split(/\\s+/).map(function(id) {
          var target = document.getElementById(id);
          return target ? norm(target.textContent) : '';
        }).filter(Boolean);
        if (parts.length > 0) return parts.join(' ');
      }
    }
    if (el.id) {
      var labels = document.querySelectorAll('label');
      for (var i = 0; i < labels.length; ++i) {
        if (labels[i].getAttribute('for') === el.id) return norm(labels[i].textContent);
      }
    }
    var parent = el.closest ? el.closest('label') : null;
    if (parent) return norm(parent.textContent);
    return '';
  }
  function accessibleName(el) {
    var lbl = labelTextFor(el);
    if (lbl) return lbl;
    var tag = el.tagName ? el.tagName.toLowerCase() : '';
    if (tag === 'input' && ['button', 'submit', 'reset'].indexOf((el.getAttribute('type') || '').toLowerCase()) !== -1)
      return el.value || '';
    if (el.getAttribute) {
      var alt = el.getAttribute('alt');
      if (alt) return alt;
      var ph = el.getAttribute('placeholder');
      if (ph) return ph;
      var title = el.getAttribute('title');
      if (title) return title;
    }
    return norm(el.innerText !== undefined ? el.innerText : el.textContent);
  }
  var all = document.querySelectorAll('a[href], button, input, textarea, select, h1, h2, h3, h4, h5, h6, img, p, li, [role], [data-testid], [contenteditable="true"], [tabindex]');
  var children = [];
  for (var i = 0; i < all.length && children.length < 300; ++i) {
    var el = all[i];
    var hidden = isHidden(el);
    var role = el.getAttribute('role') || implicitRole(el);
    var name = clip(accessibleName(el), 256);
    var text = clip(norm(el.innerText !== undefined ? el.innerText : el.textContent), 512);
    var testId = el.getAttribute ? el.getAttribute('data-testid') : null;
    if (!role && !name && !text && !testId) continue;
    var id = 'n' + (nextSeq++);
    var refToken = rev + ':' + id;
    el.setAttribute('data-e2e-ref', refToken);
    var tag = el.tagName ? el.tagName.toLowerCase() : '';
    var inputType = tag === 'input' ? (el.getAttribute('type') || 'text').toLowerCase() : '';
    var secure = inputType === 'password';
    var rect = el.getBoundingClientRect();
    var states = {
      hidden: hidden,
      disabled: Boolean(el.disabled || el.getAttribute('aria-disabled') === 'true'),
      focused: document.activeElement === el,
      secure: secure
    };
    if (inputType === 'checkbox' || inputType === 'radio' || role === 'checkbox' || role === 'radio') {
      states.checked = Boolean(el.checked || el.getAttribute('aria-checked') === 'true');
    }
    var node = {
      ref: { id: id, revision: rev },
      states: states,
      rect: { x: rect.x, y: rect.y, width: rect.width, height: rect.height }
    };
    if (role) node.role = role;
    if (name) node.name = name;
    if (text) node.text = text;
    if (testId) node.testId = testId;
    if (/^h[1-6]$/.test(tag)) node.level = Number(tag.slice(1));
    if (!secure && (tag === 'input' || tag === 'textarea' || tag === 'select')) {
      node.value = String(el.value == null ? '' : el.value);
    }
    children.push(node);
  }
  return {
    location: String(window.location.href || ''),
    root: {
      ref: { id: 'root', revision: rev },
      role: 'WebArea',
      name: String(document.title || ''),
      children: children
    },
    viewport: {
      width: window.innerWidth || 1280,
      height: window.innerHeight || 720
    }
  };
})(arguments[0]);
`;

const PERFORM_SCRIPT = `
return (function(cfg) {
  var refToken = cfg.ref.revision + ':' + cfg.ref.id;
  if (cfg.ref.id === 'root') {
    if (cfg.action && (cfg.action.kind === 'swipe' || cfg.action.kind === 'scroll')) {
      var dy = cfg.action.direction === 'up' ? -400 : (cfg.action.direction === 'down' ? 400 : 0);
      var dx = cfg.action.direction === 'left' ? -400 : (cfg.action.direction === 'right' ? 400 : 0);
      window.scrollBy(dx, dy);
      return { ok: true };
    }
    return { ok: false, code: 'UNSUPPORTED_CAPABILITY', error: 'Unsupported root action' };
  }
  var el = document.querySelector('[data-e2e-ref="' + refToken + '"]');
  if (!el) {
    var staleMatch = document.querySelector('[data-e2e-ref$=":' + cfg.ref.id + '"]');
    if (staleMatch) return { ok: false, code: 'STALE_REF', error: 'Element ref belongs to an earlier observation revision', retryable: true };
    return { ok: false, code: 'NOT_FOUND', error: 'Element ref not found in DOM', retryable: true };
  }
  var action = cfg.action || {};
  var kind = action.kind;
  function setNativeValue(target, val) {
    var proto = target instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : (target instanceof HTMLInputElement ? HTMLInputElement.prototype : null);
    var desc = proto ? Object.getOwnPropertyDescriptor(proto, 'value') : null;
    if (desc && desc.set) desc.set.call(target, val);
    else target.value = val;
  }
  if (el.scrollIntoView) el.scrollIntoView({ block: 'center', inline: 'center' });
  if (kind === 'tap') {
    if (el.disabled) return { ok: false, code: 'NOT_ACTIONABLE', error: 'Target element is disabled', retryable: true };
    if (el.focus) el.focus();
    el.click();
    return { ok: true };
  }
  if (kind === 'fill' || kind === 'clear') {
    if (el.disabled) return { ok: false, code: 'NOT_ACTIONABLE', error: 'Target element is disabled', retryable: true };
    if (el.focus) el.focus();
    var nextVal = kind === 'clear' ? '' : String(action.value == null ? '' : action.value);
    if (el.isContentEditable) el.textContent = nextVal;
    else setNativeValue(el, nextVal);
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
    return { ok: true };
  }
  if (kind === 'check' || kind === 'uncheck') {
    var want = kind === 'check';
    if (Boolean(el.checked) !== want) el.click();
    return { ok: true };
  }
  if (kind === 'scrollIntoView' || kind === 'scroll') {
    el.scrollIntoView({ block: 'center', inline: 'center' });
    return { ok: true };
  }
  if (kind === 'focus') {
    if (el.focus) el.focus();
    return { ok: true };
  }
  return { ok: false, code: 'UNSUPPORTED_CAPABILITY', error: 'Unsupported locator action: ' + kind };
})(arguments[0]);
`;

class LadybirdBridgeSession {
  #mcp;
  #tabId = null;
  #baseUrl = undefined;
  #revision = 0;

  constructor() {
    this.#mcp = new McpClient(
      process.env.LADYBIRD_MCP_URL || 'http://127.0.0.1:9223/mcp',
      process.env.LADYBIRD_MCP_TOKEN || '',
    );
  }

  configure(options) {
    if (options && typeof options === 'object') {
      this.#mcp.configure(options.mcpUrl, options.mcpToken);
    }
  }

  async doctor() {
    try {
      const res = await this.#mcp.callTool('list_tabs', {});
      const count = Array.isArray(res?.tabs) ? res.tabs.length : 0;
      return [{ id: 'ladybird-mcp', status: 'pass', message: `Connected to Ladybird MCP at ${this.#mcp.url} (${count} allowed tabs)` }];
    } catch (err) {
      return [{ id: 'ladybird-mcp', status: 'fail', message: err.message }];
    }
  }

  async init(params) {
    this.#baseUrl = params?.app?.origin;
    const opened = await this.#mcp.callTool('open_tab', { activate: true, wait_for_load: false });
    if (typeof opened?.tab_id !== 'number') {
      throw new LadybirdEngineError('ENGINE_FAILURE', 'Ladybird MCP open_tab did not return a numeric tab_id');
    }
    this.#tabId = opened.tab_id;
    return null;
  }

  async startAttempt() {
    if (this.#tabId === null) {
      const opened = await this.#mcp.callTool('open_tab', { activate: true, wait_for_load: false });
      this.#tabId = opened.tab_id;
    }
    return null;
  }

  async endAttempt() {
    return null;
  }

  async dispose() {
    if (this.#tabId !== null) {
      const id = this.#tabId;
      this.#tabId = null;
      await this.#mcp.callTool('close_tab', { tab_id: id }).catch(() => {});
    }
    return null;
  }

  #requireTab() {
    if (this.#tabId === null) {
      throw new LadybirdEngineError('INVALID_STATE', 'No active Ladybird tab in session');
    }
    return this.#tabId;
  }

  async open(url) {
    const tabId = this.#requireTab();
    const resolved = this.#baseUrl ? new URL(url, this.#baseUrl).href : url;
    await this.#mcp.callTool('navigate', { tab_id: tabId, url: resolved, wait_for_load: true });
    return null;
  }

  async back() {
    const tabId = this.#requireTab();
    await this.#mcp.callTool('go_back', { tab_id: tabId });
    return null;
  }

  async restart() {
    const tabId = this.#requireTab();
    await this.#mcp.callTool('navigate', { tab_id: tabId, url: 'about:blank', wait_for_load: false });
    return null;
  }

  async observe(options) {
    const tabId = this.#requireTab();
    const revision = ++this.#revision;
    const evalRes = await this.#mcp.callTool('eval_js', {
      tab_id: tabId,
      script: `arguments[0] = ${JSON.stringify({ revision })};\n${OBSERVE_SCRIPT}`,
    });
    const snapshot = evalRes?.value;
    if (!snapshot || typeof snapshot !== 'object' || !snapshot.root) {
      throw new LadybirdEngineError('ACTION_FAILED', 'Ladybird observe eval_js returned invalid snapshot');
    }
    if (options?.pixels) {
      const shot = await this.#mcp.callTool('take_screenshot', { tab_id: tabId });
      if (shot?.data) {
        snapshot.pixels = {
          data: shot.data,
          mediaType: 'image/png',
          width: snapshot.viewport?.width || 1280,
          height: snapshot.viewport?.height || 720,
          scale: 1,
        };
      }
    }
    return snapshot;
  }

  async perform(ref, action) {
    const tabId = this.#requireTab();
    const evalRes = await this.#mcp.callTool('eval_js', {
      tab_id: tabId,
      script: `arguments[0] = ${JSON.stringify({ ref, action })};\n${PERFORM_SCRIPT}`,
    });
    const outcome = evalRes?.value;
    if (!outcome?.ok) {
      throw new LadybirdEngineError(
        outcome?.code || 'ACTION_FAILED',
        outcome?.error || 'Action failed in Ladybird',
        Boolean(outcome?.retryable),
      );
    }
    return null;
  }

  async keyboardType(text, options) {
    const tabId = this.#requireTab();
    const script = `return (function(cfg) {
      var el = document.activeElement;
      if (!el || (el.tagName !== 'INPUT' && el.tagName !== 'TEXTAREA' && !el.isContentEditable))
        return { ok: false, code: 'NOT_ACTIONABLE', error: 'No focused text input element' };
      var cur = el.isContentEditable ? String(el.textContent || '') : String(el.value || '');
      var next = cfg.replace ? cfg.text : (cur + cfg.text);
      if (el.isContentEditable) el.textContent = next;
      else el.value = next;
      el.dispatchEvent(new Event('input', { bubbles: true }));
      el.dispatchEvent(new Event('change', { bubbles: true }));
      return { ok: true };
    })(${JSON.stringify({ text, replace: Boolean(options?.replace) })});`;
    const res = await this.#mcp.callTool('eval_js', { tab_id: tabId, script });
    if (!res?.value?.ok) {
      throw new LadybirdEngineError(res?.value?.code || 'NOT_ACTIONABLE', res?.value?.error || 'keyboard.type failed');
    }
    return null;
  }

  async keyboardPress(key) {
    const tabId = this.#requireTab();
    const script = `return (function(cfg) {
      var el = document.activeElement || document.body || document.documentElement;
      if (!el) return { ok: false, code: 'ACTION_FAILED', error: 'No document element' };
      var init = { key: cfg.key, bubbles: true, cancelable: true };
      var notCancelled = el.dispatchEvent(new KeyboardEvent('keydown', init));
      if (cfg.key === 'Enter' || cfg.key.length === 1)
        el.dispatchEvent(new KeyboardEvent('keypress', init));
      el.dispatchEvent(new KeyboardEvent('keyup', init));
      if (cfg.key === 'Enter' && notCancelled && el.form) {
        if (typeof el.form.requestSubmit === 'function') el.form.requestSubmit();
        else el.form.submit();
      }
      return { ok: true };
    })(${JSON.stringify({ key })});`;
    const res = await this.#mcp.callTool('eval_js', { tab_id: tabId, script });
    if (!res?.value?.ok) {
      throw new LadybirdEngineError(res?.value?.code || 'ACTION_FAILED', res?.value?.error || 'keyboard.press failed');
    }
    return null;
  }

  async screenshot() {
    const tabId = this.#requireTab();
    const shot = await this.#mcp.callTool('take_screenshot', { tab_id: tabId });
    if (!shot?.data) {
      throw new LadybirdEngineError('ACTION_FAILED', 'Ladybird take_screenshot did not return PNG data');
    }
    return { data: shot.data, mediaType: 'image/png' };
  }
}

function writeFrame(output, message) {
  const json = Buffer.from(JSON.stringify(message), 'utf8');
  const header = Buffer.from(`Content-Length: ${json.length}\r\n\r\n`, 'ascii');
  output.write(Buffer.concat([header, json]));
}

function sendResult(output, id, result) {
  writeFrame(output, { jsonrpc: '2.0', id, result });
}

function sendError(output, id, err) {
  if (err instanceof LadybirdEngineError) {
    writeFrame(output, {
      jsonrpc: '2.0',
      id,
      error: {
        code: RPC_ENGINE_ERROR,
        message: err.message,
        data: {
          type: 'engine',
          code: err.code,
          retryable: err.retryable,
        },
      },
    });
    return;
  }
  writeFrame(output, {
    jsonrpc: '2.0',
    id,
    error: {
      code: err?.rpcCode ?? RPC_INTERNAL_ERROR,
      message: err?.message || String(err),
    },
  });
}

export function startServer(streams = { input: process.stdin, output: process.stdout }) {
  const session = new LadybirdBridgeSession();
  const DELIM = Buffer.from('\r\n\r\n');
  let buffer = Buffer.alloc(0);
  let bodyLength = undefined;
  let closed = false;

  async function handleRequest(msg) {
    const { id, method, params } = msg;
    const isNotification = id === undefined;

    if (method === 'exit') {
      closed = true;
      await session.dispose().catch(() => {});
      if (streams.input === process.stdin) process.exit(0);
      return;
    }
    if (method === 'initialized' || method === '$/abort') {
      return;
    }

    if (isNotification) return;

    try {
      let result = null;
      switch (method) {
        case 'initialize': {
          if (params?.protocolVersion !== ENGINE_PROTOCOL_VERSION) {
            throw Object.assign(new Error(`Unsupported protocolVersion: ${params?.protocolVersion}`), {
              rpcCode: RPC_INVALID_PARAMS,
            });
          }
          session.configure(params?.initializationOptions);
          result = { protocolVersion: ENGINE_PROTOCOL_VERSION, manifest: LADYBIRD_MANIFEST };
          break;
        }
        case 'shutdown':
          result = null;
          break;
        case 'lifecycle/doctor':
          result = await session.doctor();
          break;
        case 'lifecycle/init':
          result = await session.init(params);
          break;
        case 'lifecycle/startAttempt':
          result = await session.startAttempt(params);
          break;
        case 'lifecycle/endAttempt':
          result = await session.endAttempt(params);
          break;
        case 'lifecycle/dispose':
          result = await session.dispose();
          break;
        case 'session/open':
          result = await session.open(params?.url);
          break;
        case 'session/back':
          result = await session.back();
          break;
        case 'session/restart':
          result = await session.restart();
          break;
        case 'engine/observe':
          result = await session.observe(params?.options);
          break;
        case 'engine/perform':
          result = await session.perform(params?.ref, params?.action);
          break;
        case 'keyboard/type':
          result = await session.keyboardType(params?.text ?? '', params?.options);
          break;
        case 'keyboard/press':
          result = await session.keyboardPress(params?.key ?? '');
          break;
        case 'artifacts/screenshot':
          result = await session.screenshot();
          break;
        default:
          throw Object.assign(new Error(`Method not found: ${method}`), { rpcCode: RPC_METHOD_NOT_FOUND });
      }
      if (!closed) sendResult(streams.output, id, result);
    } catch (err) {
      if (!closed) sendError(streams.output, id, err);
    }
  }

  function processBuffer() {
    while (!closed) {
      if (bodyLength === undefined) {
        const idx = buffer.indexOf(DELIM);
        if (idx === -1) return;
        const headerText = buffer.subarray(0, idx).toString('ascii');
        const match = /Content-Length:\s*(\d+)/i.exec(headerText);
        if (!match) {
          buffer = buffer.subarray(idx + DELIM.length);
          continue;
        }
        bodyLength = Number(match[1]);
        buffer = buffer.subarray(idx + DELIM.length);
      }
      if (buffer.length < bodyLength) return;
      const body = buffer.subarray(0, bodyLength).toString('utf8');
      buffer = buffer.subarray(bodyLength);
      bodyLength = undefined;
      try {
        const msg = JSON.parse(body);
        if (!msg || typeof msg !== 'object' || msg.jsonrpc !== '2.0' || typeof msg.method !== 'string') {
          if (msg && msg.id !== undefined) {
            sendError(streams.output, msg.id, Object.assign(new Error('Invalid Request'), { rpcCode: RPC_INVALID_REQUEST }));
          }
          continue;
        }
        void handleRequest(msg);
      } catch (err) {
        process.stderr.write(`[e2e-ladybird-engine] JSON parse error: ${err.message}\n`);
      }
    }
  }

  streams.input.on('data', chunk => {
    const buf = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk, 'utf8');
    buffer = Buffer.concat([buffer, buf]);
    processBuffer();
  });

  streams.input.once('end', () => {
    closed = true;
    void session.dispose().catch(() => {});
  });
}

const isEntrypoint = process.argv[1] && import.meta.url.endsWith(process.argv[1].replace(/\\/g, '/').split('/').slice(-2).join('/'));
if (isEntrypoint || process.argv[1]?.endsWith('server.mjs')) {
  startServer();
}
