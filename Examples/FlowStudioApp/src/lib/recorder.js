/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

export const RECORDER_START_SCRIPT = `return (() => {
  if (window.__fsRecorder && window.__fsRecorder.active) {
    return { active: true, already: true };
  }
  const state = { active: true, events: [] };

  const badge = document.createElement('div');
  badge.id = '__fs_rec_badge';
  badge.style.cssText = 'position:fixed;bottom:14px;right:14px;z-index:2147483647;background:#111827;color:#f8fafc;border:2px solid #ef4444;border-radius:999px;padding:7px 14px;font:600 12px/1.3 system-ui,sans-serif;box-shadow:0 8px 24px rgba(0,0,0,.4);pointer-events:none;display:flex;align-items:center;gap:8px';
  const dot = document.createElement('span');
  dot.style.cssText = 'width:8px;height:8px;border-radius:50%;background:#ef4444;display:inline-block';
  const txt = document.createElement('span');
  txt.textContent = 'Flow Studio Recording · Click / Type / Alt+Click=Extract';
  badge.appendChild(dot);
  badge.appendChild(txt);

  const highlight = document.createElement('div');
  highlight.id = '__fs_rec_hl';
  highlight.style.cssText = 'position:fixed;z-index:2147483646;pointer-events:none;border:2px solid #6366f1;background:rgba(99,102,241,0.12);border-radius:4px;display:none';

  document.documentElement.appendChild(badge);
  document.documentElement.appendChild(highlight);

  function cleanText(s) {
    return String(s || '').replace(/\\s+/g, ' ').trim();
  }

  function cssEscapeVal(v) {
    return String(v || '').replace(/\\\\/g, '\\\\\\\\').replace(/"/g, '\\\\"');
  }

  function buildSemanticLocator(el) {
    if (!el || el.nodeType !== 1) return { kind: 'css', value: 'body', exact: false };
    const testId = el.getAttribute('data-testid') || el.getAttribute('data-test-id');
    if (testId) return { kind: 'test_id', value: testId, exact: true };

    const explicitRole = el.getAttribute('role');
    const tag = el.tagName.toLowerCase();
    const implicitRole = tag === 'button' ? 'button' : tag === 'a' && el.hasAttribute('href') ? 'link' : tag === 'input' && (el.type === 'submit' || el.type === 'button') ? 'button' : '';
    const role = explicitRole || implicitRole;
    const ariaName = cleanText(el.getAttribute('aria-label') || '');
    const innerName = cleanText(el.innerText || el.value || '');
    if (role && (ariaName || (innerName && innerName.length <= 48))) {
      return { kind: 'role', role, name: ariaName || innerName, exact: false };
    }

    if (el.id && document.labels) {
      for (const lbl of document.labels) {
        if (lbl.control === el) {
          const lt = cleanText(lbl.textContent);
          if (lt && lt.length <= 48) return { kind: 'label', value: lt, exact: false };
        }
      }
    }
    const ph = cleanText(el.getAttribute('placeholder'));
    if (ph && ph.length <= 48) return { kind: 'placeholder', value: ph, exact: false };

    if ((tag === 'button' || tag === 'a' || el.getAttribute('role')) && innerName && innerName.length <= 40) {
      return { kind: 'text', value: innerName, exact: false };
    }

    if (el.id && !/^\\d+$/.test(el.id) && !el.id.startsWith(':')) {
      return { kind: 'css', value: '#' + el.id, exact: false };
    }
    const nameAttr = el.getAttribute('name');
    if (nameAttr) {
      return { kind: 'css', value: tag + '[name="' + cssEscapeVal(nameAttr) + '"]', exact: false };
    }
    const parts = [];
    let cur = el;
    for (let depth = 0; cur && cur.nodeType === 1 && depth < 4 && cur !== document.body; depth++) {
      let seg = cur.tagName.toLowerCase();
      const cls = Array.from(cur.classList || []).filter((c) => c && !/^[a-zA-Z0-9]{1,3}\\d+$/.test(c)).slice(0, 2);
      if (cls.length) seg += '.' + cls.join('.');
      parts.unshift(seg);
      cur = cur.parentElement;
    }
    return { kind: 'css', value: parts.join(' > ') || tag, exact: false };
  }

  function isOwnUi(target) {
    return target === badge || target === highlight || badge.contains(target);
  }

  const onMove = (e) => {
    const t = e.target;
    if (!t || t.nodeType !== 1 || isOwnUi(t)) {
      highlight.style.display = 'none';
      return;
    }
    const r = t.getBoundingClientRect();
    highlight.style.left = r.left + 'px';
    highlight.style.top = r.top + 'px';
    highlight.style.width = r.width + 'px';
    highlight.style.height = r.height + 'px';
    highlight.style.display = r.width > 0 && r.height > 0 ? 'block' : 'none';
  };

  const onClick = (e) => {
    const t = e.target;
    if (!t || t.nodeType !== 1 || isOwnUi(t)) return;
    const locator = buildSemanticLocator(t);
    if (e.altKey) {
      e.preventDefault();
      e.stopPropagation();
      state.events.push({ type: 'extract', locator, tag: t.tagName.toLowerCase(), preview: cleanText(t.innerText || t.textContent).slice(0, 40) });
      return;
    }
    state.events.push({ type: 'click', locator, tag: t.tagName.toLowerCase() });
  };

  const onChange = (e) => {
    const t = e.target;
    if (!t || t.nodeType !== 1 || isOwnUi(t)) return;
    if (!['INPUT', 'TEXTAREA', 'SELECT'].includes(t.tagName)) return;
    const locator = buildSemanticLocator(t);
    const isSecret = String(t.type || '').toLowerCase() === 'password';
    state.events.push({
      type: 'fill',
      locator,
      text: isSecret ? '{{secrets.PASSWORD}}' : String(t.value ?? ''),
    });
  };

  const onKeyDown = (e) => {
    if (!['Enter', 'Escape', 'Tab'].includes(e.key)) return;
    const t = e.target;
    if (!t || t.nodeType !== 1 || isOwnUi(t)) return;
    state.events.push({ type: 'press', key: e.key });
  };

  document.addEventListener('mousemove', onMove, true);
  document.addEventListener('click', onClick, true);
  document.addEventListener('change', onChange, true);
  document.addEventListener('input', onChange, true);
  document.addEventListener('keydown', onKeyDown, true);

  state.cleanup = () => {
    document.removeEventListener('mousemove', onMove, true);
    document.removeEventListener('click', onClick, true);
    document.removeEventListener('change', onChange, true);
    document.removeEventListener('input', onChange, true);
    document.removeEventListener('keydown', onKeyDown, true);
    badge.remove();
    highlight.remove();
    state.active = false;
  };
  window.__fsRecorder = state;
  return { active: true, started: true };
})();`;

