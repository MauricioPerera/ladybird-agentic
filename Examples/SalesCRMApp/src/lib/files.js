/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { normalizeText, uid } from './util.js';

export const MAX_FILE_BYTES = 25 * 1024 * 1024;
export const ATTACHMENT_DIR = 'attachments';
export const APP_FILE_PATH = /^attachments\/[A-Za-z0-9][A-Za-z0-9._-]*$/;
const UNSAFE_MIME_RE = /^(?:text\/html|application\/xhtml\+xml|image\/svg\+xml|text\/javascript|application\/javascript|application\/x-javascript)/i;

export function safeMimeType(mime) {
  const raw = String(mime || '').split(';')[0].trim();
  if (!raw || UNSAFE_MIME_RE.test(raw)) return 'application/octet-stream';
  return raw;
}

export function sanitizeDisplayFileName(name) {
  const cleaned = String(name || 'file')
    .replace(/[\x00-\x1f\x7f\u202a-\u202e\\/]+/g, '_')
    .trim()
    .slice(0, 180);
  return cleaned || 'file';
}

export function isAppFilePath(path) {
  return typeof path === 'string' && !path.includes('..') && APP_FILE_PATH.test(path);
}

export function safeFileName(name) {
  const raw = String(name || 'file').replace(/^.*[\\/]/, '');
  const dot = raw.lastIndexOf('.');
  const hasExt = dot > 0 && dot < raw.length - 1;
  const rawBase = hasExt ? raw.slice(0, dot) : raw;
  const rawExt = hasExt ? raw.slice(dot + 1) : '';
  const cleanBase =
    normalizeText(rawBase)
      .replace(/[^a-z0-9._-]+/gi, '-')
      .replace(/^-+|-+$/g, '')
      .slice(0, 60) || 'file';
  const cleanExt = normalizeText(rawExt)
    .replace(/[^a-z0-9]+/gi, '')
    .slice(0, 10);
  return cleanExt ? `${cleanBase}.${cleanExt}` : cleanBase;
}

export function formatBytes(bytes) {
  const n = Number(bytes) || 0;
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / (1024 * 1024)).toFixed(1)} MB`;
}

function fsBridge() {
  return typeof window !== 'undefined' && window.ladybirdApp?.fs ? window.ladybirdApp.fs : null;
}

export async function uploadAttachment(file) {
  const fs = fsBridge();
  if (!fs) throw new Error('Ladybird App Mode fs bridge is unavailable');
  if (!file) throw new Error('No file provided');
  if (file.size > MAX_FILE_BYTES) {
    throw new Error(`File exceeds maximum size of ${formatBytes(MAX_FILE_BYTES)}`);
  }
  await fs.mkdir(ATTACHMENT_DIR).catch(() => {});
  const relPath = `${ATTACHMENT_DIR}/${uid()}-${safeFileName(file.name)}`;
  if (!isAppFilePath(relPath)) {
    throw new Error('Generated attachment path failed confinement check');
  }
  const buf = await file.arrayBuffer();
  if (buf.byteLength > MAX_FILE_BYTES) {
    throw new Error(`File exceeds maximum size of ${formatBytes(MAX_FILE_BYTES)}`);
  }
  await fs.writeBytes(relPath, buf);
  return {
    name: sanitizeDisplayFileName(file.name),
    path: relPath,
    size: buf.byteLength,
    type: safeMimeType(file.type),
    uploadedAt: Date.now(),
  };
}

export async function readAttachmentBlob(path, mime = 'application/octet-stream') {
  const fs = fsBridge();
  if (!fs) throw new Error('Ladybird App Mode fs bridge is unavailable');
  if (!isAppFilePath(path)) throw new Error('Invalid attachment path');
  const buf = await fs.readBytes(path);
  return new Blob([buf], { type: safeMimeType(mime) });
}

export async function deleteStoredFile(path) {
  const fs = fsBridge();
  if (!fs || !isAppFilePath(path)) return false;
  try {
    await fs.deleteFile(path);
    return true;
  } catch {
    return false;
  }
}
