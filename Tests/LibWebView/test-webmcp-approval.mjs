import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import { createContext, runInContext } from 'node:vm';

const application = readFileSync(new URL('../../Libraries/LibWebView/Application.cpp', import.meta.url), 'utf8');
const shim = application.match(/mcp_webmcp_shim_js = R"WEBMCP\(([\s\S]*?)\)WEBMCP"sv;/)?.[1];
assert.ok(shim, 'WebMCP shim source');

function createDocument(forms = []) {
  const location = { href: 'https://example.test/flows', origin: 'https://example.test', protocol: 'https:', hostname: 'example.test' };
  const document = { querySelectorAll: (selector) => selector === 'form[toolname]' ? forms : [] };
  const window = { isSecureContext: true, dispatchEvent() {} };
  const navigator = {};
  runInContext(shim, createContext({ document, window, navigator, location }));
  return { api: window.__ladybirdWebMCP, modelContext: navigator.modelContext, location };
}

function register(modelContext, value, replace = false) {
  modelContext.registerTool({
    name: 'sample',
    description: 'Return a sample value',
    inputSchema: { type: 'object' },
    annotations: { consequentialHint: true },
    execute: () => ({ value }),
  }, { replace });
}

const approval = (inspectionTicket) => ({ inspectionTicket, confirmConsequential: true });

test('WebMCP approval is consumed by the matching registration exactly once', async () => {
  const { api, modelContext } = createDocument();
  register(modelContext, 'original');
  assert.equal(api.inspectTool('sample', 'original-ticket').consequential, true);
  assert.equal((await api.callTool('sample', {}, approval('original-ticket'))).result.value, 'original');
  assert.match((await api.callTool('sample', {}, approval('original-ticket'))).error, /changed/);
});

test('WebMCP approval expires when a tool registration is replaced', async () => {
  const { api, modelContext } = createDocument();
  register(modelContext, 'first');
  api.inspectTool('sample', 'first-ticket');
  register(modelContext, 'second', true);
  assert.match((await api.callTool('sample', {}, approval('first-ticket'))).error, /changed/);

  api.inspectTool('sample', 'current-ticket');
  assert.equal((await api.callTool('sample', {}, approval('current-ticket'))).result.value, 'second');
});

test('a new WebMCP document at the same URL requires its own inspection', async () => {
  const before = createDocument();
  register(before.modelContext, 'before');
  before.api.inspectTool('sample', 'before-ticket');

  const after = createDocument();
  register(after.modelContext, 'after');
  assert.equal(before.location.href, after.location.href);
  assert.match((await after.api.callTool('sample', {}, approval('before-ticket'))).error, /changed/);
});

test('WebMCP approval expires when a declarative form changes its submission target', async () => {
  const attributes = { toolname: 'sample', method: 'post', tooldescription: 'Sample form', toolautosubmit: '' };
  let submitted = false;
  const form = {
    getAttribute: (name) => attributes[name] ?? null,
    hasAttribute: (name) => Object.hasOwn(attributes, name),
    elements: [],
    action: 'https://example.test/original',
    target: '',
    enctype: 'application/x-www-form-urlencoded',
    requestSubmit() { submitted = true; },
  };
  const { api } = createDocument([form]);
  api.inspectTool('sample', 'form-ticket');
  form.action = 'https://example.test/changed';
  assert.match((await api.callTool('sample', {}, approval('form-ticket'))).error, /changed/);
  assert.equal(submitted, false);
});
