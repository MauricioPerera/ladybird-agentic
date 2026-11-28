/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { t } from '../lib/i18n.js';
import { createCustomSiteActionTemplate, getAllSiteActions, validateCustomSiteAction } from '../lib/site-packs.js';
import { SESSION_PACK_TEMPLATES } from '../lib/templates.js';
import { el, icon, openModal, toast } from './dom.js';

export function renderTriggersView(container, ctx) {
  const { triggers, onToggleTrigger, onDeleteTrigger, onOpenFlow } = ctx;
  const rows = (triggers || []).map((tr) =>
    el(
      'div.list-row',
      { style: 'flex-wrap:wrap' },
      el(
        'div',
        { style: 'min-width:0;flex:1' },
        el(
          'div.chip-row',
          {},
          el('span', { style: 'font-weight:700' }, tr.name || tr.id),
          el('span.pill.pill-warn', {}, tr.type),
          el('span.pill.pill-accent', {}, tr.serviceId),
          el('span', { class: `pill${tr.enabled ? ' pill-good' : ''}` }, tr.enabled ? t('Enabled') : t('Pause'))
        ),
        el(
          'div.muted.mono',
          { style: 'font-size:11px;margin-top:4px' },
          [
            tr.type === 'interval' ? `${tr.intervalMs}ms` : null,
            tr.type === 'cron' ? `cron "${tr.cronExpr || '0 9 * * 1-5'}"` : null,
            tr.urlContains ? `url~"${tr.urlContains}"` : null,
            tr.method || null,
            tr.statusCode ? `HTTP ${tr.statusCode}` : null,
            t('Fires: {count}', { count: tr.fireCount || 0 }),
            tr.lastRunId ? t('Last run: {id}', { id: tr.lastRunId }) : null,
          ].filter(Boolean).join(' · ')
        )
      ),
      el(
        'div.chip-row',
        {},
        el('button.btn.btn-sm', { type: 'button', onclick: () => onOpenFlow(tr.flowId) }, t('Open Flow')),
        el('button.btn.btn-sm', { type: 'button', onclick: () => onToggleTrigger(tr.id, !tr.enabled) }, tr.enabled ? t('Pause') : t('Enable')),
        el('button.btn.btn-sm.btn-danger', { type: 'button', onclick: () => onDeleteTrigger(tr.id) }, icon('trash', 12))
      )
    )
  );

  container.replaceChildren(
    el(
      'div.view-scroll',
      {},
      el(
        'div.card',
        {},
        el('div.card-head', {}, el('h2.card-title', {}, t('Active Triggers')), el('span.pill', {}, String((triggers || []).length))),
        rows.length
          ? el('div.row-list', {}, ...rows)
          : el('p.muted', {}, t('No triggers registered yet. Edit any workflow trigger from Manual to Interval, Network, Notification, or Navigation.'))
      )
    )
  );
}

export function renderEventsView(container, ctx) {
  const { events, filterType, onFilterType, onCreateTriggerFromEvent } = ctx;
  const types = [
    { id: 'all', label: 'All' },
    { id: 'notification', label: 'Notification' },
    { id: 'network', label: 'Network' },
    { id: 'navigation', label: 'Navigation' },
  ];
  const filtered = (events || []).filter((ev) => filterType === 'all' || ev.type === filterType);

  const rows = filtered.slice(0, 60).map((ev) => {
    const d = ev.data || {};
    const summary = ev.type === 'network'
      ? `${d.method || 'GET'} ${d.statusCode || ''} · ${d.url || ''}`.trim()
      : ev.type === 'notification'
      ? `${d.title || ''} — ${d.body || ''}`.trim()
      : `${d.title || ''} · ${d.url || ''}`.trim();
    const tone = ev.type === 'network' ? 'pill-info' : ev.type === 'notification' ? 'pill-warn' : 'pill-accent';
    return el(
      'div.list-row',
      { style: 'flex-wrap:wrap' },
      el(
        'div',
        { style: 'min-width:0;flex:1' },
        el(
          'div.chip-row',
          {},
          el(`span.pill ${tone}`, {}, ev.type),
          el('span.pill', {}, ev.serviceId),
          el('span.muted.mono', { style: 'font-size:11px' }, new Date(ev.timestampMs || Date.now()).toLocaleTimeString())
        ),
        el('div.truncate.mono', { style: 'font-size:11.5px;margin-top:4px' }, summary || '—')
      ),
      el(
        'button.btn.btn-sm.btn-primary',
        { type: 'button', onclick: () => onCreateTriggerFromEvent(ev) },
        icon('bolt', 12),
        t('Create Trigger')
      )
    );
  });

  container.replaceChildren(
    el(
      'div.view-scroll',
      {},
      el(
        'div.card',
        {},
        el(
          'div.card-head',
          {},
          el('div', {}, el('h2.card-title', {}, t('Live Events')), el('p.muted', { style: 'margin:4px 0 0' }, t('Stream of live browser events (notifications, network calls, navigations) from allowed service tabs.'))),
          el(
            'div.chip-row',
            {},
            ...types.map((tp) =>
              el(
                'button',
                { type: 'button', class: `btn btn-sm${filterType === tp.id ? ' btn-primary' : ''}`, onclick: () => onFilterType(tp.id) },
                t(tp.label)
              )
            )
          )
        ),
        rows.length
          ? el('div.row-list', {}, ...rows)
          : el('p.muted', {}, t('No events captured yet. Interact with any opened service tab to see real-time triggers here.'))
      )
    )
  );
}

