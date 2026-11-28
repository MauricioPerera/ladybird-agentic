/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { formatBytes, readAttachmentBlob, uploadAttachment } from '../lib/files.js';
import { t } from '../lib/i18n.js';
import { deletedToast } from './bits.js';
import { el } from './dom.js';
import { icon } from './icons.js';
import { confirmDialog } from './modal.js';
import { toast } from './toast.js';

export function attachmentsSection(store, kind, ownerId) {
  const input = el('input', { type: 'file', hidden: true });
  const uploadBtn = el('button.btn.btn-sm', { type: 'button' }, icon('upload', 13), ` ${t('Attach')}`);
  const listEl = el('div.row-list');

  uploadBtn.addEventListener('click', () => input.click());
  input.addEventListener('change', async () => {
    const file = input.files?.[0];
    if (!file) return;
    uploadBtn.disabled = true;
    uploadBtn.replaceChildren(t('Uploading…'));
    try {
      const saved = await uploadAttachment(file);
      const fk = kind === 'contact' ? 'contactId' : kind === 'company' ? 'companyId' : 'dealId';
      await store.addAttachment({ ...saved, [fk]: ownerId });
      toast(t('Attached {name}', { name: saved.name }));
    } catch (err) {
      toast(err.message || t('Upload failed'), { tone: 'danger' });
    } finally {
      uploadBtn.disabled = false;
      uploadBtn.replaceChildren(icon('upload', 13), ` ${t('Attach')}`);
      input.value = '';
    }
  });

  const files = store.attachmentsFor(kind, ownerId);
  if (files.length === 0) {
    listEl.appendChild(el('div.muted', {}, t('No files attached (up to 25 MB via /__ladybird_fs).')));
  } else {
    for (const att of files) {
      listEl.appendChild(
        el(
          'div.list-row',
          {},
          el(
            'div.row-main',
            {},
            el('span', {}, icon('paperclip', 14)),
            el(
              'div',
              { style: 'min-width:0;flex:1' },
              el('div.row-title.truncate', {}, att.name),
              el('div.row-meta', {}, formatBytes(att.size))
            )
          ),
          el(
            'div.chip-row',
            {},
            el('button.icon-btn', {
              type: 'button',
              title: t('Download'),
              'aria-label': t('Download {name}', { name: att.name }),
              onclick: async () => {
                try {
                  const blob = await readAttachmentBlob(att.path, att.type);
                  const url = URL.createObjectURL(blob);
                  const a = el('a', { href: url, download: att.name });
                  document.body.appendChild(a);
                  a.click();
                  a.remove();
                  setTimeout(() => URL.revokeObjectURL(url), 1000);
                } catch (err) {
                  toast(err.message || t('Download failed'), { tone: 'danger' });
                }
              },
            }, icon('download', 14)),
            el('button.icon-btn', {
              type: 'button',
              title: t('Delete file'),
              'aria-label': t('Delete {name}', { name: att.name }),
              onclick: async () => {
                if (!(await confirmDialog(t('Remove attachment "{name}"?', { name: att.name }), { confirmLabel: t('Remove'), tone: 'danger' }))) return;
                const entry = await store.deleteAttachment(att.id);
                deletedToast(store, entry, t('Attachment'));
              },
            }, icon('trash', 14))
          )
        )
      );
    }
  }

  return el('div.card', {}, el('div.card-head', {}, el('h3.card-title', {}, t('Files')), uploadBtn, input), listEl);
}
