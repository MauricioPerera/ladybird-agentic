/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

const MIN_DB = -120;

export function dbToLinear(db) {
  if (!Number.isFinite(db)) return 1;
  if (db <= MIN_DB) return 0;
  return Math.pow(10, db / 20);
}

export function linearToDb(linear) {
  if (!Number.isFinite(linear) || linear <= 0) return MIN_DB;
  const db = 20 * Math.log10(linear);
  return Math.max(MIN_DB, Number(db.toFixed(3)));
}

export function cloneChannels(channels) {
  return channels.map((ch) => new Float32Array(ch));
}

export function resolveRange(length, sampleRate, range = null) {
  if (!range || (range.startSec == null && range.endSec == null)) {
    return { startSample: 0, endSample: length };
  }
  const startSec = Number.isFinite(range.startSec) ? Math.max(0, range.startSec) : 0;
  const endSec = Number.isFinite(range.endSec) ? Math.max(startSec, range.endSec) : length / sampleRate;
  const startSample = Math.min(length, Math.max(0, Math.round(startSec * sampleRate)));
  const endSample = Math.min(length, Math.max(startSample, Math.round(endSec * sampleRate)));
  return { startSample, endSample };
}

export function generateTone({
  waveform = 'sine',
  freq = 440,
  durationSec = 2.0,
  sampleRate = 44100,
  gainDb = -6,
  channels = 1,
} = {}) {
  const sr = Math.max(8000, Math.min(192000, Math.round(Number(sampleRate) || 44100)));
  const dur = Math.max(0.02, Math.min(120, Number(durationSec) || 1.0));
  const f = Math.max(20, Math.min(sr * 0.45, Number(freq) || 440));
  const chCount = Math.max(1, Math.min(2, Math.round(Number(channels) || 1)));
  const frameCount = Math.max(1, Math.round(sr * dur));
  const amp = dbToLinear(Number.isFinite(Number(gainDb)) ? Number(gainDb) : -6);
  const kind = String(waveform || 'sine').toLowerCase();

  const mono = new Float32Array(frameCount);
  let seed = 0x12345678;

  if (kind === 'chord') {
    let peak = 0;
    for (let i = 0; i < frameCount; i++) {
      const t = i / sr;
      const v =
        Math.sin(2 * Math.PI * f * t) +
        0.85 * Math.sin(2 * Math.PI * (f * 1.25) * t) +
        0.75 * Math.sin(2 * Math.PI * (f * 1.5) * t);
      mono[i] = v;
      const av = Math.abs(v);
      if (av > peak) peak = av;
    }
    const scale = peak > 0 ? amp / peak : amp;
    for (let i = 0; i < frameCount; i++) mono[i] *= scale;
  } else {
    for (let i = 0; i < frameCount; i++) {
      const phase = ((i * f) / sr) % 1;
      let v = 0;
      if (kind === 'square') {
        v = phase < 0.5 ? 1 : -1;
      } else if (kind === 'sawtooth' || kind === 'saw') {
        v = 2 * phase - 1;
      } else if (kind === 'triangle' || kind === 'tri') {
        v = 1 - 4 * Math.abs(Math.round(phase - 0.25) - (phase - 0.25));
      } else if (kind === 'noise') {
        seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
        v = (seed / 0xffffffff) * 2 - 1;
      } else {
        v = Math.sin(2 * Math.PI * phase);
      }
      mono[i] = v * amp;
    }
  }

  const out = [];
  for (let ch = 0; ch < chCount; ch++) {
    out.push(ch === 0 ? mono : new Float32Array(mono));
  }
  return { sampleRate: sr, channels: out, length: frameCount };
}

export function applyGain(channels, gainDb, range = null, sampleRate = 44100) {
  const factor = dbToLinear(gainDb);
  const length = channels[0]?.length || 0;
  const { startSample, endSample } = resolveRange(length, sampleRate, range);
  return channels.map((ch) => {
    const next = new Float32Array(ch);
    for (let i = startSample; i < endSample; i++) {
      next[i] *= factor;
    }
    return next;
  });
}

