/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import * as dsp from './dsp.js';

export class WaveformView {
  constructor(canvas, engine) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.engine = engine;

    this.zoom = 1;
    this.scrollRatio = 0;
    this.dragging = false;
    this.dragStartSec = 0;
    this.cachedKey = '';
    this.cachedPeaks = null;

    this.bindEvents();
  }

  setZoom(nextZoom) {
    this.zoom = Math.max(1, Math.min(32, Number(nextZoom) || 1));
    if (this.zoom === 1) this.scrollRatio = 0;
    this.cachedKey = '';
    this.render();
  }

  getVisibleWindow(doc) {
    const totalDur = doc && doc.sampleRate > 0 ? doc.length / doc.sampleRate : 0;
    const visibleDur = totalDur / this.zoom;
    const maxStart = Math.max(0, totalDur - visibleDur);
    const startSec = Math.max(0, Math.min(maxStart, this.scrollRatio * totalDur));
    const endSec = Math.min(totalDur, startSec + visibleDur);
    return { totalDur, startSec, endSec, visibleDur };
  }

  xToSec(clientX, doc) {
    const rect = this.canvas.getBoundingClientRect();
    const ratio = rect.width > 0 ? Math.max(0, Math.min(1, (clientX - rect.left) / rect.width)) : 0;
    const { startSec, visibleDur } = this.getVisibleWindow(doc);
    return startSec + ratio * visibleDur;
  }

  bindEvents() {
    this.canvas.addEventListener('pointerdown', (e) => {
      const doc = this.engine.getActiveDocument();
      if (!doc) return;
      this.dragging = true;
      this.dragStartSec = this.xToSec(e.clientX, doc);
      this.canvas.setPointerCapture?.(e.pointerId);
      this.engine.seek(this.dragStartSec);
    });

    this.canvas.addEventListener('pointermove', (e) => {
      if (!this.dragging) return;
      const doc = this.engine.getActiveDocument();
      if (!doc) return;
      const curSec = this.xToSec(e.clientX, doc);
      if (Math.abs(curSec - this.dragStartSec) > 0.005) {
        this.engine.setSelection(this.dragStartSec, curSec);
      }
    });

    const finishDrag = (e) => {
      if (!this.dragging) return;
      this.dragging = false;
      const doc = this.engine.getActiveDocument();
      if (!doc) return;
      const curSec = this.xToSec(e.clientX, doc);
      if (Math.abs(curSec - this.dragStartSec) <= 0.005) {
        this.engine.clearSelection();
        this.engine.seek(curSec);
      } else {
        this.engine.setSelection(this.dragStartSec, curSec);
      }
    };

    this.canvas.addEventListener('pointerup', finishDrag);
    this.canvas.addEventListener('pointercancel', finishDrag);

    this.canvas.addEventListener(
      'wheel',
      (e) => {
        const doc = this.engine.getActiveDocument();
        if (!doc) return;
        e.preventDefault();
        if (e.shiftKey || Math.abs(e.deltaX) > Math.abs(e.deltaY)) {
          const delta = (e.deltaX || e.deltaY) * 0.0005;
          this.scrollRatio = Math.max(0, Math.min(1 - 1 / this.zoom, this.scrollRatio + delta));
          this.cachedKey = '';
          this.render();
        } else {
          const factor = e.deltaY < 0 ? 1.25 : 0.8;
          this.setZoom(this.zoom * factor);
        }
      },
      { passive: false }
    );
  }

  ensureSize() {
    const dpr = Math.max(1, window.devicePixelRatio || 1);
    const rect = this.canvas.getBoundingClientRect();
    const w = Math.max(300, Math.round(rect.width * dpr));
    const h = Math.max(180, Math.round(rect.height * dpr));
    if (this.canvas.width !== w || this.canvas.height !== h) {
      this.canvas.width = w;
      this.canvas.height = h;
      this.cachedKey = '';
    }
    return { w, h, dpr };
  }

  render() {
    const { w, h, dpr } = this.ensureSize();
    const ctx = this.ctx;
    ctx.clearRect(0, 0, w, h);

    ctx.fillStyle = '#090d16';
    ctx.fillRect(0, 0, w, h);

    const rulerH = Math.round(24 * dpr);
    const waveTop = rulerH;
    const waveH = h - rulerH;

    ctx.strokeStyle = 'rgba(148, 163, 184, 0.1)';
    ctx.lineWidth = Math.max(1, dpr);
    for (let i = 1; i < 4; i++) {
      const y = waveTop + (waveH * i) / 4;
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(w, y);
      ctx.stroke();
    }

    const doc = this.engine.getActiveDocument();
    if (!doc || doc.length === 0) {
      ctx.fillStyle = '#64748b';
      ctx.font = `${Math.round(13 * dpr)}px system-ui, sans-serif`;
      ctx.textAlign = 'center';
      ctx.fillText('Synthesize a tone or open an audio file from the sidebar', w / 2, h / 2);
      return;
    }

    const { startSec, endSec, visibleDur } = this.getVisibleWindow(doc);
    const startSample = Math.floor(startSec * doc.sampleRate);
    const endSample = Math.min(doc.length, Math.ceil(endSec * doc.sampleRate));

    ctx.fillStyle = '#0f172a';
    ctx.fillRect(0, 0, w, rulerH);
    ctx.strokeStyle = '#1e293b';
    ctx.beginPath();
    ctx.moveTo(0, rulerH);
    ctx.lineTo(w, rulerH);
    ctx.stroke();

    const tickTarget = Math.max(4, Math.floor(w / (110 * dpr)));
    const tickStep = visibleDur > 0 ? visibleDur / tickTarget : 1;
    ctx.fillStyle = '#94a3b8';
    ctx.font = `${Math.round(10 * dpr)}px ui-monospace, monospace`;
    ctx.textAlign = 'left';
    for (let i = 0; i <= tickTarget; i++) {
      const t = startSec + i * tickStep;
      const x = ((t - startSec) / (visibleDur || 1)) * w;
      ctx.strokeStyle = '#334155';
      ctx.beginPath();
      ctx.moveTo(x, rulerH - 6 * dpr);
      ctx.lineTo(x, rulerH);
      ctx.stroke();
      if (i < tickTarget) {
        ctx.fillText(`${t.toFixed(2)}s`, x + 4 * dpr, rulerH - 7 * dpr);
      }
    }

    if (doc.selection && visibleDur > 0) {
      const sx = ((doc.selection.startSec - startSec) / visibleDur) * w;
      const ex = ((doc.selection.endSec - startSec) / visibleDur) * w;
      const left = Math.max(0, sx);
      const right = Math.min(w, ex);
      if (right > left) {
        ctx.fillStyle = 'rgba(245, 158, 11, 0.16)';
        ctx.fillRect(left, waveTop, right - left, waveH);
        ctx.strokeStyle = 'rgba(251, 191, 36, 0.75)';
        ctx.lineWidth = Math.max(1, dpr);
        ctx.beginPath();
        ctx.moveTo(left, waveTop);
        ctx.lineTo(left, h);
        ctx.moveTo(right, waveTop);
        ctx.lineTo(right, h);
        ctx.stroke();
      }
    }

    const buckets = Math.min(w, 2048);
    const cacheKey = `${doc.id}:${doc.length}:${doc.undoStack.length}:${doc.redoStack.length}:${startSample}:${endSample}:${buckets}`;
    if (cacheKey !== this.cachedKey || !this.cachedPeaks) {
      this.cachedPeaks = dsp.computePeaks(doc.channels, buckets, startSample, endSample);
      this.cachedKey = cacheKey;
    }

    const chCount = this.cachedPeaks.length;
    const laneH = waveH / chCount;
    for (let c = 0; c < chCount; c++) {
      const peaks = this.cachedPeaks[c];
      const centerY = waveTop + laneH * (c + 0.5);
      const amp = laneH * 0.44;

      ctx.strokeStyle = 'rgba(56, 189, 248, 0.18)';
      ctx.beginPath();
      ctx.moveTo(0, centerY);
      ctx.lineTo(w, centerY);
      ctx.stroke();

      ctx.fillStyle = c === 0 ? '#38bdf8' : '#22d3ee';
      const binWidth = w / buckets;
      for (let b = 0; b < buckets; b++) {
        const minV = peaks[b * 2];
        const maxV = peaks[b * 2 + 1];
        const yTop = centerY - maxV * amp;
        const yBot = centerY - minV * amp;
        const barH = Math.max(dpr, yBot - yTop);
        ctx.fillRect(b * binWidth, yTop, Math.max(1, Math.ceil(binWidth)), barH);
      }
    }

    if (visibleDur > 0) {
      ctx.font = `${Math.round(10 * dpr)}px system-ui, sans-serif`;
      for (const marker of doc.markers) {
        if (marker.timeSec < startSec || marker.timeSec > endSec) continue;
        const mx = ((marker.timeSec - startSec) / visibleDur) * w;
        ctx.strokeStyle = marker.color || '#f59e0b';
        ctx.lineWidth = Math.max(1, dpr);
        ctx.beginPath();
        ctx.moveTo(mx, rulerH);
        ctx.lineTo(mx, h);
        ctx.stroke();

        const label = marker.label || `${marker.timeSec.toFixed(2)}s`;
        const textW = ctx.measureText(label).width + 8 * dpr;
        ctx.fillStyle = marker.color || '#f59e0b';
        ctx.fillRect(mx, 2 * dpr, Math.min(textW, 120 * dpr), 16 * dpr);
        ctx.fillStyle = '#090d16';
        ctx.fillText(label, mx + 4 * dpr, 13 * dpr);
      }

      const ph = this.engine.transport.playheadSec;
      if (ph >= startSec && ph <= endSec) {
        const px = ((ph - startSec) / visibleDur) * w;
        ctx.strokeStyle = '#f8fafc';
        ctx.lineWidth = Math.max(1.5, 1.5 * dpr);
        ctx.beginPath();
        ctx.moveTo(px, 0);
        ctx.lineTo(px, h);
        ctx.stroke();
      }
    }
  }
}

