/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from '../lib/i18n.js';
import { findSiteAction, getAllSiteActions } from '../lib/site-packs.js';
import { ACTION_META_MAP, LOCAL_META_MAP, LOCAL_NODE_KINDS, STEP_ACTIONS, isLocalNode, normalizeStep } from '../lib/workflow.js';
import { el, icon, openModal, svgEl } from './dom.js';

const NODE_W = 232;
const NODE_H = 88;
const START_X = 48;
const STEP_GAP_X = 282;
const DEFAULT_Y = 128;

export function defaultNodePos(nodeKey, step = null) {
  if (nodeKey === 'trigger') return { x: START_X, y: DEFAULT_Y };
  const idx = Number(String(nodeKey).replace('step:', '')) || 0;
  let yOffset = idx % 2 === 1 ? 26 : 0;
  if (step?.runWhen === 'if_true') yOffset = -56;
  else if (step?.runWhen === 'if_false') yOffset = 98;
  return { x: START_X + (idx + 1) * STEP_GAP_X, y: Math.max(24, DEFAULT_Y + yOffset) };
}

function summarizeStep(raw, customActions, fallbackService) {
  const s = normalizeStep(raw);
  if (s.kind === 'site_action') {
    const def = findSiteAction(s.siteActionId, customActions);
    return `${s.serviceId || def?.serviceId || fallbackService} · ${s.save_as ? `-> {{${s.save_as}}}` : s.siteActionId}`;
  }
  if (s.kind === 'transform') return `local · -> {{${s.save_as || 'transformed'}}}`;
  if (s.kind === 'condition') {
    const modeTag = s.onFalse === 'branch' ? ' [IF/ELSE]' : '';
    return `IF ${s.path || '?'} ${s.operator || 'not_empty'} ${s.expected || ''}${modeTag}`.trim();
  }
  if (s.kind === 'foreach') {
    const innerLabel = s.bodyStep?.siteActionId || s.bodyStep?.action || s.bodyStep?.kind || 'step';
    return `EACH ${s.itemsPath || '?'} -> ${innerLabel}`;
  }
  if (s.kind === 'http') return `${s.method || 'GET'} ${s.url || ''}`;
  if (s.kind === 'save_file') return `fs · ${s.path || 'exports/output.json'}`;
  const svc = s.serviceId || fallbackService;
  if (s.action === 'open') return `${svc} · ${s.url || ''}`;
  if (s.action === 'sleep') return `${svc} · ${s.ms || 1000} ms`;
  if (s.action === 'eval') return `${svc} · ${s.save_as ? `-> {{${s.save_as}}}` : 'JS'}`;
  if (s.action === 'press') return `${svc} · ${s.key || ''}`;
  if (s.locator) {
    const loc = s.locator.kind === 'role'
      ? `role=${s.locator.role || ''} ${s.locator.name || ''}`.trim()
      : `${s.locator.kind}:${s.locator.value || ''}`;
    return `${svc} · ${loc}`;
  }
  return `${svc} · ${s.contains || s.equals || ''}`;
}

function connectNodes(svgLayer, fromNode, toNode, cls, labelText = '') {
  const x1 = fromNode.pos.x + NODE_W;
  const y1 = fromNode.pos.y + NODE_H / 2;
  const x2 = toNode.pos.x;
  const y2 = toNode.pos.y + NODE_H / 2;
  const dx = Math.max(48, Math.abs(x2 - x1) * 0.5);
  const d = `M ${x1} ${y1} C ${x1 + dx} ${y1}, ${x2 - dx} ${y2}, ${x2} ${y2}`;
  svgLayer.appendChild(svgEl('path', { d, class: cls }));
  if (labelText) {
    const mx = (x1 + x2) / 2;
    const my = (y1 + y2) / 2 - 6;
    const textNode = svgEl('text', { x: mx, y: my, 'text-anchor': 'middle', class: 'edge-label' });
    textNode.textContent = labelText;
    svgLayer.appendChild(textNode);
  }
}

