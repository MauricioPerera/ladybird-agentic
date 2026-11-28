/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

const SVG_NS = 'http://www.w3.org/2000/svg';

export function el(tagSpec, attrs = {}, ...children) {
  const parts = String(tagSpec || 'div').split('.');
  const tag = parts[0] || 'div';
  const node = document.createElement(tag);
  if (parts.length > 1) node.className = parts.slice(1).join(' ');
  if (attrs && typeof attrs === 'object') {
    for (const [k, v] of Object.entries(attrs)) {
      if (v === undefined || v === null || v === false) continue;
      if (k === 'class') node.className = node.className ? `${node.className} ${v}` : String(v);
      else if (k === 'style') node.setAttribute('style', String(v));
      else if (k.startsWith('on') && typeof v === 'function') node.addEventListener(k.slice(2).toLowerCase(), v);
      else if (k in node && k !== 'list' && k !== 'form') {
        try { node[k] = v; } catch { node.setAttribute(k, v === true ? '' : String(v)); }
      } else {
        node.setAttribute(k, v === true ? '' : String(v));
      }
    }
  }
  const appendChild = (c) => {
    if (c === undefined || c === null || c === false) return;
    if (Array.isArray(c)) c.forEach(appendChild);
    else if (c instanceof Node) node.appendChild(c);
    else node.appendChild(document.createTextNode(String(c)));
  };
  children.forEach(appendChild);
  return node;
}

export function svgEl(tag, attrs = {}, ...children) {
  const node = document.createElementNS(SVG_NS, tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === undefined || v === null) continue;
    node.setAttribute(k, String(v));
  }
  for (const c of children) {
    if (c instanceof Node) node.appendChild(c);
  }
  return node;
}

const ICON_PATHS = {
  flow: 'M5 6h5v5H5zM14 6h5v5h-5zM14 14h5v5h-5zM10 8.5h4M10 8.5c2 0 2 8 4 8',
  bolt: 'M13 2L4 14h7l-1 8 10-12h-7z',
  activity: 'M3 12h4l2.5-6 4 12 2.5-6H21',
  play: 'M7 5v14l11-7z',
  pack: 'M4 7l8-4 8 4-8 4-8-4zm0 5l8 4 8-4M4 17l8 4 8-4',
  plus: 'M12 5v14M5 12h14',
  save: 'M5 4h11l3 3v13H5zm3 0v5h7V4M8 20v-6h8v6',
  tab: 'M4 5h16v14H4zm5 0v4h11',
  trash: 'M4 7h16M9 7V4h6v3M7 7l1 13h8l1-13',
  download: 'M12 4v11m0 0l-4-4m4 4l4-4M4 20h16',
  x: 'M6 6l12 12M18 6L6 18',
  stop: 'M6 6h12v12H6z',
  pin: 'M12 17v5M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16h14v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V7a1 1 0 0 1 1-1 1 1 0 0 0 0-2H8a1 1 0 0 0 0 2 1 1 0 0 1 1 1z',
  refresh: 'M21 12a9 9 0 1 1-3-6.7L21 8m0-5v5h-5',
  lock: 'M7 11V7a5 5 0 0 1 10 0v4M5 11h14v10H5z',
  shield: 'M12 2l8 4v6c0 5.25-3.4 9.74-8 11-4.6-1.26-8-5.75-8-11V6z',
  record: 'M12 12m-6 0a6 6 0 1 0 12 0a6 6 0 1 0-12 0',
  clock: 'M12 6v6l4 2M12 22a10 10 0 1 0 0-20 10 10 0 0 0 0 20z',
  upload: 'M12 16V5m0 0l-4 4m4-4l4 4M4 20h16',
  wrench: 'M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94l-3.76 3.76z',
  webhook: 'M18 16.98h-5.99c-1.1 0-1.95.94-2.48 1.9A4 4 0 0 1 2 17c.01-.7.2-1.4.57-2M6 17a4 4 0 0 1 7.26-2.36L16 10a4 4 0 1 0-7.74-1.36',
  check: 'M20 6L9 17l-5-5',
};

export function icon(name, size = 15) {
  const d = ICON_PATHS[name] || ICON_PATHS.flow;
  return svgEl(
    'svg',
    { width: size, height: size, viewBox: '0 0 24 24', fill: 'none', stroke: 'currentColor', 'stroke-width': 2, 'stroke-linecap': 'round', 'stroke-linejoin': 'round', 'aria-hidden': 'true' },
    svgEl('path', { d })
  );
}

let toastRack = null;
export function toast(message, opts = {}) {
  if (typeof document === 'undefined') return;
  if (!toastRack) {
    toastRack = el('div.toast-rack');
    document.body.appendChild(toastRack);
  }
  const toneClass = opts.tone ? ` tone-${opts.tone}` : '';
  const item = el(
    `div.toast${toneClass}`,
    { role: 'status' },
    el('span', {}, String(message || '')),
    opts.action
      ? el('button.btn.btn-sm.btn-primary', { type: 'button', onclick: () => { item.remove(); opts.onAction?.(); } }, opts.action)
      : null
  );
  toastRack.appendChild(item);
  setTimeout(() => item.remove(), opts.duration || 4000);
}

export function openModal({ title, body, foot = null }) {
  const backdrop = el('div.modal-backdrop');
  const close = () => backdrop.remove();
  backdrop.addEventListener('click', (e) => { if (e.target === backdrop) close(); });
  const card = el(
    'div.modal-card',
    { role: 'dialog', 'aria-modal': 'true' },
    el('div.modal-head', {}, el('span', {}, title || ''), el('button.btn.btn-sm', { type: 'button', onclick: close }, icon('x', 13))),
    el('div.modal-body', {}, body),
    foot ? el('div.modal-foot', {}, foot) : null
  );
  backdrop.appendChild(card);
  document.body.appendChild(backdrop);
  return { close };
}
