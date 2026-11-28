/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { CRON_PRESETS, nextCronRuns, parseCronExpression } from '../lib/cron.js';
import { t } from '../lib/i18n.js';
import { findSiteAction, getAllSiteActions } from '../lib/site-packs.js';
import {
  ACTION_META_MAP,
  CONDITION_FALSE_MODES,
  CONDITION_OPERATORS,
  LOCATOR_KINDS,
  ON_ERROR_MODES,
  RUN_WHEN_MODES,
  STEP_ACTIONS,
  TRIGGER_TYPES,
  createBlankStep,
  createLocalNode,
  createSiteActionNode,
  extractTemplateVars,
  normalizeStep,
  sanitizeId,
} from '../lib/workflow.js';
import { el, icon, toast } from './dom.js';

function field(labelText, control) {
  return el('div.field', {}, el('label.field-label', {}, labelText), control);
}

function renderStepSpecificFields(step, targetWorkflow, services, customActions, mutateStep) {
  const serviceOpts = (services || []).map((s) => ({ id: s.id, label: `${s.label} (${s.id})` }));
  if (!serviceOpts.length) serviceOpts.push({ id: 'mail', label: 'Gmail (mail)' });
  const controls = [];

  if (step.kind === 'site_action') {
    const allActions = getAllSiteActions(customActions);
    const saDef = findSiteAction(step.siteActionId, customActions);

    controls.push(
      field(t('Site Action'), el('select.select', {
        onchange: (e) => mutateStep((st) => {
          const nextDef = findSiteAction(e.target.value, customActions);
          st.siteActionId = e.target.value;
          if (nextDef) {
            st.serviceId = nextDef.serviceId;
            st.label = nextDef.name;
            st.save_as = nextDef.defaultSaveAs || 'result';
            const nextParams = {};
            for (const f of nextDef.fields || []) nextParams[f.key] = f.default ?? '';
            st.params = nextParams;
          }
        }),
      }, ...allActions.map((a) => el('option', { value: a.id, selected: a.id === step.siteActionId }, a.name)))),
      field(t('Target Service'), el('select.select', {
        onchange: (e) => mutateStep((st) => { st.serviceId = e.target.value; }),
      }, ...serviceOpts.map((s) => el('option', { value: s.id, selected: s.id === (step.serviceId || saDef?.serviceId || targetWorkflow.serviceId) }, s.label))))
    );

    if (saDef?.fields?.length) {
      controls.push(el('div.field-label', { style: 'margin-top:4px' }, t('Parameters')));
      for (const pf of saDef.fields) {
        const val = step.params?.[pf.key] ?? pf.default ?? '';
        if (pf.type === 'textarea') {
          controls.push(field(pf.label, el('textarea.textarea', {
            value: String(val),
            placeholder: pf.placeholder || '',
            oninput: (e) => mutateStep((st) => {
              st.params = { ...(st.params || {}), [pf.key]: e.target.value };
            }, { silent: true }),
          })));
        } else {
          controls.push(field(pf.label, el('input.input', {
            type: pf.type === 'number' ? 'number' : 'text',
            value: String(val),
            placeholder: pf.placeholder || '',
            oninput: (e) => mutateStep((st) => {
              st.params = { ...(st.params || {}), [pf.key]: e.target.value };
            }, { renderCanvasOnly: true }),
          })));
        }
      }
    }
    controls.push(
      field(t('Save output as variable'), el('input.input.mono', {
        value: step.save_as || '',
        oninput: (e) => mutateStep((st) => { st.save_as = e.target.value; }, { renderCanvasOnly: true }),
      }))
    );
  } else if (step.kind === 'transform') {
    controls.push(
      field(t('JavaScript (input, outputs, vars)'), el('textarea.textarea', {
        style: 'min-height:135px',
        value: step.code || '',
        oninput: (e) => mutateStep((st) => { st.code = e.target.value; }, { silent: true }),
      })),
      field(t('Save output as variable'), el('input.input.mono', {
        value: step.save_as || '',
        oninput: (e) => mutateStep((st) => { st.save_as = e.target.value; }, { renderCanvasOnly: true }),
      }))
    );
  } else if (step.kind === 'condition') {
    controls.push(
      field(t('Variable / Dot path'), el('input.input.mono', {
        value: step.path || '',
        placeholder: 'calendar_notices.found',
        oninput: (e) => mutateStep((st) => { st.path = e.target.value; }, { renderCanvasOnly: true }),
      })),
      field(t('Operator'), el('select.select', {
        onchange: (e) => mutateStep((st) => { st.operator = e.target.value; }),
      }, ...CONDITION_OPERATORS.map((op) => el('option', { value: op.id, selected: op.id === (step.operator || 'not_empty') }, t(op.label))))),
      field(t('Expected value'), el('input.input', {
        value: step.expected ?? '',
        oninput: (e) => mutateStep((st) => { st.expected = e.target.value; }, { renderCanvasOnly: true }),
      })),
      field(t('On False'), el('select.select', {
        onchange: (e) => mutateStep((st) => { st.onFalse = e.target.value; }),
      }, ...CONDITION_FALSE_MODES.map((m) => el('option', { value: m.id, selected: m.id === (step.onFalse || 'stop') }, t(m.label)))))
    );
  } else if (step.kind === 'http') {
    controls.push(
      field(t('HTTP Method'), el('select.select', {
        onchange: (e) => mutateStep((st) => { st.method = e.target.value; }),
      }, ...['GET', 'POST', 'PUT', 'PATCH', 'DELETE'].map((m) => el('option', { value: m, selected: m === (step.method || 'GET') }, m)))),
      field(t('URL'), el('input.input.mono', {
        value: step.url || '',
        oninput: (e) => mutateStep((st) => { st.url = e.target.value; }, { renderCanvasOnly: true }),
      })),
      field(t('Headers (JSON)'), el('textarea.textarea', {
        style: 'min-height:64px',
        value: step.headersJson || '',
        oninput: (e) => mutateStep((st) => { st.headersJson = e.target.value; }, { silent: true }),
      })),
      field(t('Request Body'), el('textarea.textarea', {
        value: step.body || '',
        oninput: (e) => mutateStep((st) => { st.body = e.target.value; }, { silent: true }),
      })),
      field(t('Save output as variable'), el('input.input.mono', {
        value: step.save_as || '',
        oninput: (e) => mutateStep((st) => { st.save_as = e.target.value; }, { renderCanvasOnly: true }),
      }))
    );
  } else if (step.kind === 'save_file') {
    controls.push(
      field(t('Workspace path (exports/...)'), el('input.input.mono', {
        value: step.path || 'exports/output.json',
        oninput: (e) => mutateStep((st) => { st.path = e.target.value; }, { renderCanvasOnly: true }),
      })),
      field(t('File content (supports {{var}})'), el('textarea.textarea', {
        value: step.content || '',
        oninput: (e) => mutateStep((st) => { st.content = e.target.value; }, { silent: true }),
      })),
      field(t('Save output as variable'), el('input.input.mono', {
        value: step.save_as || '',
        oninput: (e) => mutateStep((st) => { st.save_as = e.target.value; }, { renderCanvasOnly: true }),
      }))
    );
  } else {
    const meta = ACTION_META_MAP[step.action] || {};
    controls.push(
      field(t('Target Service'), el('select.select', {
        onchange: (e) => mutateStep((st) => { st.serviceId = e.target.value || undefined; }),
      },
        el('option', { value: '', selected: !step.serviceId }, `${t('Default')} (${targetWorkflow.serviceId})`),
        ...serviceOpts.map((s) => el('option', { value: s.id, selected: s.id === step.serviceId }, s.label))
      )),
      field(t('Action'), el('select.select', {
        onchange: (e) => mutateStep((st) => {
          const nextAction = e.target.value;
          st.action = nextAction;
          if (['click', 'wait_for', 'assert_visible', 'extract', 'fill', 'assert_text'].includes(nextAction) && !st.locator) {
            st.locator = { kind: 'css', value: 'body', exact: false };
          }
          if (nextAction === 'eval' && !st.script) st.script = 'return { url: location.href, title: document.title };';
          if (nextAction === 'open' && !st.url) st.url = 'https://mail.google.com/';
          if (nextAction === 'sleep' && !st.ms) st.ms = 1000;
        }),
      }, ...STEP_ACTIONS.map((a) => el('option', { value: a.id, selected: a.id === step.action }, `${t(a.label)} (${a.id})`))))
    );

    if (meta.needsUrl || step.action === 'open') {
      controls.push(
        field(t('URL'), el('input.input', {
          value: step.url || '',
          oninput: (e) => mutateStep((st) => { st.url = e.target.value; }, { renderCanvasOnly: true }),
        })),
        el('label.chip-row', { style: 'cursor:pointer' },
          el('input', { type: 'checkbox', checked: step.wait_for_load !== false, onchange: (e) => mutateStep((st) => { st.wait_for_load = e.target.checked; }, { silent: true }) }),
          el('span', {}, t('Wait for page load'))
        )
      );
    }

    if (meta.needsLocator && step.locator) {
      const loc = step.locator;
      controls.push(
        field(t('Locator Kind'), el('select.select', {
          onchange: (e) => mutateStep((st) => { st.locator.kind = e.target.value; }),
        }, ...LOCATOR_KINDS.map((k) => el('option', { value: k.id, selected: k.id === loc.kind }, t(k.label)))))
      );
      if (loc.kind === 'role') {
        controls.push(
          el('div.field-row', {},
            field(t('ARIA Role'), el('input.input', {
              value: loc.role || '',
              placeholder: 'button',
              oninput: (e) => mutateStep((st) => { st.locator.role = e.target.value; }, { renderCanvasOnly: true }),
            })),
            field(t('Accessible Name'), el('input.input', {
              value: loc.name || '',
              oninput: (e) => mutateStep((st) => { st.locator.name = e.target.value; }, { renderCanvasOnly: true }),
            }))
          )
        );
      } else {
        controls.push(
          field(t('Locator Value'), el('input.input.mono', {
            value: loc.value || '',
            oninput: (e) => mutateStep((st) => { st.locator.value = e.target.value; }, { renderCanvasOnly: true }),
          }))
        );
      }
      controls.push(
        el('label.chip-row', { style: 'cursor:pointer' },
          el('input', { type: 'checkbox', checked: Boolean(loc.exact), onchange: (e) => mutateStep((st) => { st.locator.exact = e.target.checked; }, { silent: true }) }),
          el('span', {}, t('Exact match'))
        )
      );
    }

    if (step.action === 'fill') {
      controls.push(
        field(t('Text to type'), el('input.input', {
          value: step.text || '',
          oninput: (e) => mutateStep((st) => { st.text = e.target.value; }, { renderCanvasOnly: true }),
        })),
        el('label.chip-row', { style: 'cursor:pointer' },
          el('input', { type: 'checkbox', checked: step.clear !== false, onchange: (e) => mutateStep((st) => { st.clear = e.target.checked; }, { silent: true }) }),
          el('span', {}, t('Clear before typing'))
        )
      );
    }

    if (step.action === 'press') {
      controls.push(
        field(t('Key name'), el('input.input', {
          value: step.key || '',
          placeholder: 'Enter',
          oninput: (e) => mutateStep((st) => { st.key = e.target.value; }, { renderCanvasOnly: true }),
        }))
      );
    }

    if (step.action === 'assert_text' || step.action === 'assert_url') {
      controls.push(
        field(t('Contains text/URL'), el('input.input', {
          value: step.contains || '',
          oninput: (e) => mutateStep((st) => { st.contains = e.target.value; }, { silent: true }),
        })),
        field(t('Exact match text/URL'), el('input.input', {
          value: step.equals || '',
          oninput: (e) => mutateStep((st) => { st.equals = e.target.value; }, { silent: true }),
        }))
      );
    }

    if (step.action === 'sleep') {
      controls.push(
        field(t('Delay (ms)'), el('input.input', {
          type: 'number',
          min: 50,
          max: 30000,
          value: step.ms || 1000,
          oninput: (e) => mutateStep((st) => { st.ms = Number(e.target.value) || 1000; }, { renderCanvasOnly: true }),
        }))
      );
    }

    if (step.action === 'eval') {
      controls.push(
        field(t('Script (must end with return ...)'), el('textarea.textarea', {
          value: step.script || '',
          oninput: (e) => mutateStep((st) => { st.script = e.target.value; }, { silent: true }),
        }))
      );
    }

    if (meta.supportsSaveAs || step.save_as) {
      controls.push(
        field(t('Save output as variable'), el('input.input.mono', {
          value: step.save_as || '',
          placeholder: 'result',
          oninput: (e) => mutateStep((st) => { st.save_as = e.target.value; }, { renderCanvasOnly: true }),
        }))
      );
    }

    controls.push(
      field(t('Timeout (ms)'), el('input.input', {
        type: 'number',
        min: 500,
        max: 60000,
        value: step.timeout_ms || '',
        placeholder: String(targetWorkflow.default_timeout_ms || 12000),
        oninput: (e) => mutateStep((st) => { st.timeout_ms = e.target.value ? Number(e.target.value) : undefined; }, { silent: true }),
      }))
    );
  }
  return controls;
}