export function normalizePeak(channels, targetDb = -1.0, range = null, sampleRate = 44100) {
  const length = channels[0]?.length || 0;
  const { startSample, endSample } = resolveRange(length, sampleRate, range);
  let peak = 0;
  for (const ch of channels) {
    for (let i = startSample; i < endSample; i++) {
      const a = Math.abs(ch[i]);
      if (a > peak) peak = a;
    }
  }
  if (peak <= 1e-12) return cloneChannels(channels);
  const factor = dbToLinear(targetDb) / peak;
  return channels.map((ch) => {
    const next = new Float32Array(ch);
    for (let i = startSample; i < endSample; i++) {
      next[i] *= factor;
    }
    return next;
  });
}

export function applyFade(channels, { direction = 'in', durationSec = 0.25, curve = 'linear' } = {}, sampleRate = 44100, range = null) {
  const length = channels[0]?.length || 0;
  const { startSample, endSample } = resolveRange(length, sampleRate, range);
  const span = Math.max(1, endSample - startSample);
  const requestedFrames = Math.max(1, Math.round((Number(durationSec) || 0.25) * sampleRate));
  const fadeFrames = range ? span : Math.min(span, requestedFrames);
  const fadeStart = direction === 'out' ? endSample - fadeFrames : startSample;
  const isOut = direction === 'out';
  const useSCurve = curve === 's-curve' || curve === 'cosine';

  return channels.map((ch) => {
    const next = new Float32Array(ch);
    for (let idx = 0; idx < fadeFrames; idx++) {
      const sampleIndex = fadeStart + idx;
      if (sampleIndex < startSample || sampleIndex >= endSample) continue;
      let t = fadeFrames <= 1 ? 1 : idx / (fadeFrames - 1);
      if (isOut) t = 1 - t;
      const gain = useSCurve ? 0.5 * (1 - Math.cos(Math.PI * t)) : t;
      next[sampleIndex] *= gain;
    }
    return next;
  });
}

export function reverseAudio(channels, range = null, sampleRate = 44100) {
  const length = channels[0]?.length || 0;
  const { startSample, endSample } = resolveRange(length, sampleRate, range);
  return channels.map((ch) => {
    const next = new Float32Array(ch);
    let left = startSample;
    let right = endSample - 1;
    while (left < right) {
      const tmp = next[left];
      next[left] = next[right];
      next[right] = tmp;
      left++;
      right--;
    }
    return next;
  });
}

export function invertPhase(channels, range = null, sampleRate = 44100) {
  const length = channels[0]?.length || 0;
  const { startSample, endSample } = resolveRange(length, sampleRate, range);
  return channels.map((ch) => {
    const next = new Float32Array(ch);
    for (let i = startSample; i < endSample; i++) {
      next[i] = -next[i];
    }
    return next;
  });
}

export function biquadFilter(channels, sampleRate, { type = 'lowpass', freq = 1000, q = 0.707 } = {}, range = null) {
  const sr = Math.max(1000, sampleRate);
  const f0 = Math.max(10, Math.min(sr * 0.49, Number(freq) || 1000));
  const qVal = Math.max(0.1, Math.min(20, Number(q) || 0.707));
  const w0 = (2 * Math.PI * f0) / sr;
  const cosW0 = Math.cos(w0);
  const sinW0 = Math.sin(w0);
  const alpha = sinW0 / (2 * qVal);

  let b0 = 1;
  let b1 = 0;
  let b2 = 0;
  const a0 = 1 + alpha;
  const a1 = -2 * cosW0;
  const a2 = 1 - alpha;

  const filterType = String(type || 'lowpass').toLowerCase();
  if (filterType === 'highpass') {
    b0 = (1 + cosW0) / 2;
    b1 = -(1 + cosW0);
    b2 = (1 + cosW0) / 2;
  } else if (filterType === 'bandpass') {
    b0 = alpha;
    b1 = 0;
    b2 = -alpha;
  } else if (filterType === 'notch') {
    b0 = 1;
    b1 = -2 * cosW0;
    b2 = 1;
  } else {
    b0 = (1 - cosW0) / 2;
    b1 = 1 - cosW0;
    b2 = (1 - cosW0) / 2;
  }

  const nb0 = b0 / a0;
  const nb1 = b1 / a0;
  const nb2 = b2 / a0;
  const na1 = a1 / a0;
  const na2 = a2 / a0;

  const length = channels[0]?.length || 0;
  const { startSample, endSample } = resolveRange(length, sr, range);

  return channels.map((ch) => {
    const next = new Float32Array(ch);
    let z1 = 0;
    let z2 = 0;
    for (let i = startSample; i < endSample; i++) {
      const x = next[i];
      const y = nb0 * x + z1;
      z1 = nb1 * x - na1 * y + z2;
      z2 = nb2 * x - na2 * y;
      next[i] = y;
    }
    return next;
  });
}

