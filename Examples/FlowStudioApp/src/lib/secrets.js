/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

export const SECRETS_KV_KEY = 'flowstudio:secrets:v1';
export const SECRET_KEY_KV_KEY = 'flowstudio:secrets_k:v1';

export function isValidSecretName(name) {
  return typeof name === 'string' && /^[A-Z][A-Z0-9_]{1,47}$/.test(name);
}

function bytesToB64(u8) {
  if (typeof Buffer === 'function') return Buffer.from(u8).toString('base64');
  let bin = '';
  for (let i = 0; i < u8.length; i++) bin += String.fromCharCode(u8[i]);
  return btoa(bin);
}

function b64ToBytes(b64) {
  if (typeof Buffer === 'function') return new Uint8Array(Buffer.from(b64, 'base64'));
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

async function getOrCreateAesKey(kv) {
  const subtle = globalThis.crypto?.subtle;
  if (!subtle) return null;
  let rawB64 = null;
  try {
    const existing = await kv.get(SECRET_KEY_KV_KEY);
    if (existing?.found && typeof existing.value === 'string') rawB64 = existing.value;
  } catch {}
  if (!rawB64) {
    const raw = new Uint8Array(32);
    globalThis.crypto.getRandomValues(raw);
    rawB64 = bytesToB64(raw);
    await kv.set(SECRET_KEY_KV_KEY, rawB64);
  }
  const keyBytes = b64ToBytes(rawB64);
  return await subtle.importKey('raw', keyBytes, { name: 'AES-GCM' }, false, ['encrypt', 'decrypt']);
}

async function encryptSecret(plaintext, kv) {
  const key = await getOrCreateAesKey(kv);
  const bytes = new TextEncoder().encode(String(plaintext));
  if (!key) {
    return { scheme: 'v1-b64', ct: bytesToB64(bytes) };
  }
  const iv = new Uint8Array(12);
  globalThis.crypto.getRandomValues(iv);
  const buf = await globalThis.crypto.subtle.encrypt({ name: 'AES-GCM', iv }, key, bytes);
  return {
    scheme: 'v1-aesgcm',
    iv: bytesToB64(iv),
    ct: bytesToB64(new Uint8Array(buf)),
  };
}

async function decryptSecret(entry, kv) {
  if (!entry || typeof entry !== 'object') return '';
  if (entry.scheme === 'v1-b64') {
    return new TextDecoder().decode(b64ToBytes(entry.ct || ''));
  }
  if (entry.scheme === 'v1-aesgcm') {
    const key = await getOrCreateAesKey(kv);
    if (!key) throw new Error('WebCrypto unavailable to decrypt secret');
    const plain = await globalThis.crypto.subtle.decrypt(
      { name: 'AES-GCM', iv: b64ToBytes(entry.iv) },
      key,
      b64ToBytes(entry.ct)
    );
    return new TextDecoder().decode(plain);
  }
  return '';
}

export function createSecretStore(bridge) {
  const readAllRaw = async () => {
    if (!bridge?.kv) return {};
    try {
      const res = await bridge.kv.get(SECRETS_KV_KEY);
      return res?.found && res.value && typeof res.value === 'object' ? { ...res.value } : {};
    } catch {
      return {};
    }
  };

  const writeAllRaw = async (map) => {
    if (!bridge?.kv) throw new Error('KV storage unavailable');
    await bridge.kv.set(SECRETS_KV_KEY, map);
  };

  return {
    async list() {
      const raw = await readAllRaw();
      return Object.entries(raw)
        .map(([name, rec]) => ({
          name,
          updatedAt: rec?.updatedAt || null,
          scheme: rec?.scheme || 'v1-aesgcm',
          hasValue: true,
        }))
        .sort((a, b) => a.name.localeCompare(b.name));
    },

    async set(name, plaintext) {
      const cleanName = String(name || '').trim().toUpperCase();
      if (!isValidSecretName(cleanName)) {
        throw new Error('Secret name must match [A-Z][A-Z0-9_]{1,47} (e.g. STRIPE_KEY)');
      }
      if (plaintext === undefined || plaintext === null || String(plaintext) === '') {
        throw new Error('Secret value cannot be empty');
      }
      const enc = await encryptSecret(String(plaintext), bridge.kv);
      const raw = await readAllRaw();
      raw[cleanName] = { ...enc, updatedAt: new Date().toISOString() };
      await writeAllRaw(raw);
      return { name: cleanName, updatedAt: raw[cleanName].updatedAt, hasValue: true };
    },

    async remove(name) {
      const raw = await readAllRaw();
      if (name in raw) {
        delete raw[name];
        await writeAllRaw(raw);
      }
      return true;
    },

    async resolveMap() {
      const raw = await readAllRaw();
      const out = {};
      for (const [name, entry] of Object.entries(raw)) {
        try {
          out[name] = await decryptSecret(entry, bridge.kv);
        } catch {}
      }
      return out;
    },
  };
}

export function redactSecrets(value, secretMap) {
  const secrets = Object.values(secretMap || {}).filter((v) => typeof v === 'string' && v.length >= 4);
  if (!secrets.length) return value;

  const maskStr = (str) => {
    let out = str;
    for (const sec of secrets) {
      if (out.includes(sec)) out = out.split(sec).join('••••••••');
    }
    return out;
  };

  const walk = (node) => {
    if (typeof node === 'string') return maskStr(node);
    if (Array.isArray(node)) return node.map(walk);
    if (node && typeof node === 'object') {
      const copy = {};
      for (const [k, v] of Object.entries(node)) copy[k] = walk(v);
      return copy;
    }
    return node;
  };

  return walk(value);
}

export function stripSecretsFromExport(obj, secretMap) {
  const cloned = obj ? JSON.parse(JSON.stringify(obj)) : obj;
  return redactSecrets(cloned, secretMap);
}
