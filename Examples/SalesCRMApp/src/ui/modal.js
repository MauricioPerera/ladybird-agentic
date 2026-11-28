/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from '../lib/i18n.js';
import { el } from './dom.js';
import { icon } from './icons.js';

const openDialogs = [];
let modalSeq = 0;

function focusablesIn(container) {
  return [
    ...container.querySelectorAll(
      'a[href], button:not([disabled]), input:not([disabled]):not([type="hidden"]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])'
    ),
  ].filter((n) => !n.hidden && (n.offsetParent !== null || n.getClientRects().length > 0));
}

function updateInert() {
  const active = openDialogs.length > 0;
  const app = document.getElementById('app');
  if (app) app.inert = active;
  const toastStack = document.querySelector('.toast-stack');
  if (toastStack) toastStack.inert = active;
  for (let i = 0; i < openDialogs.length; i++) {
    openDialogs[i].root.inert = i < openDialogs.length - 1;
  }
}

function snapshotControls(container) {
  const vals = [];
  for (const node of container.querySelectorAll('input, select, textarea, .tag-chip, [role="switch"]')) {
    if (node.classList?.contains('tag-chip')) vals.push(`chip:${node.textContent}`);
    else if (node.getAttribute('role') === 'switch') vals.push(`sw:${node.getAttribute('aria-checked')}`);
    else if (node.type === 'checkbox' || node.type === 'radio') vals.push(`${node.name}:${node.checked}`);
    else vals.push(`${node.name || node.id}:${node.value}`);
  }
  return JSON.stringify(vals);
}

export function hasOpenModal() {
  return openDialogs.length > 0;
}

export function closeAllModals() {
  while (openDialogs.length > 0) {
    openDialogs[openDialogs.length - 1].forceClose();
  }
}

export function openModal({ title = '', body, foot = null, wide = false, focusFirst = 'desktop', guardUnsaved = false, onClose = null } = {}) {
  const prevFocus = document.activeElement;
  const prevFocusKey = prevFocus?.getAttribute?.('data-focus-key') || '';
  const titleId = `modal-title-${++modalSeq}`;
  let initialSnap = null;
  let closing = false;

  const card = el('div', {
    class: `modal-card${wide ? ' wide' : ''}`,
    role: 'dialog',
    'aria-modal': 'true',
    'aria-labelledby': titleId,
    tabindex: '-1',
  });
  const closeBtn = el('button.icon-btn', { type: 'button', 'aria-label': t('Close') }, icon('x', 16));
  card.appendChild(el('div.modal-head', {}, el('h2.modal-title', { id: titleId }, title), closeBtn));

  const bodyEl = el('div.modal-body');
  if (typeof body === 'function') body(bodyEl);
  else if (body) bodyEl.appendChild(body);
  card.appendChild(bodyEl);

  if (foot) {
    const footEl = el('div.modal-foot');
    if (typeof foot === 'function') foot(footEl);
    else footEl.appendChild(foot);
    card.appendChild(footEl);
  }

  const root = el('div.modal-root', {}, card);

  const forceClose = () => {
    if (closing) return;
    closing = true;
    root.classList.add('closing');
    document.removeEventListener('keydown', onKey);
    const idx = openDialogs.findIndex((d) => d.root === root);
    if (idx >= 0) openDialogs.splice(idx, 1);
    root.remove();
    updateInert();
    if (prevFocus && typeof prevFocus.focus === 'function' && prevFocus.isConnected) {
      prevFocus.focus({ preventScroll: true });
    } else if (prevFocusKey) {
      const esc = typeof CSS !== 'undefined' && CSS.escape ? CSS.escape(prevFocusKey) : prevFocusKey.replace(/"/g, '\\"');
      document.querySelector(`[data-focus-key="${esc}"]`)?.focus?.({ preventScroll: true });
    }
    if (typeof onClose === 'function') onClose();
  };

  const requestClose = async () => {
    if (closing) return;
    if (guardUnsaved && initialSnap !== null && snapshotControls(card) !== initialSnap) {
      const discard = await confirmDialog(t('Discard your changes?'), { confirmLabel: t('Discard'), tone: 'danger' });
      if (!discard) return;
    }
    forceClose();
  };

  const onKey = (e) => {
    if (openDialogs[openDialogs.length - 1]?.root !== root) return;
    if (e.key === 'Escape') {
      e.preventDefault();
      requestClose();
    } else if (e.key === 'Tab') {
      const items = focusablesIn(card);
      if (items.length === 0) {
        e.preventDefault();
        card.focus({ preventScroll: true });
        return;
      }
      const first = items[0];
      const last = items[items.length - 1];
      if (e.shiftKey && (document.activeElement === first || !card.contains(document.activeElement))) {
        e.preventDefault();
        last.focus({ preventScroll: true });
      } else if (!e.shiftKey && (document.activeElement === last || !card.contains(document.activeElement))) {
        e.preventDefault();
        first.focus({ preventScroll: true });
      }
    }
  };

  closeBtn.addEventListener('click', requestClose);
  root.addEventListener('click', (e) => {
    if (e.target === root) requestClose();
  });
  document.addEventListener('keydown', onKey);

  openDialogs.push({ root, forceClose, requestClose });
  document.body.appendChild(root);
  updateInert();

  if (guardUnsaved) {
    requestAnimationFrame(() => {
      initialSnap = snapshotControls(card);
    });
  }

  const shouldFocus = focusFirst === true || (focusFirst === 'desktop' && window.innerWidth >= 640);
  requestAnimationFrame(() => {
    if (shouldFocus) {
      const first = card.querySelector('input:not([type="hidden"]), select, textarea, button.btn-primary');
      (first || card)?.focus?.({ preventScroll: true });
    } else {
      card.focus({ preventScroll: true });
    }
  });

  return { root, card, bodyEl, close: forceClose, requestClose };
}

export function confirmDialog(message, { title = t('Confirm'), confirmLabel = t('Confirm'), cancelLabel = t('Cancel'), tone = 'primary' } = {}) {
  return new Promise((resolve) => {
    let settled = false;
    const done = (val) => {
      if (settled) return;
      settled = true;
      dlg.close();
      resolve(val);
    };
    const dlg = openModal({
      title,
      focusFirst: true,
      body: el('p', { style: 'margin:0' }, message),
      foot: el(
        'div.btn-group',
        { style: 'display:flex;gap:8px' },
        el('button.btn', { type: 'button', onclick: () => done(false) }, cancelLabel),
        el('button', { type: 'button', class: `btn ${tone === 'danger' ? 'btn-danger' : 'btn-primary'}`, onclick: () => done(true) }, confirmLabel)
      ),
      onClose: () => {
        if (!settled) {
          settled = true;
          resolve(false);
        }
      },
    });
  });
}