export function renderInspector(container, ctx) {
  const {
    workflow,
    selectedKey,
    services,
    customActions = [],
    secretNames = [],
    appOrigin = typeof window !== 'undefined' ? window.location.origin : 'http://127.0.0.1',
    latestRun = null,
    running = false,
    onMutate,
    onDeleteWorkflow,
    onTestWebhook,
    onRunFromStep,
    onPinStepOutput,
    onUnpinStepOutput,
  } = ctx;

  if (!workflow) {
    container.replaceChildren(el('p.muted', {}, t('No workflow selected')));
    return;
  }

  const vars = extractTemplateVars(workflow);
  const serviceOpts = (services || []).map((s) => ({ id: s.id, label: `${s.label} (${s.id})` }));
  if (!serviceOpts.length) serviceOpts.push({ id: 'mail', label: 'Gmail (mail)' });

  if (selectedKey === 'trigger' || !selectedKey?.startsWith('step:')) {
    const trig = workflow.trigger || {};
    const header = el(
      'div.card-head',
      { style: 'margin-bottom:4px' },
      el('span.pill.pill-warn', {}, t('Trigger Node')),
      el('button.btn.btn-sm.btn-danger', { type: 'button', onclick: () => onDeleteWorkflow(workflow) }, icon('trash', 12), t('Delete'))
    );

    const wfSection = el(
      'div.row-list',
      {},
      el('div.field-label', {}, t('Workflow Settings')),
      field(t('Name'), el('input.input', {
        value: workflow.name || '',
        oninput: (e) => onMutate((w) => {
          w.name = e.target.value;
          if (w._isNew) w.id = sanitizeId(e.target.value, 'flow-1');
        }, { renderCanvasOnly: true }),
      })),
      field(t('Workflow ID'), el('input.input', {
        value: workflow.id || '',
        onchange: (e) => onMutate((w) => { w.id = sanitizeId(e.target.value, 'flow-1'); }),
      })),
      field(t('Target Service'), el('select.select', {
        onchange: (e) => onMutate((w) => {
          w.serviceId = e.target.value;
          if (w.trigger) w.trigger.serviceId = e.target.value;
        }),
      }, ...serviceOpts.map((s) => el('option', { value: s.id, selected: s.id === workflow.serviceId }, s.label)))),
      field(t('Description'), el('input.input', {
        value: workflow.description || '',
        oninput: (e) => onMutate((w) => { w.description = e.target.value; }, { silent: true }),
      }))
    );

    const trigControls = [
      el('div.field-label', { style: 'margin-top:6px' }, t('Trigger Mode')),
      field(t('Trigger Mode'), el('select.select', {
        onchange: (e) => onMutate((w) => { w.trigger.type = e.target.value; }),
      }, ...TRIGGER_TYPES.map((tt) => el('option', { value: tt.id, selected: tt.id === (trig.type || 'manual') }, t(tt.label))))),
    ];

    if (trig.type && trig.type !== 'manual') {
      trigControls.push(
        el('label.chip-row', { style: 'cursor:pointer;margin-top:4px' },
          el('input', { type: 'checkbox', checked: trig.enabled !== false, onchange: (e) => onMutate((w) => { w.trigger.enabled = e.target.checked; }) }),
          el('span', {}, t('Enabled'))
        )
      );
      if (trig.type === 'interval') {
        trigControls.push(
          field(t('Interval (ms, min 5000)'), el('input.input', {
            type: 'number',
            min: 5000,
            step: 1000,
            value: trig.intervalMs || 60000,
            onchange: (e) => onMutate((w) => { w.trigger.intervalMs = Math.max(5000, Number(e.target.value) || 60000); }),
          }))
        );
      }
      if (trig.type === 'cron') {
        const cronExpr = trig.cronExpr || '0 9 * * 1-5';
        const parsedCron = parseCronExpression(cronExpr);
        const upcoming = parsedCron.ok ? nextCronRuns(cronExpr, 3) : [];
        trigControls.push(
          field(t('Cron Expression (min hr dom mon dow)'), el('input.input.mono', {
            value: cronExpr,
            placeholder: '0 9 * * 1-5',
            onchange: (e) => onMutate((w) => { w.trigger.cronExpr = e.target.value.trim(); }),
          })),
          el('div.var-chips', {}, ...CRON_PRESETS.map((p) =>
            el('button.var-chip-btn', {
              type: 'button',
              onclick: () => onMutate((w) => { w.trigger.cronExpr = p.id; }),
            }, t(p.label))
          )),
          parsedCron.ok
            ? el('div.muted.mono', { style: 'font-size:11px;margin-top:4px' }, `${t('Next runs:')} ${upcoming.map((d) => d.toLocaleString()).join(' · ')}`)
            : el('div', { style: 'color:var(--danger);font-size:11.5px;margin-top:4px' }, parsedCron.error)
        );
      }
      if (trig.type === 'webhook') {
        const trigId = trig.id || sanitizeId(`trig-${workflow.id || workflow.name}`, 'trig-flow');
        const hookUrl = `${appOrigin}/__ladybird_webhook/${trigId}`;
        const tok = trig.webhookSecret || '';
        const curlCmd = `curl -X POST "${hookUrl}" -H "Content-Type: application/json" -H "X-Webhook-Token: ${tok || '<SAVE_FIRST>'}" -d '{"event":"demo"}'`;
        trigControls.push(
          field(t('Webhook Endpoint URL (POST)'), el('input.input.mono', { value: hookUrl, readOnly: true })),
          field(t('X-Webhook-Token'), el('input.input.mono', {
            value: tok ? `••••••••${tok.slice(-6)}` : t('Click Save to generate webhook token'),
            readOnly: true,
          })),
          el(
            'div.chip-row',
            {},
            el('button.btn.btn-sm', {
              type: 'button',
              onclick: () => {
                navigator.clipboard?.writeText?.(curlCmd).catch(() => {});
                toast(t('cURL command copied'));
              },
            }, t('Copy cURL')),
            onTestWebhook
              ? el('button.btn.btn-sm.btn-primary', { type: 'button', onclick: () => onTestWebhook(workflow) }, icon('webhook', 12), t('Test Webhook'))
              : null
          )
        );
      }
      if (trig.type === 'network' || trig.type === 'navigation') {
        trigControls.push(
          field(t('URL contains'), el('input.input', {
            value: trig.urlContains || '',
            placeholder: '/sync/u/0/i/s',
            oninput: (e) => onMutate((w) => { w.trigger.urlContains = e.target.value; }, { silent: true }),
          }))
        );
      }
      if (trig.type === 'network') {
        trigControls.push(
          el('div.field-row', {},
            field(t('HTTP Method'), el('input.input', {
              value: trig.method || '',
              placeholder: 'POST',
              oninput: (e) => onMutate((w) => { w.trigger.method = e.target.value.toUpperCase(); }, { silent: true }),
            })),
            field(t('Status Code'), el('input.input', {
              type: 'number',
              value: trig.statusCode ?? '',
              placeholder: '200',
              oninput: (e) => onMutate((w) => { w.trigger.statusCode = e.target.value; }, { silent: true }),
            }))
          )
        );
      }
      if (trig.type === 'notification' || trig.type === 'navigation') {
        trigControls.push(
          field(t('Title contains'), el('input.input', {
            value: trig.titleContains || '',
            oninput: (e) => onMutate((w) => { w.trigger.titleContains = e.target.value; }, { silent: true }),
          }))
        );
      }
      if (trig.type === 'notification') {
        trigControls.push(
          field(t('Body contains'), el('input.input', {
            value: trig.bodyContains || '',
            oninput: (e) => onMutate((w) => { w.trigger.bodyContains = e.target.value; }, { silent: true }),
          }))
        );
      }
      trigControls.push(
        field(t('Cooldown (ms)'), el('input.input', {
          type: 'number',
          min: 500,
          step: 500,
          value: trig.cooldownMs || 5000,
          onchange: (e) => onMutate((w) => { w.trigger.cooldownMs = Math.max(500, Number(e.target.value) || 5000); }),
        })),
        el('label.chip-row', { style: 'cursor:pointer' },
          el('input', { type: 'checkbox', checked: Boolean(trig.openIfClosed), onchange: (e) => onMutate((w) => { w.trigger.openIfClosed = e.target.checked; }, { silent: true }) }),
          el('span', {}, t('Open tab automatically if closed'))
        ),
        el('label.chip-row', { style: 'cursor:pointer' },
          el('input', { type: 'checkbox', checked: Boolean(trig.activate), onchange: (e) => onMutate((w) => { w.trigger.activate = e.target.checked; }, { silent: true }) }),
          el('span', {}, t('Focus tab while running'))
        ),
        el('label.chip-row', { style: 'cursor:pointer' },
          el('input', { type: 'checkbox', checked: Boolean(trig.closeOnFinish), onchange: (e) => onMutate((w) => { w.trigger.closeOnFinish = e.target.checked; }, { silent: true }) }),
          el('span', {}, t('Close tab after finishing'))
        )
      );
    }

    container.replaceChildren(
      header,
      wfSection,
      el('div.row-list', {}, ...trigControls),
      vars.length
        ? el('div', {}, el('div.field-label', {}, t('Detected template variables:')), el('div.chip-row', { style: 'margin-top:4px' }, ...vars.map((v) => el('span.pill.mono', {}, `{{${v}}}`))))
        : null
    );
    return;
  }

  const idx = Number(selectedKey.replace('step:', '')) || 0;
  const rawStep = workflow.steps?.[idx];
  if (!rawStep) {
    container.replaceChildren();
    return;
  }
  const step = normalizeStep(rawStep);

  const stepHeader = el(
    'div.card-head',
    { style: 'margin-bottom:4px' },
    el('span.pill.pill-accent', {}, t('Step {n}', { n: idx + 1 })),
    el(
      'div.chip-row',
      {},
      el('button.btn.btn-sm', {
        type: 'button',
        disabled: idx === 0,
        onclick: () => onMutate((w) => {
          const [s] = w.steps.splice(idx, 1);
          w.steps.splice(idx - 1, 0, s);
        }, { selectKey: `step:${idx - 1}` }),
      }, t('Move Up')),
      el('button.btn.btn-sm', {
        type: 'button',
        disabled: idx >= workflow.steps.length - 1,
        onclick: () => onMutate((w) => {
          const [s] = w.steps.splice(idx, 1);
          w.steps.splice(idx + 1, 0, s);
        }, { selectKey: `step:${idx + 1}` }),
      }, t('Move Down')),
      el('button.btn.btn-sm', {
        type: 'button',
        onclick: () => onMutate((w) => {
          const copy = JSON.parse(JSON.stringify(w.steps[idx]));
          w.steps.splice(idx + 1, 0, copy);
        }, { selectKey: `step:${idx + 1}` }),
      }, t('Duplicate'))
    )
  );

  // Available upstream variable chips
  const upstreamVars = [];
  for (let i = 0; i < idx; i++) {
    const u = normalizeStep(workflow.steps[i]);
    if (u.save_as && !upstreamVars.includes(u.save_as)) upstreamVars.push(u.save_as);
  }
  upstreamVars.push('last');
  if (step.kind === 'foreach') {
    upstreamVars.push('item', 'index');
  }
  for (const sName of secretNames || []) {
    upstreamVars.push(`secrets.${sName}`);
  }
  const varChipBox = el(
    'div',
    {},
    el('div.field-label', {}, t('Available variables (click to copy)')),
    el(
      'div.var-chips',
      {},
      ...upstreamVars.map((v) =>
        el('button.var-chip-btn', {
          type: 'button',
          onclick: () => {
            const token = `{{${v}}}`;
            navigator.clipboard?.writeText?.(token).catch(() => {});
            toast(t('Copied {token}', { token }));
          },
        }, `{{${v}}}`)
      )
    )
  );

  const controls = [
    varChipBox,
    field(t('Label'), el('input.input', {
      value: step.label || '',
      oninput: (e) => onMutate((w) => { w.steps[idx].label = e.target.value; }, { renderCanvasOnly: true }),
    })),
  ];

  const mutateTopStep = (fn, opts) => onMutate((w) => fn(w.steps[idx]), opts);

  if (step.kind === 'foreach') {
    const inner = normalizeStep(step.bodyStep || createLocalNode('transform'));
    const allActions = getAllSiteActions(customActions);
    const defaultSiteActionId = allActions[0]?.id || 'gmail.read_inbox';
    const innerTypeVal = inner.kind === 'site_action' ? 'site_action' : inner.kind === 'step' ? 'step' : inner.kind;

    controls.push(
      field(t('Items Array Path'), el('input.input.mono', {
        value: step.itemsPath || '',
        placeholder: 'inbox.rows',
        oninput: (e) => mutateTopStep((st) => { st.itemsPath = e.target.value; }, { renderCanvasOnly: true }),
      })),
      field(t('Max items (1-50)'), el('input.input', {
        type: 'number',
        min: 1,
        max: 50,
        value: step.maxItems || 10,
        onchange: (e) => mutateTopStep((st) => { st.maxItems = Math.max(1, Math.min(50, Number(e.target.value) || 10)); }),
      })),
      field(t('Save output as variable'), el('input.input.mono', {
        value: step.save_as || 'loop_results',
        oninput: (e) => mutateTopStep((st) => { st.save_as = e.target.value; }, { renderCanvasOnly: true }),
      })),
      el('div.field-label', { style: 'margin-top:6px' }, t('Inner Step')),
      field(t('Inner Step Type'), el('select.select', {
        onchange: (e) => mutateTopStep((st) => {
          const tp = e.target.value;
          if (tp === 'site_action') st.bodyStep = createSiteActionNode(defaultSiteActionId, customActions);
          else if (tp === 'step') st.bodyStep = createBlankStep('eval', workflow.serviceId);
          else st.bodyStep = createLocalNode(tp);
        }),
      },
        el('option', { value: 'transform', selected: innerTypeVal === 'transform' }, t('Transform JS (Local)')),
        el('option', { value: 'site_action', selected: innerTypeVal === 'site_action' }, t('Site Action')),
        el('option', { value: 'http', selected: innerTypeVal === 'http' }, t('HTTP / Webhook Request')),
        el('option', { value: 'save_file', selected: innerTypeVal === 'save_file' }, t('Save Workspace File')),
        el('option', { value: 'step', selected: innerTypeVal === 'step' }, t('Browser Primitives'))
      ))
    );
    controls.push(...renderStepSpecificFields(inner, workflow, services, customActions, (fn, opts) => {
      onMutate((w) => {
        if (!w.steps[idx].bodyStep) w.steps[idx].bodyStep = createLocalNode('transform');
        fn(w.steps[idx].bodyStep);
      }, opts);
    }));
  } else {
    controls.push(...renderStepSpecificFields(step, workflow, services, customActions, mutateTopStep));
  }

  // Execution & Reliability section
  controls.push(
    el('div.field-label', { style: 'margin-top:8px;padding-top:8px;border-top:1px solid var(--border)' }, t('Execution & Reliability')),
    field(t('Run When'), el('select.select', {
      onchange: (e) => mutateTopStep((st) => { st.runWhen = e.target.value; }),
    }, ...RUN_WHEN_MODES.map((m) => el('option', { value: m.id, selected: m.id === (step.runWhen || 'always') }, t(m.label))))),
    el('div.field-row', {},
      field(t('Retries (0-3)'), el('input.input', {
        type: 'number',
        min: 0,
        max: 3,
        value: step.retryCount || 0,
        onchange: (e) => mutateTopStep((st) => { st.retryCount = Math.max(0, Math.min(3, Number(e.target.value) || 0)); }),
      })),
      field(t('Retry Delay (ms)'), el('input.input', {
        type: 'number',
        min: 250,
        max: 15000,
        step: 250,
        value: step.retryDelayMs || 1500,
        onchange: (e) => mutateTopStep((st) => { st.retryDelayMs = Math.max(250, Math.min(15000, Number(e.target.value) || 1500)); }),
      }))
    ),
    field(t('On Error'), el('select.select', {
      onchange: (e) => mutateTopStep((st) => { st.onError = e.target.value; }),
    }, ...ON_ERROR_MODES.map((m) => el('option', { value: m.id, selected: m.id === (step.onError || 'stop') }, t(m.label))))),
    el('label.chip-row', { style: 'cursor:pointer;margin-top:2px' },
      el('input', { type: 'checkbox', checked: Boolean(step.requireApproval), onchange: (e) => mutateTopStep((st) => { st.requireApproval = e.target.checked; }, { renderCanvasOnly: true }) }),
      el('span', {}, t('Require human approval before running'))
    )
  );

  // Pin Data & Run From Step debug controls
  const lastStepOut = (latestRun?.steps || []).find((s) => s.index === idx)?.output;
  const isPinned = workflow.pinnedOutputs && Object.prototype.hasOwnProperty.call(workflow.pinnedOutputs, String(idx));
  controls.push(
    el('div.field-label', { style: 'margin-top:8px;padding-top:8px;border-top:1px solid var(--border)' }, t('Debug & Pin Data')),
    el('div.chip-row', {},
      el('button.btn.btn-sm', {
        type: 'button',
        disabled: running,
        onclick: () => onRunFromStep?.(idx),
      }, icon('play', 12), t('Run from here')),
      isPinned
        ? el('button.btn.btn-sm.btn-primary', {
            type: 'button',
            onclick: () => onUnpinStepOutput?.(idx),
          }, icon('pin', 12), t('Unpin data'))
        : el('button.btn.btn-sm', {
            type: 'button',
            disabled: lastStepOut === undefined || lastStepOut === null,
            onclick: () => onPinStepOutput?.(idx, lastStepOut),
          }, icon('pin', 12), t('Pin last output'))
    ),
    isPinned
      ? el('pre.output-body', { style: 'max-height:90px;background:var(--surface-2);border:1px solid var(--info);border-radius:8px;font-size:11px' }, JSON.stringify(workflow.pinnedOutputs[String(idx)], null, 2))
      : null
  );

  controls.push(
    el(
      'button.btn.btn-sm.btn-danger',
      {
        type: 'button',
        disabled: workflow.steps.length <= 1,
        onclick: () => onMutate((w) => {
          w.steps.splice(idx, 1);
          if (w.pinnedOutputs) delete w.pinnedOutputs[String(idx)];
        }, { selectKey: 'trigger' }),
      },
      icon('trash', 12),
      t('Remove Step')
    )
  );

  container.replaceChildren(stepHeader, el('div.row-list', {}, ...controls));
}
