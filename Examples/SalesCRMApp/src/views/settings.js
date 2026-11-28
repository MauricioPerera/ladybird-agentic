/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { listBackups, readBackup } from '../lib/backups.js';
import { formatBytes } from '../lib/files.js';
import { CUSTOM_KINDS, FIELD_TYPES, MAX_CUSTOM_FIELDS, blankField, sanitizeFieldOptions } from '../lib/fields.js';
import { DEFAULT_OPEN_STAGES, MAX_OPEN_STAGES, openStages, resolveStages } from '../lib/forecast.js';
import { getLanguagePreference, setLanguage, stageLabel, SUPPORTED_LANGUAGES, t } from '../lib/i18n.js';
import { MAX_PLAYBOOKS } from '../lib/playbooks.js';
import { notificationPermission, notificationSupported, requestNotificationPermission } from '../lib/reminders.js';
import { sampleDataset } from '../lib/sample.js';
import { CURRENCIES, timeAgo } from '../lib/util.js';
import { EXPORT_SETS } from '../lib/csv.js';
import { deletedToast, pageHead, toggleSwitch } from '../ui/bits.js';
import { downloadTextFile, exportOpenCalendarIcs } from '../ui/calendar.js';
import { exportSet, openCSVImport } from '../ui/csv.js';
import { el, keepingFocus } from '../ui/dom.js';
import { openTemplateForm } from '../ui/email.js';
import { openTaskForm } from '../ui/forms.js';
import { icon } from '../ui/icons.js';
import { confirmDialog, openModal } from '../ui/modal.js';
import { openDuplicatesReview } from '../ui/merge.js';
import { openPlaybookForm } from '../ui/playbooks.js';
import { applyTheme } from '../ui/theme.js';
import { toast } from '../ui/toast.js';

