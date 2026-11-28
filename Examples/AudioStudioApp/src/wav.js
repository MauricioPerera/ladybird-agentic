/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

function toUint8Array(input) {
  if (input instanceof Uint8Array) return input;
  if (input instanceof ArrayBuffer) return new Uint8Array(input);
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  throw new TypeError('Expected Uint8Array or ArrayBuffer');
}

function readAscii(view, offset, length) {
  let out = '';
  for (let i = 0; i < length; i++) {
    out += String.fromCharCode(view.getUint8(offset + i));
  }
  return out;
}

function writeAscii(view, offset, text) {
  for (let i = 0; i < text.length; i++) {
    view.setUint8(offset + i, text.charCodeAt(i) & 0xff);
  }
}

export function parseWavHeader(input, fileSize = null) {
  const bytes = toUint8Array(input);
  if (bytes.byteLength < 12) {
    return { valid: false, error: 'Buffer too small for RIFF/WAVE header' };
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (readAscii(view, 0, 4) !== 'RIFF' || readAscii(view, 8, 4) !== 'WAVE') {
    return { valid: false, error: 'Missing RIFF/WAVE signature' };
  }

  const riffSize = view.getUint32(4, true);
  let audioFormat = 0;
  let channels = 0;
  let sampleRate = 0;
  let byteRate = 0;
  let blockAlign = 0;
  let bitDepth = 0;
  let dataOffset = 0;
  let dataSize = 0;
  let foundFmt = false;
  let foundData = false;

  let offset = 12;
  while (offset + 8 <= bytes.byteLength) {
    const chunkId = readAscii(view, offset, 4);
    const chunkSize = view.getUint32(offset + 4, true);
    const payloadOffset = offset + 8;

    if (chunkId === 'fmt ' && payloadOffset + 16 <= bytes.byteLength) {
      foundFmt = true;
      audioFormat = view.getUint16(payloadOffset, true);
      channels = view.getUint16(payloadOffset + 2, true);
      sampleRate = view.getUint32(payloadOffset + 4, true);
      byteRate = view.getUint32(payloadOffset + 8, true);
      blockAlign = view.getUint16(payloadOffset + 12, true);
      bitDepth = view.getUint16(payloadOffset + 14, true);
      if (audioFormat === 0xfffe && chunkSize >= 40 && payloadOffset + 26 <= bytes.byteLength) {
        audioFormat = view.getUint16(payloadOffset + 24, true);
      }
    } else if (chunkId === 'data') {
      foundData = true;
      dataOffset = payloadOffset;
      dataSize = chunkSize;
      break;
    }

    const paddedSize = chunkSize + (chunkSize & 1);
    if (paddedSize <= 0 && chunkSize !== 0) break;
    offset = payloadOffset + paddedSize;
  }

  if (!foundFmt || channels <= 0 || sampleRate <= 0 || bitDepth <= 0) {
    return { valid: false, error: 'Missing or invalid WAV fmt chunk' };
  }

  const effectiveBlockAlign = blockAlign || Math.floor((channels * bitDepth) / 8);
  const effectiveDataSize =
    foundData && dataSize > 0
      ? dataSize
      : fileSize && dataOffset > 0 && fileSize > dataOffset
        ? fileSize - dataOffset
        : 0;
  const frameCount = effectiveBlockAlign > 0 ? Math.floor(effectiveDataSize / effectiveBlockAlign) : 0;
  const durationSec = sampleRate > 0 ? Number((frameCount / sampleRate).toFixed(6)) : 0;

  return {
    valid: true,
    riffSize,
    audioFormat,
    formatName: audioFormat === 3 ? 'ieee-float' : audioFormat === 1 ? 'pcm' : `format-${audioFormat}`,
    channels,
    sampleRate,
    bitDepth,
    byteRate,
    blockAlign: effectiveBlockAlign,
    dataOffset,
    dataSize: effectiveDataSize,
    frameCount,
    durationSec,
    fileSize: fileSize ?? riffSize + 8,
  };
}

export function decodeWavBytes(input) {
  const bytes = toUint8Array(input);
  const hdr = parseWavHeader(bytes, bytes.byteLength);
  if (!hdr.valid) {
    throw new Error(hdr.error || 'Invalid WAV file');
  }
  if (!hdr.dataOffset || hdr.dataOffset > bytes.byteLength) {
    throw new Error('WAV data chunk is missing or truncated');
  }

  const supportedPcm = hdr.audioFormat === 1 && (hdr.bitDepth === 8 || hdr.bitDepth === 16 || hdr.bitDepth === 24 || hdr.bitDepth === 32);
  const supportedFloat = hdr.audioFormat === 3 && hdr.bitDepth === 32;
  if (!supportedPcm && !supportedFloat) {
    throw new Error(`Unsupported WAV format (${hdr.formatName}, ${hdr.bitDepth}-bit)`);
  }

  const availableBytes = Math.min(hdr.dataSize, bytes.byteLength - hdr.dataOffset);
  const frameCount = hdr.blockAlign > 0 ? Math.floor(availableBytes / hdr.blockAlign) : 0;
  const bytesPerSample = Math.floor(hdr.bitDepth / 8);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

  const channels = [];
  for (let ch = 0; ch < hdr.channels; ch++) {
    channels.push(new Float32Array(frameCount));
  }

  for (let i = 0; i < frameCount; i++) {
    const frameBase = hdr.dataOffset + i * hdr.blockAlign;
    for (let ch = 0; ch < hdr.channels; ch++) {
      const samplePos = frameBase + ch * bytesPerSample;
      let sample = 0;
      if (hdr.audioFormat === 3 && hdr.bitDepth === 32) {
        sample = view.getFloat32(samplePos, true);
      } else if (hdr.bitDepth === 8) {
        sample = (view.getUint8(samplePos) - 128) / 128;
      } else if (hdr.bitDepth === 16) {
        sample = view.getInt16(samplePos, true) / 32768;
      } else if (hdr.bitDepth === 24) {
        const b0 = bytes[samplePos];
        const b1 = bytes[samplePos + 1];
        const b2 = bytes[samplePos + 2];
        let val = b0 | (b1 << 8) | (b2 << 16);
        if (val & 0x800000) val |= ~0xffffff;
        sample = val / 8388608;
      } else if (hdr.bitDepth === 32) {
        sample = view.getInt32(samplePos, true) / 2147483648;
      }
      channels[ch][i] = Number.isFinite(sample) ? sample : 0;
    }
  }

  return {
    sampleRate: hdr.sampleRate,
    channels,
    bitDepth: hdr.bitDepth,
    length: frameCount,
  };
}

export function encodeWav(channels, sampleRate, bitDepth = 16) {
  if (!Array.isArray(channels) || channels.length === 0) {
    throw new Error('At least one audio channel is required');
  }
  const numChannels = channels.length;
  const frameCount = channels[0].length;
  const targetDepth = bitDepth === 32 ? 32 : 16;
  const audioFormat = targetDepth === 32 ? 3 : 1;
  const bytesPerSample = targetDepth / 8;
  const blockAlign = numChannels * bytesPerSample;
  const byteRate = sampleRate * blockAlign;
  const dataSize = frameCount * blockAlign;

  const buffer = new ArrayBuffer(44 + dataSize);
  const view = new DataView(buffer);

  writeAscii(view, 0, 'RIFF');
  view.setUint32(4, 36 + dataSize, true);
  writeAscii(view, 8, 'WAVE');
  writeAscii(view, 12, 'fmt ');
  view.setUint32(16, 16, true);
  view.setUint16(20, audioFormat, true);
  view.setUint16(22, numChannels, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, byteRate, true);
  view.setUint16(32, blockAlign, true);
  view.setUint16(34, targetDepth, true);
  writeAscii(view, 36, 'data');
  view.setUint32(40, dataSize, true);

  for (let i = 0; i < frameCount; i++) {
    const frameBase = 44 + i * blockAlign;
    for (let ch = 0; ch < numChannels; ch++) {
      const pos = frameBase + ch * bytesPerSample;
      let s = channels[ch][i] || 0;
      if (!Number.isFinite(s)) s = 0;
      if (targetDepth === 32) {
        view.setFloat32(pos, s, true);
      } else {
        if (s > 1) s = 1;
        if (s < -1) s = -1;
        view.setInt16(pos, Math.round(s * 32767), true);
      }
    }
  }

  return new Uint8Array(buffer);
}
