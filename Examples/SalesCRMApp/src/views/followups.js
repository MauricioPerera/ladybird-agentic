/*
 * Copyright (c) 2026, Ladybird contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { byDay, cellRange, inRange, monthCells, periodAnchor, shiftPeriod, weekCells } from '../lib/calendar-grid.js';
import { isOpen } from '../lib/forecast.js';
import { t, weekdaysShort } from '../lib/i18n.js';
import { repeatLabel } from '../lib/repeat.js';
import { addDays, formatDate, formatMonth, todayISO } from '../lib/util.js';
import { duePill, emptyState, pageHead } from '../ui/bits.js';
import { exportOpenCalendarIcs } from '../ui/calendar.js';
import { el, keepingFocus } from '../ui/dom.js';
import { openEmailFlow } from '../ui/email.js';
import { openTaskForm } from '../ui/forms.js';
import { icon } from '../ui/icons.js';
import { openModal } from '../ui/modal.js';
import { maybeNudgeNextStep } from '../ui/nudge.js';
import { toast } from '../ui/toast.js';

const PER_DAY = 3;

export function followupsView(root, { store, params }) {
  let anchor = periodAnchor(store.settings.followupView === 'month' ? 'month' : 'week');
  let openedDeepTask = false;

  const isWide = () => typeof window === 'undefined' || window.innerWidth > 720;

  const completeTask = async (tsk) => {
    const res = await store.toggleTask(tsk.id, !tsk.done);
    if (!res) return;
    if (res.spawned) {
      toast(t('Completed · next scheduled for {date}', { date: formatDate(res.spawned.due) }));
    } else if (res.task.done) {
      maybeNudgeNextStep(store, { dealId: res.task.dealId, contactId: res.task.contactId, openTaskForm: (x, o) => openTaskForm(store, x, o) });
    }
  };

  const openRescheduleSheet = (task) => {
    const inp = el('input.input', { type: 'date', value: task.due });
    const dlg = openModal({
      title: t('Move "{name}"', { name: task.title }),
      focusFirst: true,
      body: el('div.field', {}, el('label.field-label', {}, t('New date')), inp),
      foot: el(
        'div.btn-group',
        { style: 'display:flex;gap:8px' },
        el('button.btn', { type: 'button', onclick: () => dlg.close() }, t('Cancel')),
        el('button.btn.btn-primary', {
          type: 'button',
          onclick: async () => {
            if (!inp.value) return;
            const prevDue = task.due;
            await store.moveTask(task.id, inp.value);
            dlg.close();
            toast(t('Moved to {date}', { date: formatDate(inp.value) }), { action: t('Undo'), onAction: () => store.moveTask(task.id, prevDue) });
          },
        }, t('Move'))
      ),
    });
  };

  const openChipSheet = (task) => {
    const dlg = openModal({
      title: task.title,
      focusFirst: true,
      body: el(
        'div.row-list',
        {},
        el('button.list-row', { type: 'button', style: 'cursor:pointer', onclick: () => { dlg.close(); openRescheduleSheet(task); } }, el('span.row-title', {}, t('Move to another day'))),
        el('button.list-row', { type: 'button', style: 'cursor:pointer', onclick: () => { dlg.close(); openTaskForm(store, task); } }, el('span.row-title', {}, t('Edit follow-up'))),
        el('button.list-row', { type: 'button', style: 'cursor:pointer', onclick: () => { dlg.close(); completeTask(task); } }, el('span.row-title', {}, task.done ? t('Mark open') : t('Mark done')))
      ),
    });
  };

  const openDaySheet = (iso) => {
    const body = el('div.row-list');
    const fill = () => {
      const dayTasks = store.openTasks().filter((tsk) => tsk.due === iso);
      const dayDeals = store.deals.filter((d) => isOpen(d) && d.expectedClose === iso);
      if (dayTasks.length === 0 && dayDeals.length === 0) {
        body.replaceChildren(el('div.muted', {}, t('No follow-ups remaining for this day.')));
        return;
      }
      body.replaceChildren(
        ...dayTasks.map((tsk) =>
          el('button.list-row', { type: 'button', style: 'cursor:pointer', onclick: () => openChipSheet(tsk) }, el('span.row-title', {}, tsk.title), duePill(tsk.due, tsk.dueTime))
        ),
        ...dayDeals.map((d) =>
          el('a.list-row', { href: `#/pipeline?id=${d.id}` }, el('span.row-title', {}, t('Close: {name}', { name: d.name })), el('span.pill.pill-accent', {}, t('Deal')))
        )
      );
    };
    fill();
    const onChange = () => {
      if (body.isConnected) fill();
    };
    store.addEventListener('change', onChange);
    openModal({
      title: formatDate(iso, { year: 'always' }),
      body,
      onClose: () => store.removeEventListener('change', onChange),
    });
  };

  const renderTaskRow = (tsk) => {
    const today = todayISO();
    const contact = store.contact(tsk.contactId);
    const deal = store.deal(tsk.dealId);
    const tpl = tsk.templateId ? store.template(tsk.templateId) : null;
    const sub = [contact?.name, deal?.name].filter(Boolean).join(' · ');
    return el(
      'div.list-row',
      {},
      el(
        'div.row-main',
        {},
        el('input', { type: 'checkbox', 'aria-label': t('Toggle {name}', { name: tsk.title }), checked: tsk.done, onchange: () => completeTask(tsk) }),
        el(
          'div',
          { style: 'min-width:0;flex:1' },
          el('div.row-title.truncate', { style: tsk.done ? 'text-decoration:line-through;opacity:0.65' : '' }, tsk.title),
          sub ? el('div.row-meta.truncate', {}, sub) : null
        )
      ),
      el(
        'div.chip-row',
        {},
        tsk.repeat ? el('span.pill', {}, icon('repeat', 11), ' ', repeatLabel(tsk.repeat)) : null,
        tsk.done ? el('span.pill', {}, t('Done')) : duePill(tsk.due, tsk.dueTime),
        tpl && contact?.email && !tsk.done
          ? el('button.icon-btn', { type: 'button', title: t('Send template email'), 'aria-label': t('Send template email for {name}', { name: tsk.title }), onclick: () => openEmailFlow(store, { contact, deal, template: tpl }) }, icon('mail', 14))
          : null,
        !tsk.done && tsk.due <= today
          ? el('button.icon-btn', {
              type: 'button',
              title: t('Snooze +1 day'),
              'aria-label': t('Snooze {name} +1 day', { name: tsk.title }),
              onclick: async () => {
                const prev = tsk.due;
                const next = addDays(today, 1);
                await store.moveTask(tsk.id, next);
                toast(t('Snoozed to {date}', { date: formatDate(next) }), { action: t('Undo'), onAction: () => store.moveTask(tsk.id, prev) });
              },
            }, icon('clock', 14))
          : null,
        !tsk.done && store.services?.isAvailable()
          ? el('button.icon-btn', {
              type: 'button',
              title: t('Schedule in Google Calendar'),
              'aria-label': t('Schedule in Google Calendar'),
              onclick: () => store.services.scheduleFollowUpInCalendar(tsk).catch((err) => toast(err?.message || t('Could not open Calendar tab'), { tone: 'danger' })),
            }, icon('calendar', 14))
          : null,
        el('button.icon-btn', { type: 'button', title: t('Edit follow-up'), 'aria-label': t('Edit {name}', { name: tsk.title }), onclick: () => openTaskForm(store, tsk) }, icon('edit', 14))
      )
    );
  };

  const render = () => {
    const taskParam = params.get('task');
    if (taskParam && !openedDeepTask) {
      openedDeepTask = true;
      const target = store.task(taskParam);
      if (target) setTimeout(() => openTaskForm(store, target), 0);
      else toast(t('Follow-up not found'), { tone: 'warn' });
    }

    const today = todayISO();
    const savedMode = store.settings.followupView || 'list';
    const effectiveMode = !isWide() && savedMode === 'month' ? 'week' : savedMode;

    const modeButtons = [
      { id: 'list', label: 'List' },
      { id: 'week', label: 'Week_mode' },
      ...(isWide() ? [{ id: 'month', label: 'Month_mode' }] : []),
    ];

    const modeSwitch = el(
      'div.segmented',
      {},
      ...modeButtons.map((m) =>
        el(
          'button',
          {
            type: 'button',
            class: `seg-btn${effectiveMode === m.id ? ' active' : ''}`,
            onclick: () => {
              anchor = periodAnchor(m.id === 'month' ? 'month' : 'week', today);
              store.updateSettings({ followupView: m.id });
            },
          },
          t(m.label)
        )
      )
    );

    const nodes = [
      pageHead(t('Follow-ups'), {
        subtitle: t('{count} open reminder(s)', { count: store.openTasks().length }),
        actions: [
          modeSwitch,
          el('button.btn.btn-sm', { type: 'button', onclick: () => exportOpenCalendarIcs(store) }, '.ics'),
          el('button.btn.btn-primary', { type: 'button', onclick: () => openTaskForm(store) }, t('+ New follow-up')),
        ],
      }),
    ];

    if (effectiveMode === 'list') {
      const open = store.openTasks();
      const overdue = open.filter((tsk) => tsk.due < today);
      const dueToday = open.filter((tsk) => tsk.due === today);
      const upcoming = open.filter((tsk) => tsk.due > today);
      const done = store.tasks.filter((tsk) => tsk.done).sort((a, b) => (b.doneAt || 0) - (a.doneAt || 0));

      if (open.length === 0) {
        nodes.push(emptyState('checkSquare', t('All caught up'), t('No open follow-ups due right now.'), [el('button.btn.btn-primary', { type: 'button', onclick: () => openTaskForm(store) }, t('+ Schedule follow-up'))]));
      } else {
        const groupCard = (title, list, critical = false) =>
          list.length
            ? el(
                'div.card',
                { style: 'margin-bottom:12px' },
                el('div.card-head', {}, el('h2.card-title', {}, title), el('span', { class: `pill${critical ? ' pill-critical' : ''}` }, String(list.length))),
                el('div.row-list', {}, ...list.map(renderTaskRow))
              )
            : null;
        nodes.push(groupCard(t('Overdue'), overdue, true), groupCard(t('Today'), dueToday), groupCard(t('Upcoming'), upcoming));
      }

      if (done.length) {
        nodes.push(
          el(
            'div.card',
            {},
            el('div.card-head', {}, el('h2.card-title', {}, t('Done')), el('span.pill', {}, String(done.length))),
            el('div.row-list', {}, ...done.slice(0, 10).map(renderTaskRow))
          )
        );
      }
    } else {
      if (effectiveMode === 'month' && anchor.length > 7) anchor = periodAnchor('month', today);
      if (effectiveMode === 'week' && anchor.length === 7) anchor = periodAnchor('week', today);

      const cells = effectiveMode === 'month' ? monthCells(anchor, today) : weekCells(anchor, today);
      const range = cellRange(cells);
      const openInRange = inRange(store.openTasks(), range, (tsk) => tsk.due);
      const closesInRange = inRange(
        store.deals.filter((d) => isOpen(d) && d.expectedClose),
        range,
        (d) => d.expectedClose
      );
      const taskMap = byDay(openInRange, (tsk) => tsk.due);
      const dealMap = byDay(closesInRange, (d) => d.expectedClose);

      const titleLabel = effectiveMode === 'month' ? formatMonth(anchor) : t('Week of {date}', { date: formatDate(range.start) });
      nodes.push(
        el(
          'div.card',
          { style: 'margin-bottom:12px' },
          el(
            'div.card-head',
            {},
            el('h2.card-title', {}, titleLabel),
            el(
              'div.chip-row',
              {},
              el('button.btn.btn-sm', { type: 'button', 'aria-label': t('Previous period'), onclick: () => { anchor = shiftPeriod(effectiveMode, anchor, -1); render(); } }, '←'),
              el('button.btn.btn-sm', { type: 'button', onclick: () => { anchor = periodAnchor(effectiveMode, today); render(); } }, effectiveMode === 'month' ? t('This month_cal') : t('This week_cal')),
              el('button.btn.btn-sm', { type: 'button', 'aria-label': t('Next period'), onclick: () => { anchor = shiftPeriod(effectiveMode, anchor, 1); render(); } }, '→')
            )
          ),
          el(
            'div.calendar-grid',
            {},
            ...weekdaysShort().map((wd) => el('div.calendar-head-cell', {}, wd)),
            ...cells.map((cell) => {
              const dt = taskMap.get(cell.iso) || [];
              const dd = dealMap.get(cell.iso) || [];
              const combined = [...dt.map((tsk) => ({ kind: 'task', item: tsk })), ...dd.map((d) => ({ kind: 'deal', item: d }))];
              const shown = combined.slice(0, PER_DAY);
              const extra = combined.length - shown.length;

              const cellEl = el('div', {
                class: `calendar-cell${!cell.inMonth ? ' outside' : ''}${cell.today ? ' today' : ''}`,
                ondragover: (e) => {
                  e.preventDefault();
                  cellEl.classList.add('drop-target');
                },
                ondragleave: () => cellEl.classList.remove('drop-target'),
                ondrop: async (e) => {
                  e.preventDefault();
                  cellEl.classList.remove('drop-target');
                  const tid = e.dataTransfer.getData('text/task-id');
                  const task = store.task(tid);
                  if (task && task.due !== cell.iso) {
                    const prev = task.due;
                    await store.moveTask(task.id, cell.iso);
                    toast(t('Moved to {date}', { date: formatDate(cell.iso) }), { action: t('Undo'), onAction: () => store.moveTask(task.id, prev) });
                  }
                },
              });

              cellEl.appendChild(
                el('div', { style: 'display:flex;justify-content:space-between;font-size:11px;font-weight:600' }, String(cell.day), combined.length ? el('span.muted', {}, String(combined.length)) : null)
              );

              for (const entry of shown) {
                if (entry.kind === 'task') {
                  const tsk = entry.item;
                  cellEl.appendChild(
                    el(
                      'button',
                      {
                        type: 'button',
                        draggable: 'true',
                        class: `cal-chip${tsk.due < today ? ' overdue' : ''}`,
                        ondragstart: (e) => e.dataTransfer.setData('text/task-id', tsk.id),
                        onclick: () => openChipSheet(tsk),
                      },
                      tsk.title
                    )
                  );
                } else {
                  const d = entry.item;
                  cellEl.appendChild(el('a', { href: `#/pipeline?id=${d.id}`, class: 'cal-chip deal' }, t('Close: {name}', { name: d.name })));
                }
              }
              if (extra > 0) {
                cellEl.appendChild(el('button.cal-chip', { type: 'button', onclick: () => openDaySheet(cell.iso) }, t('+{count} more', { count: extra })));
              }
              return cellEl;
            })
          )
        )
      );
    }

    root.replaceChildren(...nodes);
  };

  render();
  const onChange = () => keepingFocus(render);
  const onResize = () => keepingFocus(render);
  store.addEventListener('change', onChange);
  window.addEventListener('resize', onResize);
  return () => {
    store.removeEventListener('change', onChange);
    window.removeEventListener('resize', onResize);
  };
}
