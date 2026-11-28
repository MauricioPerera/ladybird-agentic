/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

const META_SELECTOR = 'meta[name="theme-color"]';
let mqBound = false;

function syncMetaThemeColor(mode) {
  const dark =
    mode === 'dark' ||
    (mode === 'system' && typeof window !== 'undefined' && window.matchMedia?.('(prefers-color-scheme: dark)').matches);
  const meta = document.querySelector(META_SELECTOR);
  if (meta) meta.setAttribute('content', dark ? '#0d0d0d' : '#f9f9f7');
}

export function applyTheme(mode = 'system') {
  const norm = mode === 'light' || mode === 'dark' ? mode : 'system';
  if (norm === 'system') {
    delete document.documentElement.dataset.theme;
    try {
      localStorage.removeItem('crm:theme');
    } catch {}
  } else {
    document.documentElement.dataset.theme = norm;
    try {
      localStorage.setItem('crm:theme', norm);
    } catch {}
  }
  syncMetaThemeColor(norm);

  if (!mqBound && typeof window !== 'undefined' && window.matchMedia) {
    mqBound = true;
    const mq = window.matchMedia('(prefers-color-scheme: dark)');
    mq.addEventListener?.('change', () => {
      if (!document.documentElement.dataset.theme) syncMetaThemeColor('system');
    });
  }
}