export const RECORDER_DRAIN_SCRIPT = `return (() => {
  const r = window.__fsRecorder;
  if (!r || !r.active) return { active: false, events: [] };
  const events = r.events.splice(0, r.events.length);
  return { active: true, events };
})();`;

export const RECORDER_STOP_SCRIPT = `return (() => {
  const r = window.__fsRecorder;
  if (r && typeof r.cleanup === 'function') r.cleanup();
  delete window.__fsRecorder;
  return { stopped: true };
})();`;

function locatorKey(loc) {
  if (!loc) return '';
  return `${loc.kind}|${loc.value || ''}|${loc.role || ''}|${loc.name || ''}`;
}

export function normalizeRecordedEvents(rawEvents = [], serviceId = 'mail') {
  const steps = [];
  let extractCount = 0;

  for (const ev of rawEvents || []) {
    if (!ev || !ev.type) continue;
    if (ev.type === 'fill') {
      const key = locatorKey(ev.locator);
      const prev = steps[steps.length - 1];
      if (prev && prev.action === 'fill' && locatorKey(prev.locator) === key) {
        prev.text = ev.text ?? '';
        continue;
      }
      steps.push({
        kind: 'step',
        action: 'fill',
        serviceId,
        label: `Fill ${ev.locator?.name || ev.locator?.value || 'input'}`.slice(0, 48),
        locator: ev.locator,
        text: ev.text ?? '',
        clear: true,
        runWhen: 'always',
        retryCount: 0,
        retryDelayMs: 1500,
        onError: 'stop',
      });
    } else if (ev.type === 'click') {
      steps.push({
        kind: 'step',
        action: 'click',
        serviceId,
        label: `Click ${ev.locator?.name || ev.locator?.value || ev.tag || 'element'}`.slice(0, 48),
        locator: ev.locator,
        runWhen: 'always',
        retryCount: 0,
        retryDelayMs: 1500,
        onError: 'stop',
      });
    } else if (ev.type === 'extract') {
      extractCount++;
      steps.push({
        kind: 'step',
        action: 'extract',
        serviceId,
        label: `Extract ${ev.preview || ev.locator?.value || extractCount}`.slice(0, 48),
        locator: ev.locator,
        save_as: `extracted_${extractCount}`,
        runWhen: 'always',
        retryCount: 0,
        retryDelayMs: 1500,
        onError: 'stop',
      });
    } else if (ev.type === 'press') {
      steps.push({
        kind: 'step',
        action: 'press',
        serviceId,
        label: `Press ${ev.key || 'Enter'}`,
        key: ev.key || 'Enter',
        runWhen: 'always',
        retryCount: 0,
        retryDelayMs: 1500,
        onError: 'stop',
      });
    }
  }
  return steps;
}