export function trimRange(channels, sampleRate, startSec, endSec) {
  const length = channels[0]?.length || 0;
  const { startSample, endSample } = resolveRange(length, sampleRate, { startSec, endSec });
  if (endSample <= startSample) {
    throw new Error('Selection range must contain at least one sample');
  }
  return channels.map((ch) => ch.slice(startSample, endSample));
}

export function cutRange(channels, sampleRate, startSec, endSec, mode = 'delete') {
  const length = channels[0]?.length || 0;
  const { startSample, endSample } = resolveRange(length, sampleRate, { startSec, endSec });
  if (endSample <= startSample) return cloneChannels(channels);

  if (mode === 'silence') {
    return channels.map((ch) => {
      const next = new Float32Array(ch);
      next.fill(0, startSample, endSample);
      return next;
    });
  }

  const cutLen = endSample - startSample;
  const nextLen = Math.max(1, length - cutLen);
  return channels.map((ch) => {
    if (length - cutLen <= 0) {
      return new Float32Array(1);
    }
    const next = new Float32Array(nextLen);
    next.set(ch.subarray(0, startSample), 0);
    next.set(ch.subarray(endSample), startSample);
    return next;
  });
}

export function trimSilence(channels, sampleRate, { thresholdDb = -45, padSec = 0.05 } = {}) {
  const length = channels[0]?.length || 0;
  if (length === 0) return cloneChannels(channels);
  const threshold = dbToLinear(thresholdDb);
  const padSamples = Math.max(0, Math.round((Number(padSec) || 0) * sampleRate));

  let firstActive = length;
  let lastActive = -1;
  for (let i = 0; i < length; i++) {
    let absMax = 0;
    for (let c = 0; c < channels.length; c++) {
      const a = Math.abs(channels[c][i]);
      if (a > absMax) absMax = a;
    }
    if (absMax >= threshold) {
      if (firstActive === length) firstActive = i;
      lastActive = i;
    }
  }

  if (lastActive < firstActive) {
    return channels.map(() => new Float32Array(1));
  }
  const start = Math.max(0, firstActive - padSamples);
  const end = Math.min(length, lastActive + 1 + padSamples);
  return channels.map((ch) => ch.slice(start, end));
}

function fftRadix2(real, imag) {
  const n = real.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    while (j & bit) {
      j ^= bit;
      bit >>= 1;
    }
    j ^= bit;
    if (i < j) {
      const tr = real[i];
      real[i] = real[j];
      real[j] = tr;
      const ti = imag[i];
      imag[i] = imag[j];
      imag[j] = ti;
    }
  }

  for (let len = 2; len <= n; len <<= 1) {
    const ang = (-2 * Math.PI) / len;
    const wlenRe = Math.cos(ang);
    const wlenIm = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let wRe = 1;
      let wIm = 0;
      const half = len >> 1;
      for (let j = 0; j < half; j++) {
        const uRe = real[i + j];
        const uIm = imag[i + j];
        const vRe = real[i + j + half] * wRe - imag[i + j + half] * wIm;
        const vIm = real[i + j + half] * wIm + imag[i + j + half] * wRe;
        real[i + j] = uRe + vRe;
        imag[i + j] = uIm + vIm;
        real[i + j + half] = uRe - vRe;
        imag[i + j + half] = uIm - vIm;
        const nextWRe = wRe * wlenRe - wIm * wlenIm;
        wIm = wRe * wlenIm + wIm * wlenRe;
        wRe = nextWRe;
      }
    }
  }
}