export class SpectrumView {
  constructor(canvas, engine) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.engine = engine;
  }

  render() {
    const dpr = Math.max(1, window.devicePixelRatio || 1);
    const rect = this.canvas.getBoundingClientRect();
    const w = Math.max(220, Math.round(rect.width * dpr));
    const h = Math.max(90, Math.round(rect.height * dpr));
    if (this.canvas.width !== w || this.canvas.height !== h) {
      this.canvas.width = w;
      this.canvas.height = h;
    }

    const ctx = this.ctx;
    ctx.fillStyle = '#0b1220';
    ctx.fillRect(0, 0, w, h);

    const doc = this.engine.getActiveDocument();
    if (!doc || doc.length === 0) return;

    const spec = dsp.computeSpectrum(doc.channels, doc.sampleRate, 1024, this.engine.transport.playheadSec);
    const bins = spec.magnitudesDb.length;
    const bars = Math.min(72, bins);
    const barW = w / bars;

    for (let i = 0; i < bars; i++) {
      const norm = i / bars;
      const bin = Math.min(bins - 1, Math.floor(Math.pow(norm, 1.65) * bins));
      const db = spec.magnitudesDb[bin];
      const level = Math.max(0, Math.min(1, (db + 96) / 96));
      const barH = level * (h - 14 * dpr);
      const x = i * barW;
      const y = h - barH;
      ctx.fillStyle = level > 0.78 ? '#f59e0b' : '#38bdf8';
      ctx.fillRect(x + 1, y, Math.max(1, barW - 2), barH);
    }
  }
}
