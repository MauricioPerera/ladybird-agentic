/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

export const BACKUP_DIR = 'backups';
export const BACKUP_KEEP = 8;
export const DEFAULT_BACKUP_DAYS = 7;
export const SAFETY_SNAPSHOT_PATH = 'backups/safety-snapshot.json';

const BACKUP_NAME_RE = /^crm-backup-(\d{4}-\d{2}-\d{2})T(\d{2})-(\d{2})-(\d{2})(?:-(\d{3}))?Z(?:-[a-z0-9]{4})?\.json$/;

function fsBridge() {
  return typeof window !== 'undefined' && window.ladybirdApp?.fs ? window.ladybirdApp.fs : null;
}

export function makeBackupName(d = new Date()) {
  const p = (n, w = 2) => String(n).padStart(w, '0');
  const suffix = Math.random().toString(36).slice(2, 6).padEnd(4, '0');
  return `crm-backup-${d.getUTCFullYear()}-${p(d.getUTCMonth() + 1)}-${p(d.getUTCDate())}T${p(d.getUTCHours())}-${p(d.getUTCMinutes())}-${p(d.getUTCSeconds())}-${p(d.getUTCMilliseconds(), 3)}Z-${suffix}.json`;
}

export function parseBackupTimestamp(name) {
  const m = String(name || '').match(BACKUP_NAME_RE);
  if (!m) return null;
  const iso = `${m[1]}T${m[2]}:${m[3]}:${m[4]}.${m[5] || '000'}Z`;
  const ms = Date.parse(iso);
  return Number.isFinite(ms) ? ms : null;
}

export async function listBackups() {
  const fs = fsBridge();
  if (!fs) return [];
  await fs.mkdir(BACKUP_DIR).catch(() => {});
  const res = await fs.listDir(BACKUP_DIR).catch(() => ({ entries: [] }));
  const items = [];
  for (const entry of res.entries || []) {
    if (entry.isDirectory) continue;
    const ts = parseBackupTimestamp(entry.name);
    if (!ts) continue;
    items.push({
      name: entry.name,
      path: `${BACKUP_DIR}/${entry.name}`,
      at: ts,
      size: entry.size || 0,
    });
  }
  items.sort((a, b) => b.at - a.at);
  return items;
}

export async function writeBackup(exportPayload, now = new Date()) {
  const fs = fsBridge();
  if (!fs) throw new Error('Ladybird App Mode fs bridge is unavailable');
  await fs.mkdir(BACKUP_DIR).catch(() => {});
  const name = makeBackupName(now);
  const relPath = `${BACKUP_DIR}/${name}`;
  const bytes = new TextEncoder().encode(JSON.stringify(exportPayload));
  await fs.writeBytes(relPath, bytes);

  const all = await listBackups();
  for (const stale of all.slice(BACKUP_KEEP)) {
    await fs.deleteFile(stale.path).catch(() => {});
  }
  return { name, path: relPath, at: now.getTime(), bytesWritten: bytes.byteLength };
}

export async function readBackup(relPath) {
  const fs = fsBridge();
  if (!fs) throw new Error('Ladybird App Mode fs bridge is unavailable');
  const prefix = `${BACKUP_DIR}/`;
  if (typeof relPath !== 'string' || !relPath.startsWith(prefix)) {
    throw new Error(`Invalid backup path: ${relPath}`);
  }
  const base = relPath.slice(prefix.length);
  if (!base || base.includes('/') || base.includes('\\') || base.includes('..') || !BACKUP_NAME_RE.test(base)) {
    throw new Error(`Invalid backup path: ${relPath}`);
  }
  const buf = await fs.readBytes(relPath);
  return JSON.parse(new TextDecoder().decode(buf));
}

export async function writeSafetySnapshot(snapshotObj) {
  const fs = fsBridge();
  if (!fs) return null;
  await fs.mkdir(BACKUP_DIR).catch(() => {});
  const bytes = new TextEncoder().encode(JSON.stringify(snapshotObj));
  await fs.writeBytes(SAFETY_SNAPSHOT_PATH, bytes);
  return { path: SAFETY_SNAPSHOT_PATH, at: snapshotObj.at, reason: snapshotObj.reason, bytesWritten: bytes.byteLength };
}

export async function readSafetySnapshot() {
  const fs = fsBridge();
  if (!fs) return null;
  const st = await fs.stat(SAFETY_SNAPSHOT_PATH).catch(() => null);
  if (!st?.exists) return null;
  try {
    const buf = await fs.readBytes(SAFETY_SNAPSHOT_PATH);
    return JSON.parse(new TextDecoder().decode(buf));
  } catch (err) {
    console.warn('Corrupt safety snapshot ignored:', err);
    return null;
  }
}

export async function runBackupIfDue(store, now = new Date()) {
  const days = Number(store?.settings?.autoBackupDays ?? DEFAULT_BACKUP_DAYS);
  if (!days || days <= 0 || !store.hasData()) return null;
  const existing = await listBackups();
  const newest = existing[0];
  if (newest && now.getTime() - newest.at < days * 86400000) {
    return null;
  }
  return writeBackup(store.exportAll(), now);
}