function drawEdges(svgLayer, nodes, latestRun) {
  svgLayer.replaceChildren();
  const statusByIndex = new Map((latestRun?.steps || []).map((s) => [s.index, s.status]));
  const stepStatusCls = (fromNode, baseCls) => {
    const st = fromNode.stepIndex !== undefined ? statusByIndex.get(fromNode.stepIndex) : null;
    if (st === 'passed' || st === 'completed' || st === 'pinned' || st === 'cached') return `${baseCls} completed`;
    if (st === 'running' || st === 'paused_auth') return `${baseCls} running`;
    return baseCls;
  };

  let activeCondNode = null;
  let lastTrueNode = null;
  let lastFalseNode = null;
  let lastLinearNode = nodes[0] || null;

  for (let i = 1; i < nodes.length; i++) {
    const cur = nodes[i];
    const runWhen = cur.step?.runWhen || 'always';

    if (activeCondNode && (runWhen === 'if_true' || runWhen === 'if_false')) {
      if (runWhen === 'if_true') {
        const src = lastTrueNode || activeCondNode;
        const lbl = lastTrueNode ? '' : 'TRUE';
        connectNodes(svgLayer, src, cur, stepStatusCls(src, 'edge-path edge-true'), lbl);
        lastTrueNode = cur;
      } else {
        const src = lastFalseNode || activeCondNode;
        const lbl = lastFalseNode ? '' : 'FALSE';
        connectNodes(svgLayer, src, cur, stepStatusCls(src, 'edge-path edge-false'), lbl);
        lastFalseNode = cur;
      }
    } else {
      if (activeCondNode && (lastTrueNode || lastFalseNode)) {
        if (lastTrueNode) connectNodes(svgLayer, lastTrueNode, cur, stepStatusCls(lastTrueNode, 'edge-path edge-true'));
        if (lastFalseNode) connectNodes(svgLayer, lastFalseNode, cur, stepStatusCls(lastFalseNode, 'edge-path edge-false'));
        if (!lastTrueNode) connectNodes(svgLayer, activeCondNode, cur, stepStatusCls(activeCondNode, 'edge-path edge-true'), 'TRUE');
        if (!lastFalseNode) connectNodes(svgLayer, activeCondNode, cur, stepStatusCls(activeCondNode, 'edge-path edge-false'), 'FALSE');
      } else if (lastLinearNode) {
        const isFirst = lastLinearNode.key === 'trigger';
        const trigDone = latestRun?.status === 'passed' || latestRun?.status === 'completed' || latestRun?.status === 'partial';
        const cls = isFirst
          ? (trigDone ? 'edge-path completed' : 'edge-path active')
          : stepStatusCls(lastLinearNode, 'edge-path active');
        connectNodes(svgLayer, lastLinearNode, cur, cls);
      }
      activeCondNode = cur.step?.kind === 'condition' && cur.step?.onFalse === 'branch' ? cur : null;
      lastTrueNode = null;
      lastFalseNode = null;
      lastLinearNode = cur;
    }
  }
}

