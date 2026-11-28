/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

export function el(tagSpec, props = {}, ...children) {
  const [tag, ...classes] = String(tagSpec).split('.');
  const node = document.createElement(tag || 'div');
  if (classes.length) node.className = classes.join(' ');

  for (const [key, val] of Object.entries(props || {})) {
    if (val === undefined || val === null || val === false) continue;
    const lower = key.toLowerCase();
    if (key === 'className' || key === 'class') {
      node.className = classes.length ? `${node.className} ${val}` : val;
    } else if (key === 'dataset' && typeof val === 'object') {
      for (const [dk, dv] of Object.entries(val)) node.dataset[dk] = dv;
    } else if (lower.startsWith('on')) {
      if (typeof val === 'function') {
        node.addEventListener(key.slice(2).toLowerCase(), val);
      }
    } else if (lower === 'html' || lower === 'srcdoc') {
      continue;
    } else if (key === 'value' || key === 'checked' || key === 'disabled' || key === 'selected' || key === 'multiple') {
      node[key] = val;
    } else {
      node.setAttribute(key, val === true ? '' : String(val));
    }
  }

  append(node, children);
  return node;
}

export function append(parent, children) {
  for (const child of Array.isArray(children) ? children : [children]) {
    if (child === null || child === undefined || child === false) continue;
    if (Array.isArray(child)) {
      append(parent, child);
    } else if (child instanceof Node) {
      parent.appendChild(child);
    } else {
      parent.appendChild(document.createTextNode(String(child)));
    }
  }
}

export function clear(node) {
  node.replaceChildren();
}

export function keepingFocus(renderFn) {
  const active = document.activeElement;
  const focusKey = active?.getAttribute?.('data-focus-key');
  const selStart = typeof active?.selectionStart === 'number' ? active.selectionStart : null;
  const selEnd = typeof active?.selectionEnd === 'number' ? active.selectionEnd : null;

  const result = renderFn();

  if (focusKey) {
    const esc = typeof CSS !== 'undefined' && CSS.escape ? CSS.escape(focusKey) : focusKey.replace(/"/g, '\\"');
    const next = document.querySelector(`[data-focus-key="${esc}"]`);
    if (next && typeof next.focus === 'function') {
      next.focus({ preventScroll: true });
      if (selStart !== null && typeof next.setSelectionRange === 'function') {
        try {
          next.setSelectionRange(selStart, selEnd ?? selStart);
        } catch {}
      }
    }
  }
  return result;
}