export function renderRunsView(container, ctx) {
  const { runs, onRefreshRuns, onCancelRun, onOpenFlow } = ctx;
  const rows = (runs || []).map((r) => {
    const tone = (r.status === 'passed' || r.status === 'completed')
      ? 'pill-good'
      : (r.status === 'failed' || r.status === 'cancelled' || r.status === 'rejected')
      ? 'pill-danger'
      : 'pill-warn';
    const failShots = (r.steps || []).filter((s) => typeof s.failureScreenshot === 'string' && s.failureScreenshot.startsWith('data:image/'));
    return el(
      'div.list-row',
      { style: 'flex-direction:column;align-items:stretch' },
      el(
        'div',
        { style: 'display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap' },
        el(
          'div.chip-row',
          {},
          el('span', { style: 'font-weight:700' }, r.flow_name || r.flow_id),
          el(`span.pill ${tone}`, {}, r.status),
          el('span.muted.mono', { style: 'font-size:11px' }, `${r.run_id} · ${r.duration_ms || 0} ms`)
        ),
        el(
          'div.chip-row',
          {},
          (r.status === 'running' || r.status === 'paused_auth' || r.status === 'paused_approval')
            ? el('button.btn.btn-sm.btn-danger', { type: 'button', onclick: () => onCancelRun(r.run_id) }, t('Cancel'))
            : null,
          el('button.btn.btn-sm', { type: 'button', onclick: () => onOpenFlow(r.flow_id) }, t('Open Flow'))
        )
      ),
      r.error ? el('div', { style: 'color:var(--danger);font-size:12px' }, r.error) : null,
      ...failShots.map((fs) => el('img.fail-shot-img', { src: fs.failureScreenshot, alt: fs.label || 'Failure screenshot' })),
      r.outputs && Object.keys(r.outputs).length
        ? el('pre.output-body', { style: 'background:var(--surface);border:1px solid var(--border);border-radius:8px;max-height:160px' }, JSON.stringify(r.outputs, null, 2))
        : null
    );
  });

  container.replaceChildren(
    el(
      'div.view-scroll',
      {},
      el(
        'div.card',
        {},
        el('div.card-head', {}, el('h2.card-title', {}, t('Execution History')), el('button.btn.btn-sm', { type: 'button', onclick: onRefreshRuns }, t('Refresh'))),
        rows.length ? el('div.row-list', {}, ...rows) : el('p.muted', {}, t('No flow executions recorded yet.'))
      )
    )
  );
}