export function computeSpectrum(channels, sampleRate, fftSize = 1024, atSec = null) {
  const length = channels[0]?.length || 0;
  const size = 1 << Math.max(6, Math.min(13, Math.round(Math.log2(fftSize || 1024))));
  const real = new Float32Array(size);
  const imag = new Float32Array(size);

  if (length === 0) {
    return {
      fftSize: size,
      magnitudesDb: new Float32Array(size / 2).fill(MIN_DB),
      dominantFreqHz: 0,
      spectralCentroidHz: 0,
      bands: { sub: 0, bass: 0, lowMid: 0, mid: 0, highMid: 0, presence: 0, air: 0 },
    };
  }

  const centerSample =
    atSec == null ? Math.floor(length / 2) : Math.max(0, Math.min(length - 1, Math.round(atSec * sampleRate)));
  const start = Math.max(0, Math.min(Math.max(0, length - size), centerSample - Math.floor(size / 2)));

  for (let i = 0; i < size; i++) {
    const idx = start + i;
    let mono = 0;
    if (idx < length) {
      for (let c = 0; c < channels.length; c++) mono += channels[c][idx];
      mono /= channels.length;
    }
    const hann = 0.5 * (1 - Math.cos((2 * Math.PI * i) / (size - 1)));
    real[i] = mono * hann;
  }

  fftRadix2(real, imag);

  const bins = size >> 1;
  const magnitudesDb = new Float32Array(bins);
  const binHz = sampleRate / size;

  let maxMag = 0;
  let maxBin = 0;
  let weightedFreq = 0;
  let totalMag = 0;

  const bandAccum = { sub: 0, bass: 0, lowMid: 0, mid: 0, highMid: 0, presence: 0, air: 0 };

  for (let k = 0; k < bins; k++) {
    const mag = Math.hypot(real[k], imag[k]) / (size * 0.25);
    magnitudesDb[k] = linearToDb(mag);
    const hz = k * binHz;
    if (k > 0) {
      if (mag > maxMag) {
        maxMag = mag;
        maxBin = k;
      }
      weightedFreq += hz * mag;
      totalMag += mag;
    }

    if (hz < 60) bandAccum.sub += mag;
    else if (hz < 250) bandAccum.bass += mag;
    else if (hz < 500) bandAccum.lowMid += mag;
    else if (hz < 2000) bandAccum.mid += mag;
    else if (hz < 4000) bandAccum.highMid += mag;
    else if (hz < 8000) bandAccum.presence += mag;
    else bandAccum.air += mag;
  }

  const bands = {};
  const denom = totalMag > 1e-12 ? totalMag : 1;
  for (const [k, v] of Object.entries(bandAccum)) {
    bands[k] = Number((v / denom).toFixed(4));
  }

  return {
    fftSize: size,
    magnitudesDb,
    dominantFreqHz: Number((maxBin * binHz).toFixed(2)),
    spectralCentroidHz: totalMag > 1e-12 ? Number((weightedFreq / totalMag).toFixed(2)) : 0,
    bands,
  };
}

export function analyzeAudio(channels, sampleRate, range = null) {
  const length = channels[0]?.length || 0;
  const { startSample, endSample } = resolveRange(length, sampleRate, range);
  const span = Math.max(0, endSample - startSample);

  let peakLinear = 0;
  let sumSq = 0;
  let sum = 0;
  let clippingSamples = 0;
  let zeroCrossings = 0;
  let count = 0;

  for (let c = 0; c < channels.length; c++) {
    const ch = channels[c];
    let prev = startSample < endSample ? ch[startSample] : 0;
    for (let i = startSample; i < endSample; i++) {
      const s = ch[i];
      const a = Math.abs(s);
      if (a > peakLinear) peakLinear = a;
      if (a >= 0.9999) clippingSamples++;
      sumSq += s * s;
      sum += s;
      if (c === 0 && i > startSample && ((s >= 0 && prev < 0) || (s < 0 && prev >= 0))) {
        zeroCrossings++;
      }
      prev = s;
      count++;
    }
  }

  const rmsLinear = count > 0 ? Math.sqrt(sumSq / count) : 0;
  const dcOffset = count > 0 ? sum / count : 0;
  const peakDb = linearToDb(peakLinear);
  const rmsDb = linearToDb(rmsLinear);
  const crestFactorDb = peakLinear > 0 ? Number((peakDb - rmsDb).toFixed(3)) : 0;
  const durationSec = sampleRate > 0 ? Number((span / sampleRate).toFixed(4)) : 0;
  const zeroCrossingRateHz = durationSec > 0 ? Number((zeroCrossings / durationSec / 2).toFixed(2)) : 0;

  const midSec = ((startSample + endSample) * 0.5) / sampleRate;
  const spec = computeSpectrum(channels, sampleRate, 2048, midSec);

  const envBins = 64;
  const envelope64 = [];
  if (span > 0) {
    const step = span / envBins;
    for (let b = 0; b < envBins; b++) {
      const bStart = startSample + Math.floor(b * step);
      const bEnd = Math.min(endSample, Math.max(bStart + 1, startSample + Math.floor((b + 1) * step)));
      let binPeak = 0;
      for (let i = bStart; i < bEnd; i++) {
        for (let c = 0; c < channels.length; c++) {
          const a = Math.abs(channels[c][i]);
          if (a > binPeak) binPeak = a;
        }
      }
      envelope64.push(Number(binPeak.toFixed(4)));
    }
  }

  return {
    sampleRate,
    channelCount: channels.length,
    frames: span,
    durationSec,
    peakLinear: Number(peakLinear.toFixed(6)),
    peakDb,
    rmsLinear: Number(rmsLinear.toFixed(6)),
    rmsDb,
    crestFactorDb,
    dcOffset: Number(dcOffset.toFixed(6)),
    clippingSamples,
    zeroCrossingRateHz,
    dominantFreqHz: spec.dominantFreqHz,
    spectralCentroidHz: spec.spectralCentroidHz,
    bands: spec.bands,
    envelope64,
  };
}

