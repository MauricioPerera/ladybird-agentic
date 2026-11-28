/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

export const MAX_SHARD_BYTES = 3 * 1024 * 1024;
export const MAX_SINGLE_RECORD_BYTES = MAX_SHARD_BYTES - 64;
export const SINGLETON_KEYS = {
  settings: 'crm:settings:v1',
  templates: 'crm:templates:v1',
  attachments: 'crm:attachments:v1',
  trash: 'crm:trash:v1',
  notified: 'crm:notified:v1',
  backupMeta: 'crm:backup:meta:v1',
  webmcpAudit: 'crm:webmcp:audit:v1',
};

export const SHARDED_KINDS = ['companies', 'contacts', 'deals', 'activities', 'tasks', 'trash'];

const encoder = new TextEncoder();

function kvBridge() {
  return typeof window !== 'undefined' && window.ladybirdApp?.kv ? window.ladybirdApp.kv : null;
}

function byteLengthOf(obj) {
  return encoder.encode(JSON.stringify(obj)).byteLength;
}

export class ShardedKVStorage {
  constructor() {
    this.queues = new Map();
    this.memoryFallback = new Map();
  }

  enqueue(lockKey, fn) {
    const prev = this.queues.get(lockKey) || Promise.resolve();
    const next = prev.then(fn, fn);
    const tail = next.then(
      () => {
        if (this.queues.get(lockKey) === tail) this.queues.delete(lockKey);
      },
      () => {
        if (this.queues.get(lockKey) === tail) this.queues.delete(lockKey);
      }
    );
    this.queues.set(lockKey, tail);
    return next;
  }

  async rawGet(key) {
    const kv = kvBridge();
    if (!kv) return this.memoryFallback.get(key) ?? null;
    const res = await kv.get(key);
    if (!res || !res.found) return null;
    return res.value ?? null;
  }

  async rawSet(key, value) {
    const kv = kvBridge();
    if (!kv) {
      this.memoryFallback.set(key, structuredClone(value));
      return;
    }
    await kv.set(key, value);
  }

  async rawDelete(key) {
    const kv = kvBridge();
    if (!kv) {
      this.memoryFallback.delete(key);
      return;
    }
    await kv.delete(key);
  }

  async rawList(prefix) {
    const kv = kvBridge();
    const numSort = (a, b) => {
      const na = Number(a.key.slice(prefix.length));
      const nb = Number(b.key.slice(prefix.length));
      if (Number.isFinite(na) && Number.isFinite(nb)) return na - nb;
      return a.key.localeCompare(b.key);
    };
    if (!kv) {
      return [...this.memoryFallback.keys()].filter((k) => k.startsWith(prefix)).map((key) => ({ key })).sort(numSort);
    }
    const res = await kv.list(prefix);
    return (res?.keys || []).slice().sort(numSort);
  }

  async safeGet(key) {
    try {
      return await this.rawGet(key);
    } catch (err) {
      console.warn(`Failed to load KV key "${key}":`, err);
      return null;
    }
  }

  saveSingleton(name, value) {
    const key = SINGLETON_KEYS[name];
    if (!key) throw new Error(`Unknown singleton: ${name}`);
    const snapshot = structuredClone(value);
    return this.enqueue(key, () => this.rawSet(key, snapshot));
  }

  packShards(kind, items) {
    const shards = [];
    let current = [];
    let currentBytes = 32;

    for (const item of items || []) {
      const itemBytes = byteLengthOf(item) + 2;
      if (itemBytes > MAX_SINGLE_RECORD_BYTES) {
        throw new Error(`Record in ${kind} exceeds maximum shard size of ${MAX_SHARD_BYTES} bytes`);
      }
      if (current.length > 0 && currentBytes + itemBytes > MAX_SHARD_BYTES) {
        shards.push(current);
        current = [item];
        currentBytes = 32 + itemBytes;
      } else {
        current.push(item);
        currentBytes += itemBytes;
      }
    }
    shards.push(current);
    return shards;
  }

  saveCollection(kind, items) {
    const prefix = `crm:${kind}:s:`;
    const snapshot = structuredClone(items || []);
    const shards = this.packShards(kind, snapshot);
    return this.enqueue(prefix, async () => {
      const existing = await this.rawList(prefix);
      const writtenKeys = new Set();
      const writes = shards.map((bucket, idx) => {
        const key = `${prefix}${String(idx).padStart(4, '0')}`;
        writtenKeys.add(key);
        return this.rawSet(key, { shard: idx, items: bucket });
      });
      await Promise.all(writes);
      const stale = existing.filter((entry) => !writtenKeys.has(entry.key));
      await Promise.all(stale.map((e) => this.rawDelete(e.key).catch(() => {})));
    });
  }

  async loadCollection(kind) {
    const prefix = `crm:${kind}:s:`;
    const keys = await this.rawList(prefix);
    if (keys.length === 0) return [];
    const payloads = await Promise.all(keys.map((k) => this.safeGet(k.key)));
    const merged = [];
    for (const p of payloads) {
      if (p && Array.isArray(p.items)) merged.push(...p.items);
    }
    return merged;
  }

  async loadAll() {
    const [settings, templates, attachments, legacyTrash, notified, backupMeta, webmcpAudit, companies, contacts, deals, activities, tasks, shardedTrash] =
      await Promise.all([
        this.safeGet(SINGLETON_KEYS.settings),
        this.safeGet(SINGLETON_KEYS.templates),
        this.safeGet(SINGLETON_KEYS.attachments),
        this.safeGet(SINGLETON_KEYS.trash),
        this.safeGet(SINGLETON_KEYS.notified),
        this.safeGet(SINGLETON_KEYS.backupMeta),
        this.safeGet(SINGLETON_KEYS.webmcpAudit),
        ...SHARDED_KINDS.map((k) => this.loadCollection(k)),
      ]);

    const trash = shardedTrash.length > 0 ? shardedTrash : Array.isArray(legacyTrash) ? legacyTrash : [];

    return {
      settings: settings && typeof settings === 'object' ? settings : {},
      templates: Array.isArray(templates) ? templates : [],
      attachments: Array.isArray(attachments) ? attachments : [],
      trash,
      legacyTrashPresent: Array.isArray(legacyTrash) && legacyTrash.length > 0,
      notified: notified && typeof notified === 'object' ? notified : {},
      backupMeta: backupMeta && typeof backupMeta === 'object' ? backupMeta : null,
      webmcpAudit: Array.isArray(webmcpAudit) ? webmcpAudit : [],
      companies,
      contacts,
      deals,
      activities,
      tasks,
    };
  }

  async flush() {
    await Promise.all([...this.queues.values()]);
  }
}