export function settingsView(root, { store }) {
  let cfKind = 'contacts';
  let backupsCache = [];
  let backupsError = false;
  let showAllTrash = false;

  listBackups().then((list) => {
    backupsCache = list;
    backupsError = false;
    if (root.isConnected) keepingFocus(render);
  }).catch(() => {
    backupsError = true;
    if (root.isConnected) keepingFocus(render);
  });

  const askReassign = (removedStage, remainingOpen) =>
    new Promise((resolve) => {
      let target = remainingOpen[0]?.id || 'lost';
      const sel = el(
        'select.select',
        { onchange: (e) => (target = e.target.value) },
        ...remainingOpen.map((s) => el('option', { value: s.id }, stageLabel(s, remainingOpen))),
        el('option', { value: 'won' }, t('Won')),
        el('option', { value: 'lost' }, t('Lost'))
      );
      let done = false;
      const dlg = openModal({
        title: t('Move deals from "{stage}"', { stage: stageLabel(removedStage) }),
        focusFirst: true,
        body: el('div.field', {}, el('label.field-label', {}, t('Reassign deals in this stage to:')), sel),
        foot: el(
          'div.btn-group',
          { style: 'display:flex;gap:8px' },
          el('button.btn', { type: 'button', onclick: () => { done = true; dlg.close(); resolve(null); } }, t('Cancel')),
          el('button.btn.btn-primary', { type: 'button', onclick: () => { done = true; dlg.close(); resolve(target); } }, t('Move & remove stage'))
        ),
        onClose: () => { if (!done) resolve(null); },
      });
    });

  const render = () => {
    const s = store.settings;
    const stages = resolveStages(s);
    const open = openStages(stages);
    const langPref = s.language || getLanguagePreference();

    // 1. Appearance & Accounts
    const appearanceCard = el(
      'div.card',
      {},
      el('div.card-head', {}, el('h2.card-title', {}, t('Appearance & Multi-Account'))),
      el(
        'div.form-grid',
        {},
        el(
          'div.field',
          {},
          el('label.field-label', {}, t('Theme')),
          el(
            'div.segmented',
            {},
            ...['light', 'system', 'dark'].map((mode) =>
              el(
                'button',
                {
                  type: 'button',
                  class: `seg-btn${s.theme === mode ? ' active' : ''}`,
                  onclick: () => {
                    applyTheme(mode);
                    store.updateSettings({ theme: mode });
                  },
                },
                t(mode[0].toUpperCase() + mode.slice(1))
              )
            )
          )
        ),
        el(
          'div.field',
          {},
          el('label.field-label', {}, t('Language')),
          el(
            'div.segmented',
            {},
            ...SUPPORTED_LANGUAGES.map((lang) =>
              el(
                'button',
                {
                  type: 'button',
                  class: `seg-btn${langPref === lang.id ? ' active' : ''}`,
                  'data-focus-key': `set-lang-${lang.id}`,
                  onclick: async () => {
                    setLanguage(lang.id);
                    await store.updateSettings({ language: lang.id });
                    toast(t('Language updated'));
                  },
                },
                lang.labelKey ? t(lang.labelKey) : lang.label
              )
            )
          )
        ),
        el(
          'div.field',
          {},
          el('label.field-label', {}, t('Currency')),
          el(
            'select.select',
            {
              'data-focus-key': 'set-currency',
              onchange: (e) => store.updateSettings({ currency: e.target.value }),
            },
            ...CURRENCIES.map((c) => el('option', { value: c, selected: s.currency === c }, c))
          )
        ),
        el(
          'div.field.full',
          {},
          el('label.field-label', {}, t('Active Account: {name}', { name: store.currentAccount?.label || store.currentAccount?.id })),
          el(
            'div.filter-line',
            {},
            ...store.accounts.map((a) =>
              el(
                'button.btn.btn-sm',
                {
                  type: 'button',
                  disabled: a.id === store.currentAccount?.id,
                  onclick: () => store.launchAccount(a.id).catch((err) => toast(err.message, { tone: 'danger' })),
                },
                a.id === store.currentAccount?.id ? t('{label} (active)', { label: a.label }) : t('Open {label}', { label: a.label })
              )
            ),
            el('button.btn.btn-sm', {
              type: 'button',
              onclick: () => {
                const inp = el('input.input', { type: 'text', placeholder: 'work / client-a' });
                const dlg = openModal({
                  title: t('Create isolated account'),
                  focusFirst: true,
                  body: el('div.field', {}, el('label.field-label', {}, t('Account slug')), inp),
                  foot: el('button.btn.btn-primary', {
                    type: 'button',
                    onclick: async () => {
                      const id = inp.value.trim().toLowerCase().replace(/[^a-z0-9_-]/g, '-').replace(/^-+|-+$/g, '');
                      if (!id) {
                        toast(t('Enter an account slug (letters, numbers, - or _)'), { tone: 'danger' });
                        inp.focus();
                        return;
                      }
                      try {
                        await store.createAccount(id, id);
                        dlg.close();
                        toast(t('Created account "{id}"', { id }));
                      } catch (err) {
                        toast(err?.message || t('Could not create account'), { tone: 'danger' });
                      }
                    },
                  }, t('Create')),
                });
              },
            }, t('+ New account'))
          )
        )
      )
    );

    // 2. Pipeline stages
    const stageRows = open.map((st, idx) =>
      el(
        'div.list-row',
        { style: 'flex-wrap:wrap;gap:8px' },
        el('input.input.input-sm', {
          type: 'text',
          value: st.label,
          'aria-label': t('Stage {num} name', { num: idx + 1 }),
          style: 'flex:1;min-width:140px',
          onchange: (e) => {
            const next = open.map((x, i) => (i === idx ? { ...x, label: e.target.value.trim() || x.label } : x));
            store.setStages(next);
          },
        }),
        el('input.input.input-sm', {
          type: 'number',
          min: 0,
          max: 100,
          step: 1,
          'aria-label': t('{stage} win probability percent', { stage: stageLabel(st, open) }),
          style: 'width:70px',
          value: Math.round((s.stageProbabilities?.[st.id] ?? st.probability) * 100),
          onchange: (e) => {
            const p = Math.max(0, Math.min(100, Number(e.target.value) || 0)) / 100;
            const next = open.map((x, i) => (i === idx ? { ...x, probability: p } : x));
            store.setStages(next);
          },
        }),
        el('span.row-meta', {}, '%'),
        el('button.btn.btn-sm', {
          type: 'button',
          'aria-label': t('Move {stage} up', { stage: stageLabel(st, open) }),
          disabled: idx === 0,
          onclick: () => {
            const next = [...open];
            [next[idx - 1], next[idx]] = [next[idx], next[idx - 1]];
            store.setStages(next);
          },
        }, '↑'),
        el('button.btn.btn-sm', {
          type: 'button',
          'aria-label': t('Move {stage} down', { stage: stageLabel(st, open) }),
          disabled: idx === open.length - 1,
          onclick: () => {
            const next = [...open];
            [next[idx + 1], next[idx]] = [next[idx], next[idx + 1]];
            store.setStages(next);
          },
        }, '↓'),
        open.length > 1
          ? el(
              'button.icon-btn',
              {
                type: 'button',
                title: t('Remove stage'),
                'aria-label': t('Remove stage {stage}', { stage: stageLabel(st, open) }),
                onclick: async () => {
                  const remaining = open.filter((_, i) => i !== idx);
                  const holds = store.deals.some((d) => d.stage === st.id);
                  let reassign = {};
                  if (holds) {
                    const target = await askReassign(st, remaining);
                    if (!target) return;
                    reassign = { [st.id]: target };
                  }
                  await store.setStages(remaining, reassign);
                },
              },
              icon('trash', 14)
            )
          : null
      )
    );

    const stagesCard = el(
      'div.card',
      {},
      el(
        'div.card-head',
        {},
        el('h2.card-title', {}, t('Pipeline stages')),
        el(
          'div.chip-row',
          {},
          open.length < MAX_OPEN_STAGES
            ? el('button.btn.btn-sm', {
                type: 'button',
                onclick: () => {
                  const nextId = `stage-${Date.now().toString(36)}`;
                  store.setStages([...open, { id: nextId, label: t('New stage'), probability: 0.5 }]);
                },
              }, t('+ Add stage'))
            : null,
          el('button.btn.btn-sm.btn-ghost', {
            type: 'button',
            onclick: async () => {
              if (!(await confirmDialog(t('Reset pipeline stages to defaults? Any deals in removed custom stages will move to the first stage.'), { confirmLabel: t('Reset stages'), tone: 'danger' }))) return;
              await store.setStages(DEFAULT_OPEN_STAGES);
              toast(t('Reset pipeline stages to defaults'));
            },
          }, t('Reset'))
        )
      ),
      el('div.row-list', {}, ...stageRows, el('div.row-meta', { style: 'margin-top:4px' }, t('Won (100%) and Lost (0%) always remain pinned as terminal stages.')))
    );

    // 3. Custom fields
    const cfList = store.customFieldsFor(cfKind);
    const cfCard = el(
      'div.card',
      {},
      el(
        'div.card-head',
        {},
        el('h2.card-title', {}, t('Custom fields')),
        el(
          'div.chip-row',
          {},
          ...CUSTOM_KINDS.map((k) =>
            el('button', { type: 'button', class: `filter-chip${cfKind === k ? ' active' : ''}`, onclick: () => { cfKind = k; render(); } }, t(k[0].toUpperCase() + k.slice(1)))
          ),
          cfList.length < MAX_CUSTOM_FIELDS
            ? el('button.btn.btn-sm', {
                type: 'button',
                onclick: () => store.setCustomFields(cfKind, [...cfList, blankField(new Set(cfList.map((f) => f.id)))]),
              }, t('+ Add field'))
            : null
        )
      ),
      el(
        'div.row-list',
        {},
        ...(cfList.length
          ? cfList.map((f, idx) =>
              el(
                'div.list-row',
                { style: 'flex-wrap:wrap;gap:8px' },
                el('input.input.input-sm', {
                  type: 'text',
                  value: f.name,
                  'aria-label': t('Custom field {num} name', { num: idx + 1 }),
                  style: 'flex:1;min-width:120px',
                  onchange: (e) => {
                    const next = cfList.map((x, i) => (i === idx ? { ...x, name: e.target.value.trim() || x.name } : x));
                    store.setCustomFields(cfKind, next);
                  },
                }),
                el(
                  'select.select.select-sm',
                  {
                    'aria-label': t('{name} field type', { name: f.name }),
                    onchange: async (e) => {
                      const next = cfList.map((x, i) => (i === idx ? { ...x, type: e.target.value } : x));
                      try {
                        await store.setCustomFields(cfKind, next);
                      } catch (err) {
                        if (err.message === 'custom-field-type') {
                          toast(t('Cannot retype "{name}": {count} record(s) contain incompatible values', { name: f.name, count: err.blocked.length }), { tone: 'warn', duration: 6000 });
                        }
                        render();
                      }
                    },
                  },
                  ...FIELD_TYPES.map((ft) => el('option', { value: ft.id, selected: f.type === ft.id }, t(ft.label)))
                ),
                f.type === 'select'
                  ? el('input.input.input-sm', {
                      type: 'text',
                      'aria-label': t('{name} options', { name: f.name }),
                      placeholder: t('Option A, Option B'),
                      value: (f.options || []).join(', '),
                      onchange: (e) => {
                        const next = cfList.map((x, i) => (i === idx ? { ...x, options: sanitizeFieldOptions(e.target.value) } : x));
                        store.setCustomFields(cfKind, next);
                      },
                    })
                  : null,
                el(
                  'button.icon-btn',
                  {
                    type: 'button',
                    title: t('Delete field {name}', { name: f.name }),
                    'aria-label': t('Delete field {name}', { name: f.name }),
                    onclick: async () => {
                      if (!(await confirmDialog(t('Delete custom field "{name}"?', { name: f.name }), { confirmLabel: t('Delete'), tone: 'danger' }))) return;
                      await store.setCustomFields(cfKind, cfList.filter((_, i) => i !== idx));
                    },
                  },
                  icon('trash', 14)
                )
              )
            )
          : [el('div.muted', {}, t('No custom fields on {kind}.', { kind: t(cfKind[0].toUpperCase() + cfKind.slice(1)) }))])
      )
    );

    // 4. Reminders, Attention & Goals
    const perm = notificationPermission();
    const remindersCard = el(
      'div.card',
      {},
      el('div.card-head', {}, el('h2.card-title', {}, t('Reminders, Attention & Goals'))),
      el(
        'div.form-grid',
        {},
        el(
          'div.field',
          {},
          el('label.field-label', {}, t('Browser notifications ({perm})', { perm: t(perm) })),
          toggleSwitch(s.notifications, async (on) => {
            if (on && notificationSupported() && notificationPermission() !== 'granted') {
              await requestNotificationPermission();
            }
            await store.updateSettings({ notifications: on });
          })
        ),
        el(
          'div.field',
          {},
          el('label.field-label', {}, t('Next-step nudges')),
          toggleSwitch(s.nextStepNudges !== false, (on) => store.updateSettings({ nextStepNudges: on }))
        ),
        el('div.field', {}, el('label.field-label', {}, t('Stale deal threshold (days, 0=off)')), el('input.input', { type: 'number', min: 0, value: s.staleDays, onchange: (e) => store.updateSettings({ staleDays: Number(e.target.value) || 0 }) })),
        el('div.field', {}, el('label.field-label', {}, t('Going cold threshold (days, 0=off)')), el('input.input', { type: 'number', min: 0, value: s.coldDays, onchange: (e) => store.updateSettings({ coldDays: Number(e.target.value) || 0 }) })),
        el('div.field', {}, el('label.field-label', {}, t('Monthly revenue goal ({currency})', { currency: s.currency })), el('input.input', { type: 'number', min: 0, value: s.revenueGoal, onchange: (e) => store.updateSettings({ revenueGoal: Number(e.target.value) || 0 }) })),
        el('div.field', {}, el('label.field-label', {}, t('Weekly touches target (all)')), el('input.input', { type: 'number', min: 0, value: s.touchTargets?.all || 0, onchange: (e) => store.updateSettings({ touchTargets: { ...(s.touchTargets || {}), all: Number(e.target.value) || 0 } }) })),
        el('div.field.full', {}, el('button.btn.btn-sm', { type: 'button', onclick: () => exportOpenCalendarIcs(store) }, t('Export open follow-ups (.ics)')))
      )
    );

    // 5. Templates, Playbooks & Tags
    const templatesCard = el(
      'div.card',
      {},
      el('div.card-head', {}, el('h2.card-title', {}, t('Email templates & Playbooks')), el('div.chip-row', {}, el('button.btn.btn-sm', { type: 'button', onclick: () => openTemplateForm(store) }, t('+ Template')), (s.playbooks || []).length < MAX_PLAYBOOKS ? el('button.btn.btn-sm', { type: 'button', onclick: () => openPlaybookForm(store) }, t('+ Playbook')) : null)),
      el(
        'div.row-list',
        {},
        ...store.templates.map((tpl) =>
          el(
            'div.list-row',
            {},
            el('span.row-title', {}, tpl.name),
            el(
              'div.chip-row',
              {},
              el('button.btn.btn-sm', { type: 'button', onclick: () => openTemplateForm(store, tpl) }, t('Edit')),
              el(
                'button.icon-btn',
                {
                  type: 'button',
                  title: t('Delete template {name}', { name: tpl.name }),
                  'aria-label': t('Delete template {name}', { name: tpl.name }),
                  onclick: async () => {
                    if (!(await confirmDialog(t('Delete email template "{name}"?', { name: tpl.name }), { confirmLabel: t('Delete'), tone: 'danger' }))) return;
                    const entry = await store.deleteTemplate(tpl.id);
                    deletedToast(store, entry, t('Template'));
                  },
                },
                icon('trash', 14)
              )
            )
          )
        ),
        ...(s.playbooks || []).map((pb) =>
          el(
            'div.list-row',
            {},
            el('div', {}, el('span.row-title', {}, pb.name), el('span.row-meta', {}, ` · ${t('{count} steps', { count: pb.steps.length })}${pb.trigger ? ` · ${stageLabel(pb.trigger, stages)}` : ''}`)),
            el(
              'div.chip-row',
              {},
              el('button.btn.btn-sm', { type: 'button', onclick: () => openPlaybookForm(store, pb) }, t('Edit')),
              el(
                'button.icon-btn',
                {
                  type: 'button',
                  title: t('Delete playbook {name}', { name: pb.name }),
                  'aria-label': t('Delete playbook {name}', { name: pb.name }),
                  onclick: async () => {
                    if (!(await confirmDialog(t('Delete playbook "{name}"?', { name: pb.name }), { confirmLabel: t('Delete'), tone: 'danger' }))) return;
                    await store.setPlaybooks((s.playbooks || []).filter((x) => x.id !== pb.id));
                  },
                },
                icon('trash', 14)
              )
            )
          )
        ),
        store.allTags().length
          ? el(
              'div.chip-row',
              { style: 'margin-top:8px' },
              ...store.allTags().map((tag) =>
                el('button.pill', {
                  type: 'button',
                  title: t('Click to rename or remove tag'),
                  onclick: () => {
                    const inp = el('input.input', { type: 'text', value: tag });
                    const dlg = openModal({
                      title: `Tag #${tag}`,
                      focusFirst: true,
                      body: inp,
                      foot: el(
                        'div.chip-row',
                        {},
                        el('button.btn.btn-danger', {
                          type: 'button',
                          onclick: async () => {
                            if (!(await confirmDialog(t('Remove tag "#{tag}" from all records?', { tag }), { confirmLabel: t('Delete tag'), tone: 'danger' }))) return;
                            await store.deleteTag(tag);
                            dlg.close();
                          },
                        }, t('Delete tag')),
                        el('button.btn.btn-primary', {
                          type: 'button',
                          onclick: async () => {
                            if (!inp.value.trim()) {
                              toast(t('Enter a tag name'), { tone: 'danger' });
                              inp.focus();
                              return;
                            }
                            await store.renameTag(tag, inp.value);
                            dlg.close();
                          },
                        }, t('Rename'))
                      ),
                    });
                  },
                }, `#${tag}`)
              )
            )
          : null
      )
    );

    // 6. Data, Local Backups & Trash
    const jsonImportInput = el('input', { type: 'file', accept: '.json,application/json', hidden: true });
    jsonImportInput.addEventListener('change', async () => {
      const f = jsonImportInput.files?.[0];
      if (!f) return;
      try {
        const payload = JSON.parse(await f.text());
        if (store.hasData()) {
          const ok = await confirmDialog('Replace all current CRM data with this JSON export? A safety snapshot will be saved first.', {
            confirmLabel: 'Replace & Import',
            tone: 'danger',
          });
          if (!ok) {
            jsonImportInput.value = '';
            return;
          }
        }
        await store.importAll(payload, { reason: 'json' });
        toast('Imported CRM JSON');
      } catch (err) {
        toast(err.message || 'Invalid JSON export', { tone: 'danger' });
      }
      jsonImportInput.value = '';
    });

    const trashShown = showAllTrash ? store.trash : store.trash.slice(0, 15);
    const dataCard = el(
      'div.card',
      {},
      el(
        'div.card-head',
        {},
        el('h2.card-title', {}, t('Data, Backups & Trash')),
        store.backupMeta
          ? el('button.btn.btn-sm', {
              type: 'button',
              onclick: async () => {
                if (!(await confirmDialog(t('Restore safety snapshot ({when}) and replace current CRM data?', { when: timeAgo(store.backupMeta.at) }), { confirmLabel: t('Restore snapshot'), tone: 'danger' }))) return;
                try {
                  if (await store.restoreBackup()) toast(t('Restored safety snapshot'));
                  else toast(t('Safety snapshot is unavailable'), { tone: 'warn' });
                } catch (err) {
                  toast(err?.message || t('Could not restore safety snapshot'), { tone: 'danger' });
                }
              },
            }, t('Restore snapshot ({when})', { when: timeAgo(store.backupMeta.at) }))
          : null
      ),
      el(
        'div.chip-row',
        { style: 'margin-bottom:10px' },
        el('button.btn.btn-sm', { type: 'button', onclick: () => openDuplicatesReview(store) }, t('Find duplicates')),
        el('button.btn.btn-sm', { type: 'button', onclick: () => openCSVImport(store) }, t('Import CSV')),
        ...EXPORT_SETS.map((es) => el('button.btn.btn-sm', { type: 'button', onclick: () => exportSet(es.id, store) }, t('CSV: {label}', { label: t(es.label) }))),
        el('button.btn.btn-sm', { type: 'button', onclick: () => downloadTextFile(`sales-crm-${new Date().toISOString().slice(0, 10)}.json`, JSON.stringify(store.exportAll(), null, 2), 'application/json') }, t('Export JSON')),
        el('button.btn.btn-sm', { type: 'button', onclick: () => jsonImportInput.click() }, t('Import JSON')),
        jsonImportInput,
        el('button.btn.btn-sm', {
          type: 'button',
          onclick: async () => {
            if (store.hasData()) {
              const ok = await confirmDialog(t('Replace your current CRM data with the sample dataset? A safety snapshot will be saved first.'), {
                confirmLabel: t('Load sample data'),
                tone: 'danger',
              });
              if (!ok) return;
            }
            try {
              await store.importAll(sampleDataset(store), { reason: 'sample' });
              toast(t('Loaded sample CRM dataset'));
            } catch (err) {
              toast(err?.message || t('Could not load sample data'), { tone: 'danger' });
            }
          },
        }, t('Load sample data')),
        el('button.btn.btn-sm.btn-danger', {
          type: 'button',
          onclick: async () => {
            if (!(await confirmDialog(t('Erase all companies, leads, deals, activities, follow-ups, and trash?'), { confirmLabel: t('Erase all'), tone: 'danger' }))) return;
            await store.eraseAll();
            toast(t('Erased CRM data'));
          },
        }, t('Erase all'))
      ),
      el(
        'div.panel-row',
        { style: 'display:flex;justify-content:space-between;align-items:center;margin:10px 0 6px' },
        el('span.field-label', {}, backupsError ? t('Rolling FS backups (unavailable)') : t('Rolling FS backups ({count}/8)', { count: backupsCache.length })),
        el(
          'div.chip-row',
          {},
          el('input.input.input-sm', { type: 'number', min: 0, 'aria-label': t('Auto-backup interval in days'), style: 'width:60px', value: s.autoBackupDays, title: t('Auto-backup interval (days, 0=off)'), onchange: (e) => store.updateSettings({ autoBackupDays: Number(e.target.value) || 0 }) }),
          el('button.btn.btn-sm', {
            type: 'button',
            onclick: async () => {
              try {
                const res = await store.createFileBackup();
                backupsCache = await listBackups();
                backupsError = false;
                toast(t('Backed up ({size})', { size: formatBytes(res.bytesWritten) }));
                render();
              } catch (err) {
                toast(err?.message || t('Backup failed'), { tone: 'danger' });
              }
            },
          }, t('Back up now'))
        )
      ),
      el(
        'div.row-list',
        {},
        ...backupsCache.map((b) =>
          el(
            'div.list-row',
            {},
            el('span.row-title', {}, b.name),
            el(
              'div.chip-row',
              {},
              el('span.row-meta', {}, formatBytes(b.size)),
              el('button.btn.btn-sm', {
                type: 'button',
                onclick: async () => {
                  try {
                    downloadTextFile(b.name, JSON.stringify(await readBackup(b.path), null, 2), 'application/json');
                  } catch (err) {
                    toast(err?.message || t('Could not read backup'), { tone: 'danger' });
                  }
                },
              }, t('Download')),
              el('button.btn.btn-sm', {
                type: 'button',
                onclick: async () => {
                  if (!(await confirmDialog(t('Restore "{name}"?', { name: b.name }), { confirmLabel: t('Restore'), tone: 'danger' }))) return;
                  try {
                    await store.restoreFileBackup(b.path);
                    toast(t('Backup restored'));
                  } catch (err) {
                    toast(err?.message || t('Could not restore backup'), { tone: 'danger' });
                  }
                },
              }, t('Restore'))
            )
          )
        )
      ),
      el(
        'div.panel-row',
        { style: 'display:flex;justify-content:space-between;align-items:center;margin:12px 0 6px' },
        el('span.field-label', {}, t('Recently deleted ({count}, kept 30d)', { count: store.trash.length })),
        store.trash.length
          ? el('button.btn.btn-sm.btn-danger', { type: 'button', onclick: async () => { if (await confirmDialog(t('Permanently empty trash?'), { confirmLabel: t('Empty trash'), tone: 'danger' })) await store.emptyTrash(); } }, t('Empty trash'))
          : null
      ),
      el(
        'div.row-list',
        {},
        ...(trashShown.length
          ? trashShown.map((e) =>
              el('div.list-row', {}, el('span.row-title', {}, `${e.name} (${t(e.kind === 'activity' ? 'Activity_rec' : e.kind)})`), el('div.chip-row', {}, el('span.row-meta', {}, timeAgo(e.at)), el('button.btn.btn-sm', { type: 'button', onclick: () => store.restoreTrash(e.id) }, t('Restore'))))
            )
          : [el('div.muted', {}, t('Trash is empty.'))]),
        store.trash.length > 15
          ? el(
              'button.btn.btn-sm.btn-ghost',
              { type: 'button', onclick: () => { showAllTrash = !showAllTrash; render(); } },
              showAllTrash ? t('Show fewer') : t('Show all ({count})', { count: store.trash.length })
            )
          : null
      )
    );

    // 7. Connected Web Services (Calendar, Gmail, Drive / Docs)
    const svcHub = store.services;
    const svcStates = svcHub?.isAvailable() ? svcHub.getStates() : [];
    const svcEvents = svcHub?.isAvailable() ? svcHub.getEvents() : [];
    const servicesCard = svcHub?.isAvailable()
      ? el(
          'div.card',
          {},
          el(
            'div.card-head',
            {},
            el('h2.card-title', {}, t('Connected Services')),
            el(
              'div.chip-row',
              {},
              el('button.btn.btn-sm', { type: 'button', onclick: () => svcHub.refreshStates() }, t('Refresh')),
              el(
                'button.btn.btn-sm.btn-primary',
                {
                  type: 'button',
                  disabled: svcHub.isScanning(),
                  onclick: async () => {
                    const res = await svcHub.scanMailInbox();
                    if (!res.scanned) toast(res.error || t('Open the Gmail service tab first.'), { tone: 'warn' });
                    else toast(t('Scanned {rows} emails · {count} calendar/CRM notices', { rows: res.totalRows || 0, count: res.added || 0 }));
                  },
                },
                svcHub.isScanning() ? t('Syncing…') : t('Sync inbox')
              )
            )
          ),
          el('p.muted', { style: 'margin:0 0 10px' }, t('Open authenticated Google Calendar, Gmail, or Drive / Docs tabs in this window and stream email/calendar updates into the CRM.')),
          el(
            'div.row-list',
            { style: 'margin-bottom:10px' },
            ...svcStates.map((st) =>
              el(
                'div.list-row',
                {},
                el(
                  'div',
                  { style: 'min-width:0;flex:1' },
                  el('div.row-title', {}, t(st.label)),
                  el('div.row-meta.truncate', {}, st.open ? (st.title || st.url || st.defaultUrl) : st.defaultUrl)
                ),
                el(
                  'div.chip-row',
                  {},
                  el('span', { class: `pill${st.open ? ' pill-good' : ''}` }, st.open ? t('Open') : t('Closed')),
                  el(
                    'button.btn.btn-sm',
                    {
                      type: 'button',
                      onclick: () => (st.open ? svcHub.focus(st.id) : svcHub.open(st.id)).catch((err) => toast(err?.message || t('Error'), { tone: 'danger' })),
                    },
                    st.open ? t('Focus') : t('Open')
                  )
                )
              )
            )
          ),
          el('div.field-label', { style: 'margin:8px 0 6px' }, t('Inbox & calendar events')),
          el(
            'div.row-list',
            {},
            ...(svcEvents.length
              ? svcEvents.slice(0, 12).map((ev) =>
                  el(
                    'div.list-row',
                    { style: 'flex-wrap:wrap;gap:8px' },
                    el(
                      'div',
                      { style: 'min-width:0;flex:1' },
                      el('div.row-title.truncate', {}, ev.eventTitle || ev.subject),
                      el(
                        'div.row-meta.truncate',
                        {},
                        [ev.kind, ev.due ? `${ev.due}${ev.dueTime ? ` ${ev.dueTime}` : ''}` : '', ev.contactName, ev.dealName].filter(Boolean).join(' · ')
                      )
                    ),
                    el(
                      'div.chip-row',
                      {},
                      !ev.handled && ev.due
                        ? el('button.btn.btn-sm.btn-primary', {
                            type: 'button',
                            onclick: async () => {
                              await svcHub.applyDateUpdate(ev.id);
                              toast(t('Updated follow-up date from notice'));
                            },
                          }, t('Apply date'))
                        : null,
                      !ev.handled
                        ? el('button.btn.btn-sm', {
                            type: 'button',
                            onclick: async () => {
                              await svcHub.logNoticeActivity(ev.id);
                              toast(t('Logged activity'));
                            },
                          }, t('Log it'))
                        : el('span.pill.pill-good', {}, t('Done')),
                      el('button.icon-btn', { type: 'button', 'aria-label': t('Remove'), onclick: () => svcHub.dismissEvent(ev.id) }, icon('x', 13))
                    )
                  )
                )
              : [el('div.muted', {}, t('No notifications or inbox calendar updates captured yet.'))])
          ),
          ...(svcHub.getLastScan()?.rows?.length
            ? [
                el('div.field-label', { style: 'margin:12px 0 6px' }, t('Recent Gmail inbox ({count})', { count: svcHub.getLastScan().totalRows || svcHub.getLastScan().rows.length })),
                el(
                  'div.row-list',
                  {},
                  ...svcHub.getLastScan().rows.slice(0, 8).map((row) =>
                    el(
                      'div.list-row',
                      { style: 'flex-wrap:wrap;gap:8px' },
                      el(
                        'div',
                        { style: 'min-width:0;flex:1' },
                        el('div.row-title.truncate', {}, row.subject || '(Sin asunto)'),
                        el('div.row-meta.truncate', {}, [row.from, row.dateTitle, row.body].filter(Boolean).join(' · '))
                      ),
                      el(
                        'div.chip-row',
                        {},
                        !row.logged
                          ? el('button.btn.btn-sm', {
                              type: 'button',
                              onclick: async () => {
                                await svcHub.logRawInboxRow(row);
                                toast(t('Logged activity'));
                              },
                            }, t('Log it'))
                          : el('span.pill.pill-good', {}, t('Done')),
                        el('button.btn.btn-sm', {
                          type: 'button',
                          onclick: () => openTaskForm(store, { title: row.subject || 'Follow up' }),
                        }, t('+ Follow-up'))
                      )
                    )
                  )
                ),
              ]
            : [])
        )
      : null;

    // 8. WebMCP Privacy & Audit Log
    const webmcpCard = el(
      'div.card',
      {},
      el('div.card-head', {}, el('h2.card-title', {}, t('WebMCP & Privacy Boundary')), el('span.pill.pill-accent', {}, `mcpAccess: ${store.systemInfo?.mcpAccess || t('unknown')}`)),
      el('p.muted', { style: 'margin:0 0 8px' }, t('Under --webmcp-only, DOM/screenshot/eval tools are blocked and only Tier A+B zero-PII aggregate/opaque-ID tools remain active.')),
      el(
        'div.row-list',
        {},
        ...(store.webmcpAudit.length
          ? store.webmcpAudit.slice(0, 15).map((w) => el('div.list-row', {}, el('span.mono', {}, `[${w.ts}] Tier ${w.tier} · ${w.tool}`), el('span', { class: `pill${w.ok ? ' pill-good' : ' pill-critical'}` }, w.ok ? 'OK' : w.note || t('Error'))))
          : [el('div.muted', {}, t('No WebMCP tool invocations in this account yet.'))])
      )
    );

    root.replaceChildren(
      pageHead(t('Settings'), { subtitle: t('Pipeline, fields, templates, backups, and privacy controls') }),
      el('div', { style: 'display:flex;flex-direction:column;gap:14px' }, ...[appearanceCard, stagesCard, cfCard, remindersCard, templatesCard, dataCard, servicesCard, webmcpCard].filter(Boolean))
    );
  };

  render();
  const onChange = () => keepingFocus(render);
  store.addEventListener('change', onChange);
  store.services?.addEventListener('change', onChange);
  return () => {
    store.removeEventListener('change', onChange);
    store.services?.removeEventListener('change', onChange);
  };
}