export function detectSilences(channels, sampleRate, { thresholdDb = -40, minDurationSec = 0.25 } = {}) {
  const length = channels[0]?.length || 0;
  if (length === 0 || sampleRate <= 0) return [];
  const threshold = dbToLinear(thresholdDb);
  const minDur = Math.max(0.02, Number(minDurationSec) || 0.25);
  const winSize = Math.max(1, Math.round(sampleRate * 0.01));

  const regions = [];
  let inSilence = false;
  let silStart = 0;

  for (let pos = 0; pos < length; pos += winSize) {
    const end = Math.min(length, pos + winSize);
    let winPeak = 0;
    for (let i = pos; i < end; i++) {
      for (let c = 0; c < channels.length; c++) {
        const a = Math.abs(channels[c][i]);
        if (a > winPeak) winPeak = a;
      }
    }
    const silent = winPeak < threshold;
    if (silent && !inSilence) {
      inSilence = true;
      silStart = pos;
    } else if (!silent && inSilence) {
      inSilence = false;
      const startSec = silStart / sampleRate;
      const endSec = pos / sampleRate;
      if (endSec - startSec >= minDur) {
        regions.push({
          startSec: Number(startSec.toFixed(4)),
          endSec: Number(endSec.toFixed(4)),
          durationSec: Number((endSec - startSec).toFixed(4)),
        });
      }
    }
  }

  if (inSilence) {
    const startSec = silStart / sampleRate;
    const endSec = length / sampleRate;
    if (endSec - startSec >= minDur) {
      regions.push({
        startSec: Number(startSec.toFixed(4)),
        endSec: Number(endSec.toFixed(4)),
        durationSec: Number((endSec - startSec).toFixed(4)),
      });
    }
  }

  return regions;
}

export function computePeaks(channels, buckets, startSample = 0, endSample = null) {
  const length = channels[0]?.length || 0;
  const start = Math.max(0, startSample);
  const end = endSample == null ? length : Math.min(length, endSample);
  const span = Math.max(1, end - start);
  const binCount = Math.max(1, Math.min(4096, Math.round(buckets)));
  const step = span / binCount;

  return channels.map((ch) => {
    const peaks = new Float32Array(binCount * 2);
    for (let b = 0; b < binCount; b++) {
      const bStart = start + Math.floor(b * step);
      const bEnd = Math.min(end, Math.max(bStart + 1, start + Math.floor((b + 1) * step)));
      let minV = 1;
      let maxV = -1;
      for (let i = bStart; i < bEnd; i++) {
        const v = ch[i];
        if (v < minV) minV = v;
        if (v > maxV) maxV = v;
      }
      if (minV > maxV) {
        minV = 0;
        maxV = 0;
      }
      peaks[b * 2] = minV;
      peaks[b * 2 + 1] = maxV;
    }
    return peaks;
  });
}
