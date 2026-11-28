/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from '../lib/i18n.js';
import { cleanTag, MAX_TAGS_PER_RECORD, normalizeTags, tagKey } from '../lib/tags.js';
import { el } from './dom.js';
import { icon } from './icons.js';

export function tagInputControl(initialTags = [], suggestions = [], onChange = null) {
  let tags = normalizeTags(initialTags);
  const root = el('div.tag-editor');
  const input = el('input', { type: 'text', placeholder: t('Add tag and press Enter…') });

  const render = () => {
    const chips = tags.map((tg) =>
      el(
        'span.tag-chip',
        {},
        tg,
        el(
          'button.icon-btn',
          {
            type: 'button',
            style: 'width:18px;height:18px',
            'aria-label': t('Remove {tag}', { tag: tg }),
            onclick: () => {
              const k = tagKey(tg);
              tags = tags.filter((x) => tagKey(x) !== k);
              render();
              onChange?.(tags);
            },
          },
          icon('x', 11)
        )
      )
    );
    root.replaceChildren(...chips, input);
  };

  const commit = (raw) => {
    const clean = cleanTag(raw);
    if (!clean) return;
    if (tags.length >= MAX_TAGS_PER_RECORD) return;
    const next = normalizeTags([...tags, clean]);
    if (next.length !== tags.length) {
      tags = next;
      onChange?.(tags);
    }
    input.value = '';
    render();
  };

  input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' || e.key === ',') {
      e.preventDefault();
      commit(input.value);
    } else if (e.key === 'Backspace' && !input.value && tags.length > 0) {
      tags = tags.slice(0, -1);
      render();
      onChange?.(tags);
    }
  });
  input.addEventListener('blur', () => {
    if (input.value.trim()) commit(input.value);
  });

  if (suggestions.length > 0) {
    const listId = `tags-${Math.random().toString(36).slice(2, 8)}`;
    input.setAttribute('list', listId);
    const dl = el('datalist', { id: listId }, ...suggestions.map((s) => el('option', { value: s })));
    root.dataset.listId = listId;
    setTimeout(() => root.parentElement?.appendChild(dl), 0);
  }

  render();
  return {
    el: root,
    getTags: () => [...tags],
  };
}

export function tagFilterControl(allTags, selectedTags, onChange, focusKey = 'tag-filter') {
  const select = el(
    'select.select.select-sm',
    {
      'data-focus-key': focusKey,
      'aria-label': t('Filter by tag'),
      onchange: () => {
        const val = select.value;
        if (!val) return;
        const next = normalizeTags([...selectedTags, val]);
        select.value = '';
        onChange(next);
      },
    },
    el('option', { value: '' }, t('+ Tag filter')),
    ...allTags
      .filter((tg) => !selectedTags.some((s) => tagKey(s) === tagKey(tg)))
      .map((tg) => el('option', { value: tg }, `#${tg}`))
  );

  const active = selectedTags.map((tg) =>
    el(
      'button.pill.pill-accent',
      {
        type: 'button',
        title: t('Remove tag filter'),
        onclick: () => onChange(selectedTags.filter((x) => tagKey(x) !== tagKey(tg))),
      },
      `#${tg} ×`
    )
  );

  return el('div.chip-row', {}, ...active, allTags.length ? select : null);
}