export function renderTemplatesView(container, { services = [], customActions = [], workspacePacks = [], onInstallTemplate, onSaveCustomAction, onDeleteCustomAction, onRecordAction, onExportPack, onImportPack, onOpenWorkspacePack }) {
  const openCustomActionModal = () => {
    const draft = createCustomSiteActionTemplate(services[0]?.id || 'mail');
    const idInp = el('input.input.mono', { value: draft.id });
    const nameInp = el('input.input', { value: draft.name });
    const svcSel = el('select.select', {}, ...(services.length ? services : [{ id: 'mail', label: 'Gmail' }]).map((s) => el('option', { value: s.id, selected: s.id === draft.serviceId }, `${s.label} (${s.id})`)));
    const descInp = el('input.input', { value: draft.description });
    const varInp = el('input.input.mono', { value: draft.defaultSaveAs });
    const scriptArea = el('textarea.textarea', { value: draft.steps[0].script });

    let dlg = null;
    dlg = openModal({
      title: t('+ Custom Site Action'),
      body: el(
        'div.row-list',
        {},
        el('div.field', {}, el('label.field-label', {}, t('Action ID')), idInp),
        el('div.field', {}, el('label.field-label', {}, t('Name')), nameInp),
        el('div.field', {}, el('label.field-label', {}, t('Target Service')), svcSel),
        el('div.field', {}, el('label.field-label', {}, t('Description')), descInp),
        el('div.field', {}, el('label.field-label', {}, t('Save output as variable')), varInp),
        el('div.field', {}, el('label.field-label', {}, t('Script (must end with return ...)')), scriptArea)
      ),
      foot: el(
        'div.chip-row',
        {},
        el('button.btn', { type: 'button', onclick: () => dlg.close() }, t('Cancel')),
        el('button.btn.btn-primary', {
          type: 'button',
          onclick: async () => {
            const def = {
              id: idInp.value.trim(),
              name: nameInp.value.trim(),
              serviceId: svcSel.value,
              category: 'data',
              description: descInp.value.trim(),
              defaultSaveAs: varInp.value.trim() || 'custom_result',
              fields: draft.fields,
              steps: [{ action: 'eval', label: nameInp.value.trim(), script: scriptArea.value }],
            };
            const errs = validateCustomSiteAction(def);
            if (errs.length) {
              toast(errs[0], { tone: 'warn' });
              return;
            }
            dlg.close();
            await onSaveCustomAction(def);
          },
        }, t('Save Site Action'))
      ),
    });
  };

  const cards = SESSION_PACK_TEMPLATES.map((tpl) =>
    el(
      'div.card',
      { style: 'display:flex;flex-direction:column;gap:8px' },
      el(
        'div.card-head',
        { style: 'margin-bottom:0' },
        el('span.pill.pill-accent', {}, tpl.serviceId),
        el('span.pill.pill-warn', {}, tpl.trigger?.type || 'manual')
      ),
      el('div', { style: 'font-weight:700;font-size:14px' }, t(tpl.name)),
      el('p.muted', { style: 'margin:0;flex:1' }, t(tpl.summary)),
      el(
        'button.btn.btn-sm.btn-primary',
        { type: 'button', onclick: () => onInstallTemplate(tpl.id) },
        icon('plus', 13),
        t('Install & Open')
      )
    )
  );

  const allSiteActions = getAllSiteActions(customActions);
  const actionRows = allSiteActions.map((sa) =>
    el(
      'div.list-row',
      {},
      el(
        'div',
        { style: 'min-width:0;flex:1' },
        el(
          'div.chip-row',
          {},
          el('span', { style: 'font-weight:700' }, sa.name),
          el('span.pill.pill-accent', {}, sa.serviceId),
          el('span.pill', {}, sa.builtin ? t('Built-in') : t('Custom'))
        ),
        el('div.muted', { style: 'font-size:11.5px;margin-top:3px' }, sa.description || sa.id)
      ),
      !sa.builtin
        ? el('button.btn.btn-sm.btn-danger', { type: 'button', onclick: () => onDeleteCustomAction(sa.id) }, icon('trash', 12))
        : null
    )
  );

  const packRows = (workspacePacks || []).map((p) =>
    el(
      'div.list-row',
      {},
      el('div', { style: 'min-width:0;flex:1' }, el('span.mono', { style: 'font-weight:700' }, p.name || p)),
      onOpenWorkspacePack
        ? el('button.btn.btn-sm.btn-primary', { type: 'button', onclick: () => onOpenWorkspacePack(p.path || p) }, t('Review & Import'))
        : null
    )
  );

  container.replaceChildren(
    el(
      'div.view-scroll',
      {},
      el(
        'div.card-head',
        { style: 'margin-bottom:0' },
        el('div', {}, el('h2', { style: 'margin:0 0 4px' }, t('Workflow Templates')), el('p.muted', { style: 'margin:0' }, t('One-click browser automations using your authenticated Personal container tabs (no API keys required).'))),
        el(
          'div.chip-row',
          {},
          onImportPack ? el('button.btn.btn-sm', { type: 'button', onclick: onImportPack }, icon('upload', 12), t('Import FlowPack')) : null,
          onExportPack ? el('button.btn.btn-sm.btn-primary', { type: 'button', onclick: onExportPack }, icon('download', 12), t('Export FlowPack')) : null
        )
      ),
      el('div.grid-cards', {}, ...cards),
      packRows.length
        ? el('div.card', {}, el('div.card-head', {}, el('h2.card-title', {}, t('Saved FlowPacks (packs/)')), el('span.pill', {}, String(packRows.length))), el('div.row-list', {}, ...packRows))
        : null,
      el(
        'div.card',
        {},
        el(
          'div.card-head',
          {},
          el('h2.card-title', {}, t('Site Actions Library')),
          el(
            'div.chip-row',
            {},
            onRecordAction
              ? el('button.btn.btn-sm', { type: 'button', onclick: onRecordAction }, el('span.rec-dot'), t('Record Site Action'))
              : null,
            el('button.btn.btn-sm.btn-primary', { type: 'button', onclick: openCustomActionModal }, icon('plus', 12), t('+ Custom Site Action'))
          )
        ),
        el('div.row-list', {}, ...actionRows)
      )
    )
  );
}