export function renderWorkflowCanvas(container, ctx) {
  const {
    workflow,
    selectedKey,
    serviceState,
    customActions = [],
    latestRun,
    running,
    queueLength = 0,
    authPause = null,
    onboarding = null,
    onSelectNode,
    onAddNode,
    onMoveNode,
    onSave,
    onRun,
    onCancelRun,
    onStartRecording,
    onDismissOnboarding,
    onApplyHealSuggestion,
    onFocusAuthTab,
    onOpenService,
    onExportJson,
  } = ctx;

  if (!workflow) {
    container.replaceChildren(
      el('div.view-scroll', {}, el('div.card', {}, el('p.muted', {}, t('Select or create a workflow'))))
    );
    return;
  }

  const openAddStepDialog = () => {
    let modal = null;
    const allSiteActions = getAllSiteActions(customActions);
    const choose = (payload) => { modal?.close(); onAddNode(payload); };

    const body = el(
      'div',
      { style: 'display:flex;flex-direction:column;gap:14px' },
      el('div', {},
        el('div.field-label', { style: 'margin-bottom:6px' }, t('Site Actions')),
        el('div.row-list', {}, ...allSiteActions.map((sa) =>
          el('button.list-row', { type: 'button', style: 'cursor:pointer;text-align:left', onclick: () => choose({ kind: 'site_action', siteActionId: sa.id }) },
            el('div', {}, el('div', { style: 'font-weight:600' }, sa.name), el('div.muted', { style: 'font-size:11px' }, sa.description || sa.id)),
            el('span.pill.pill-accent', {}, sa.serviceId)
          )
        ))
      ),
      el('div', {},
        el('div.field-label', { style: 'margin-bottom:6px' }, t('Logic & Hybrid')),
        el('div.row-list', {}, ...LOCAL_NODE_KINDS.map((lk) =>
          el('button.list-row', { type: 'button', style: 'cursor:pointer;text-align:left', onclick: () => choose({ kind: lk.id }) },
            el('div', {}, el('div', { style: 'font-weight:600' }, t(lk.label)), el('div.muted.mono', { style: 'font-size:11px' }, lk.id)),
            el('span.pill.pill-warn', {}, 'local')
          )
        ))
      ),
      el('div', {},
        el('div.field-label', { style: 'margin-bottom:6px' }, t('Browser Primitives')),
        el('div.row-list', {}, ...STEP_ACTIONS.map((act) =>
          el('button.list-row', { type: 'button', style: 'cursor:pointer;text-align:left', onclick: () => choose({ kind: 'step', action: act.id }) },
            el('div', {}, el('div', { style: 'font-weight:600' }, t(act.label)), el('div.muted.mono', { style: 'font-size:11px' }, act.id)),
            el('span.pill', {}, act.category)
          )
        ))
      )
    );
    modal = openModal({ title: t('+ Add Step'), body });
  };

  const topbar = el(
    'div.topbar',
    {},
    el(
      'div.topbar-left',
      {},
      el('span', { style: 'font-weight:700;font-size:15px' }, workflow.name || workflow.id),
      el('span.pill.pill-accent', {}, workflow.serviceId),
      el(
        'span',
        { class: `pill${serviceState?.open ? ' pill-good' : ''}` },
        el('span', { class: `dot${serviceState?.open ? ' open' : ''}` }),
        serviceState?.open ? t('Open') : t('Closed')
      ),
      queueLength > 0 ? el('span.pill.pill-info', {}, t('Queue: {n}', { n: queueLength })) : null,
      authPause
        ? el(
            'button.btn.btn-sm',
            { type: 'button', style: 'border-color:var(--warn);color:var(--warn)', onclick: () => onFocusAuthTab?.(authPause.serviceId) },
            icon('lock', 13),
            t('Login required ({service})', { service: authPause.serviceId })
          )
        : null,
      el(
        'button.btn.btn-sm',
        { type: 'button', onclick: () => onOpenService(workflow.serviceId) },
        icon('tab', 13),
        serviceState?.open ? t('Focus Tab') : t('Open Tab')
      )
    ),
    el(
      'div.topbar-right',
      {},
      onStartRecording
        ? el('button.btn.btn-sm', { type: 'button', onclick: onStartRecording }, el('span.rec-dot'), t('Record'))
        : null,
      el('button.btn.btn-sm', { type: 'button', onclick: openAddStepDialog }, icon('plus', 13), t('+ Add Step')),
      el('button.btn.btn-sm', { type: 'button', onclick: onExportJson }, icon('download', 13), t('Export JSON')),
      el('button.btn.btn-sm.btn-primary', { type: 'button', onclick: onSave }, icon('save', 13), t('Save')),
      running
        ? el('button.btn.btn-sm.btn-danger', { type: 'button', onclick: onCancelRun }, icon('stop', 13), t('Cancel'))
        : null,
      el(
        'button.btn.btn-sm.btn-good',
        { type: 'button', onclick: onRun },
        icon('play', 13),
        running ? t('Queue Run') : t('Run Now')
      )
    )
  );

  const stage = el('div.canvas-stage');
  const board = el('div.canvas-board');
  const svgLayer = svgEl('svg', { class: 'canvas-edges', width: '100%', height: '100%' });
  board.appendChild(svgLayer);

  const stepResults = new Map((latestRun?.steps || []).map((s) => [s.index, s]));
  const nodes = [];

  const getPos = (key, step = null) => {
    const p = workflow.nodePositions?.[key];
    return p && typeof p.x === 'number' && typeof p.y === 'number' ? { ...p } : defaultNodePos(key, step);
  };

  const triggerPos = getPos('trigger');
  const triggerSub = workflow.trigger?.type === 'manual'
    ? t('Manual run')
    : workflow.trigger?.type === 'cron'
    ? `cron · ${workflow.trigger?.cronExpr || '0 9 * * 1-5'}`
    : workflow.trigger?.type === 'webhook'
    ? `POST /__ladybird_webhook/${workflow.trigger?.id || `trig-${workflow.id}`}`
    : `${workflow.trigger?.type}${workflow.trigger?.urlContains ? ` · ${workflow.trigger.urlContains}` : ''}`;

  nodes.push({
    key: 'trigger',
    catClass: 'cat-trigger',
    badge: t('Trigger'),
    title: t(
      workflow.trigger?.type === 'interval' ? 'Every interval (cron)' :
      workflow.trigger?.type === 'cron' ? 'Cron schedule (5-field)' :
      workflow.trigger?.type === 'webhook' ? 'Incoming Webhook (HTTP POST)' :
      workflow.trigger?.type === 'network' ? 'On service network call' :
      workflow.trigger?.type === 'notification' ? 'On browser notification' :
      workflow.trigger?.type === 'navigation' ? 'On tab navigation' : 'Manual run'
    ),
    sub: `${workflow.serviceId} · ${triggerSub}`,
    pos: triggerPos,
    status: null,
    metaTags: [],
  });

  (workflow.steps || []).forEach((rawStep, idx) => {
    const step = normalizeStep(rawStep);
    const key = `step:${idx}`;
    const sr = stepResults.get(idx);
    let category = 'data';
    let title = step.label || step.action || step.kind;
    let svcBadge = isLocalNode(step) ? 'local' : (step.serviceId || workflow.serviceId);

    if (step.kind === 'site_action') {
      const saDef = findSiteAction(step.siteActionId, customActions);
      category = saDef?.category || 'data';
      title = step.label || saDef?.name || step.siteActionId;
      svcBadge = step.serviceId || saDef?.serviceId || workflow.serviceId;
    } else if (isLocalNode(step)) {
      category = 'local';
      title = step.label || t(LOCAL_META_MAP[step.kind]?.label || step.kind);
      svcBadge = 'local';
    } else {
      const meta = ACTION_META_MAP[step.action] || { label: step.action, category: 'data' };
      category = meta.category || 'data';
      title = step.label || t(meta.label);
    }

    const metaTags = [];
    if (workflow.pinnedOutputs && Object.prototype.hasOwnProperty.call(workflow.pinnedOutputs, String(idx))) {
      metaTags.push('PIN');
    }
    if (step.runWhen === 'if_true') metaTags.push('TRUE');
    if (step.runWhen === 'if_false') metaTags.push('FALSE');
    if (step.kind === 'foreach') metaTags.push('LOOP');
    if (step.kind === 'transform') metaTags.push('SANDBOX');
    if (step.requireApproval) metaTags.push('APPROVAL');
    if (step.retryCount > 0) metaTags.push(`↻${step.retryCount}`);

    nodes.push({
      key,
      stepIndex: idx,
      step,
      catClass: `cat-${category}`,
      badge: `${t('Step')} ${idx + 1} · ${svcBadge}`,
      title,
      sub: summarizeStep(step, customActions, workflow.serviceId),
      pos: getPos(key, step),
      status: sr?.status || null,
      metaTags,
    });
  });

  let maxRight = 980;
  let maxBottom = 520;
  for (const n of nodes) {
    maxRight = Math.max(maxRight, n.pos.x + NODE_W + 280);
    maxBottom = Math.max(maxBottom, n.pos.y + NODE_H + 140);
  }
  board.style.width = `${maxRight}px`;
  board.style.height = `${maxBottom}px`;

  nodes.forEach((n) => {
    const mappedStatus = n.status === 'passed' ? 'completed' : n.status;
    const statusCls = mappedStatus ? ` status-${mappedStatus}` : '';
    const selCls = selectedKey === n.key ? ' selected' : '';
    const rightPills = [];
    for (const tag of n.metaTags || []) {
      rightPills.push(el('span.pill', { style: 'font-size:10px;padding:1px 5px' }, tag));
    }
    if (n.status) rightPills.push(el('span.pill', {}, n.status));

    const nodeEl = el(
      `div.flow-node ${n.catClass}${statusCls}${selCls}`,
      {
        style: `left:${n.pos.x}px;top:${n.pos.y}px;width:${NODE_W}px`,
        tabindex: '0',
      },
      el(
        'div.node-head',
        {},
        el('span.truncate', {}, n.badge),
        el('div.chip-row', { style: 'gap:3px;flex-wrap:nowrap' }, ...rightPills)
      ),
      el(
        'div.node-body',
        {},
        el('div.node-title', {}, n.title),
        el('div.node-sub', {}, n.sub || '—')
      )
    );

    nodeEl.addEventListener('mousedown', (e) => {
      if (e.button !== 0) return;
      const startX = e.clientX;
      const startY = e.clientY;
      const origX = n.pos.x;
      const origY = n.pos.y;
      let moved = false;

      const onMove = (ev) => {
        const dx = ev.clientX - startX;
        const dy = ev.clientY - startY;
        if (!moved && Math.abs(dx) + Math.abs(dy) < 4) return;
        moved = true;
        n.pos.x = Math.max(16, Math.round(origX + dx));
        n.pos.y = Math.max(16, Math.round(origY + dy));
        nodeEl.style.left = `${n.pos.x}px`;
        nodeEl.style.top = `${n.pos.y}px`;
        drawEdges(svgLayer, nodes, latestRun);
      };

      const onUp = () => {
        window.removeEventListener('mousemove', onMove);
        window.removeEventListener('mouseup', onUp);
        if (moved) onMoveNode(n.key, n.pos);
        else onSelectNode(n.key);
      };

      window.addEventListener('mousemove', onMove);
      window.addEventListener('mouseup', onUp);
    });

    board.appendChild(nodeEl);
  });

  const last = nodes[nodes.length - 1];
  const addBtn = el(
    'button.canvas-add-pill',
    {
      type: 'button',
      style: `left:${last.pos.x + NODE_W + 32}px;top:${last.pos.y + 24}px`,
      onclick: openAddStepDialog,
    },
    icon('plus', 14),
    t('+ Add Step')
  );
  board.appendChild(addBtn);
  drawEdges(svgLayer, nodes, latestRun);
  stage.appendChild(board);

  const outputText = latestRun
    ? JSON.stringify(
        {
          runId: latestRun.run_id,
          status: latestRun.status,
          durationMs: latestRun.duration_ms,
          error: latestRun.error || undefined,
          outputs: latestRun.outputs,
          steps: (latestRun.steps || []).map((s) => ({
            index: s.index,
            serviceId: s.serviceId,
            action: s.action,
            status: s.status,
            attempts: s.attempts > 1 ? s.attempts : undefined,
            durationMs: s.duration_ms,
            output: s.output,
            error: s.error,
          })),
        },
        null,
        2
      )
    : t('No run output yet. Click Run Now to execute on the service tab.');

  const failShots = (latestRun?.steps || []).filter((s) => typeof s.failureScreenshot === 'string' && s.failureScreenshot.startsWith('data:image/'));
  const healItems = [];
  for (const s of latestRun?.steps || []) {
    if (s.status === 'failed' && Array.isArray(s.healSuggestions) && s.healSuggestions.length) {
      for (const cand of s.healSuggestions) {
        healItems.push({ stepIndex: s.index, stepLabel: s.label, cand });
      }
    }
  }
  const healBox = healItems.length
    ? el(
        'div.heal-box',
        {},
        el('div.chip-row', {}, icon('wrench', 13), el('span', { style: 'font-weight:700;font-size:12px' }, t('Auto-Heal Selector Suggestions'))),
        ...healItems.map((h) =>
          el(
            'div.heal-row',
            {},
            el(
              'div',
              { style: 'min-width:0;flex:1' },
              el('div.mono', { style: 'font-size:11.5px;font-weight:600' }, `${t('Step')} ${h.stepIndex + 1}: ${h.cand.locator.kind}=${h.cand.locator.value || h.cand.locator.name || ''}`),
              el('div.muted', { style: 'font-size:11px' }, `${h.cand.reason} · ${h.cand.previewText}`)
            ),
            onApplyHealSuggestion
              ? el('button.btn.btn-sm.btn-primary', { type: 'button', onclick: () => onApplyHealSuggestion(h.stepIndex, h.cand.locator) }, t('Apply & Retry'))
              : null
          )
        )
      )
    : null;

  const statusTone = (latestRun?.status === 'passed' || latestRun?.status === 'completed')
    ? 'pill-good'
    : (latestRun?.status === 'failed' || latestRun?.status === 'cancelled')
    ? 'pill-danger'
    : 'pill-warn';
  const outputBar = el(
    'div.output-bar',
    {},
    el(
      'div.output-head',
      {},
      el('span', {}, t('Execution Output')),
      latestRun ? el(`span.pill ${statusTone}`, {}, `${latestRun.status} · ${latestRun.duration_ms || 0} ms`) : null
    ),
    el(
      'div.output-body',
      {},
      healBox,
      el('pre', { style: 'margin:0;white-space:pre-wrap;word-break:break-word' }, outputText),
      ...failShots.map((fs) => el('img.fail-shot-img', { src: fs.failureScreenshot, alt: fs.label || 'Failure screenshot' }))
    )
  );

  let onboardBar = null;
  if (onboarding && !onboarding.dismissed) {
    onboardBar = el(
      'div.onboard-banner',
      {},
      el(
        'div.chip-row',
        {},
        el('span', { style: 'font-weight:700;font-size:12px' }, t('Quick Start (60s):')),
        el('button.onboard-step' + (onboarding.serviceOpen ? '.done' : ''), { type: 'button', onclick: () => onOpenService(workflow.serviceId) }, icon(onboarding.serviceOpen ? 'check' : 'tab', 12), t('1. Sign in to service tab')),
        el('button.onboard-step' + (onboarding.hasRun ? '.done' : ''), { type: 'button', onclick: onRun }, icon(onboarding.hasRun ? 'check' : 'play', 12), t('2. Run a workflow')),
        onStartRecording
          ? el('button.onboard-step', { type: 'button', onclick: onStartRecording }, icon('record', 12), t('3. Record custom action'))
          : null
      ),
      onDismissOnboarding
        ? el('button.btn.btn-sm', { type: 'button', onclick: onDismissOnboarding }, icon('x', 12), t('Dismiss'))
        : null
    );
  }

  container.replaceChildren(...[topbar, onboardBar, stage, outputBar].filter(Boolean));
}
