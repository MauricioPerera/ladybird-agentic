/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

export const HEAL_INSPECT_SCRIPT = `return (() => {
  const clean = (s) => String(s || '').replace(/\\s+/g, ' ').trim();
  const out = [];
  const els = document.querySelectorAll('button, a[href], input, textarea, select, [role], [data-testid], [data-test-id], [aria-label], tr.zA, h1, h2, h3');
  for (let i = 0; i < els.length && out.length < 120; i++) {
    const el = els[i];
    const rect = el.getBoundingClientRect ? el.getBoundingClientRect() : { width: 1, height: 1 };
    if (rect.width <= 0 && rect.height <= 0) continue;
    const tag = el.tagName.toLowerCase();
    const role = el.getAttribute('role') || (tag === 'button' ? 'button' : tag === 'a' ? 'link' : '');
    const ariaLabel = clean(el.getAttribute('aria-label') || '');
    const testId = el.getAttribute('data-testid') || el.getAttribute('data-test-id') || '';
    const placeholder = clean(el.getAttribute('placeholder') || '');
    let labelText = '';
    if (el.id && document.labels) {
      for (const lbl of document.labels) {
        if (lbl.control === el) { labelText = clean(lbl.textContent); break; }
      }
    }
    const text = clean(el.innerText || el.value || '').slice(0, 64);
    const id = el.id && !/^\\d+$/.test(el.id) ? el.id : '';
    const name = el.getAttribute('name') || '';
    const cls = Array.from(el.classList || []).filter((c) => c && !/^[a-zA-Z0-9]{1,3}\\d+$/.test(c)).slice(0, 2);
    const css = id ? '#' + id : name ? tag + '[name="' + name.replace(/"/g, '\\\\"') + '"]' : cls.length ? tag + '.' + cls.join('.') : tag;
    out.push({ tag, role, ariaLabel, labelText, placeholder, testId, id, name, text, css });
  }
  return out;
})();`;

function tokenize(str) {
  return String(str || '')
    .toLowerCase()
    .split(/[^a-z0-9]+/)
    .filter((tok) => tok.length >= 2 && !['btn', 'button', 'div', 'span', 'input', 'css', 'role', 'true'].includes(tok));
}

function bestLocatorForCandidate(c) {
  if (c.testId) return { locator: { kind: 'test_id', value: c.testId, exact: true }, bonus: 20, reason: 'data-testid' };
  const roleName = c.ariaLabel || c.text;
  if (c.role && roleName && roleName.length <= 48) {
    return { locator: { kind: 'role', role: c.role, name: roleName, exact: false }, bonus: 16, reason: 'role+name' };
  }
  if (c.labelText) return { locator: { kind: 'label', value: c.labelText, exact: false }, bonus: 14, reason: 'label' };
  if (c.placeholder) return { locator: { kind: 'placeholder', value: c.placeholder, exact: false }, bonus: 12, reason: 'placeholder' };
  if (c.text && c.text.length <= 40) return { locator: { kind: 'text', value: c.text, exact: false }, bonus: 10, reason: 'text' };
  return { locator: { kind: 'css', value: c.css || c.tag || 'body', exact: false }, bonus: 5, reason: 'css' };
}

export function rankHealCandidates(brokenStep, candidates = []) {
  const brokenLoc = brokenStep?.locator || {};
  const queryTokens = new Set([
    ...tokenize(brokenLoc.value),
    ...tokenize(brokenLoc.name),
    ...tokenize(brokenLoc.role),
    ...tokenize(brokenStep?.label),
    ...tokenize(brokenStep?.contains),
  ]);

  const action = brokenStep?.action || 'click';
  const scored = [];
  const seenKey = new Set();

  for (const c of candidates || []) {
    if (!c) continue;
    const candText = [c.testId, c.ariaLabel, c.labelText, c.placeholder, c.text, c.id, c.name, c.css, c.role].join(' ').toLowerCase();
    let score = 0;
    const matchedTokens = [];
    for (const tok of queryTokens) {
      if (candText.includes(tok)) {
        score += 18;
        matchedTokens.push(tok);
      }
    }

    const tag = String(c.tag || '').toLowerCase();
    if (action === 'fill' && ['input', 'textarea', 'select'].includes(tag)) score += 12;
    else if (action === 'click' && (tag === 'button' || tag === 'a' || c.role === 'button' || c.role === 'link')) score += 10;

    if (brokenLoc.role && c.role === brokenLoc.role) score += 10;

    const { locator, bonus, reason } = bestLocatorForCandidate(c);
    score += bonus;

    if (queryTokens.size > 0 && matchedTokens.length === 0 && score < 22) continue;

    const dedupe = `${locator.kind}|${locator.value || ''}|${locator.role || ''}|${locator.name || ''}`;
    if (seenKey.has(dedupe)) continue;
    seenKey.add(dedupe);

    scored.push({
      locator,
      score,
      reason: matchedTokens.length ? `${reason} (${matchedTokens.slice(0, 3).join(', ')})` : reason,
      previewText: c.ariaLabel || c.text || c.placeholder || c.css || c.tag,
    });
  }

  return scored.sort((a, b) => b.score - a.score).slice(0, 5);
}
