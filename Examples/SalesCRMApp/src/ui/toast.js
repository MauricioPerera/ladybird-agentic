/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { el } from './dom.js';

let stack = null;

function ensureStack() {
  if (!stack || !stack.isConnected) {
    stack = el('div.toast-stack', { 'aria-live': 'polite' });
    document.body.appendChild(stack);
  }
  return stack;
}

export function toast(message, { tone = 'info', duration = 4500, action = null, onAction = null } = {}) {
  const root = ensureStack();
  const node = el('div', { class: `toast${tone === 'warn' ? ' warn' : tone === 'danger' ? ' danger' : ''}` });
  node.appendChild(el('span', {}, message));

  let timer = 0;
  const dismiss = () => {
    if (timer) clearTimeout(timer);
    node.remove();
  };

  if (action && typeof onAction === 'function') {
    node.appendChild(
      el(
        'button.toast-action',
        {
          type: 'button',
          onclick: () => {
            dismiss();
            onAction();
          },
        },
        action
      )
    );
  }

  root.appendChild(node);
  if (duration > 0) timer = setTimeout(dismiss, duration);
  return dismiss;
}