export function renderSecretsView(container, { secrets = [], onSaveSecret, onDeleteSecret }) {
  const nameInp = el('input.input.mono', { placeholder: 'STRIPE_API_KEY' });
  const valInp = el('input.input.mono', { type: 'password', placeholder: 'sk_live_...' });

  const addRow = el(
    'div.card',
    {},
    el('div.card-head', {}, el('h2.card-title', {}, t('Encrypted Secrets Vault')), el('span.pill.pill-good', {}, t('AES-256-GCM Local'))),
    el('p.muted', { style: 'margin:0 0 10px' }, t('Store API keys or passwords locally encrypted. Use {{secrets.NAME}} in HTTP/Site steps; values are never exposed to transform JS or exports.')),
    el(
      'div.field-row',
      { style: 'align-items:end' },
      el('div.field', {}, el('label.field-label', {}, t('Secret Name (UPPER_SNAKE)')), nameInp),
      el('div.field', {}, el('label.field-label', {}, t('Secret Value')), valInp)
    ),
    el(
      'div',
      { style: 'margin-top:10px' },
      el('button.btn.btn-sm.btn-primary', {
        type: 'button',
        onclick: async () => {
          const name = nameInp.value.trim().toUpperCase();
          const val = valInp.value;
          if (!name || !val) {
            toast(t('Secret name and value are required'), { tone: 'warn' });
            return;
          }
          await onSaveSecret(name, val);
          nameInp.value = '';
          valInp.value = '';
        },
      }, icon('shield', 13), t('Save Secret'))
    )
  );

  const listRows = (secrets || []).map((s) =>
    el(
      'div.list-row',
      {},
      el(
        'div',
        { style: 'min-width:0;flex:1' },
        el(
          'div.chip-row',
          {},
          el('span.mono', { style: 'font-weight:700' }, s.name),
          el('span.pill', {}, '••••••••'),
          el('span.pill.pill-accent', {}, s.scheme || 'v1-aesgcm')
        ),
        s.updatedAt ? el('div.muted', { style: 'font-size:11px;margin-top:2px' }, new Date(s.updatedAt).toLocaleString()) : null
      ),
      el(
        'div.chip-row',
        {},
        el('button.btn.btn-sm', {
          type: 'button',
          onclick: () => {
            const tok = `{{secrets.${s.name}}}`;
            navigator.clipboard?.writeText?.(tok).catch(() => {});
            toast(t('Copied {token}', { token: tok }));
          },
        }, t('Copy {{var}}')),
        el('button.btn.btn-sm.btn-danger', { type: 'button', onclick: () => onDeleteSecret(s.name) }, icon('trash', 12))
      )
    )
  );

  container.replaceChildren(
    el(
      'div.view-scroll',
      {},
      addRow,
      el(
        'div.card',
        {},
        el('div.card-head', {}, el('h2.card-title', {}, t('Stored Secrets')), el('span.pill', {}, String((secrets || []).length))),
        listRows.length ? el('div.row-list', {}, ...listRows) : el('p.muted', {}, t('No secrets stored yet.'))
      )
    )
  );
}
