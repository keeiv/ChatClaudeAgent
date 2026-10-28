// Calendar heatmap for host-recorded usage; civil dates avoid local DST offsets.
'use strict';
const CCAActivity = (() => {
  const MODES = { daily: '每日', weekly: '每週', cumulative: '累計' };
  const NOTES = { daily: '每格顯示當日用量', weekly: '每格顯示截至當日的近 7 日用量', cumulative: '每格顯示已保存紀錄截至當日的累計用量' };
  const amount = value => (value || 0).toLocaleString('zh-TW') + ' 個 token';

  function render(model, { h, mode = 'daily', onMode }) {
    if (!MODES[mode]) mode = 'daily';
    const days = model?.days || [];
    const card = h('section', { class: 'card activity-card', 'aria-label': 'Token 活動' });
    card.append(h('div', { class: 'activity-head' }, h('div', { class: 'card-title' }, 'Token 活動'),
      h('div', { class: 'activity-tabs', role: 'group', 'aria-label': '活動統計方式' },
        Object.entries(MODES).map(([key, label]) => h('button', { class: 'activity-tab' + (key === mode ? ' sel' : ''),
          type: 'button', 'aria-pressed': key === mode ? 'true' : 'false', onclick: () => onMode(key) }, label)))),
      h('div', { class: 'activity-summary' }, h('span', {}, '近一年 ', h('strong', {}, amount(model?.windowTotal))),
        h('span', {}, (model?.activeDays || 0) + ' 個活動日'), h('span', {}, '紀錄累計 ', amount(model?.total))));
    if (!days.length) { card.append(h('p', { class: 'muted' }, '尚無用量紀錄')); return card; }
    const offset = new Date(days[0].date + 'T00:00:00Z').getUTCDay();
    const columns = Math.ceil((offset + days.length) / 7);
    const max = Math.max(0, ...days.map(day => day[mode] || 0));
    const grid = h('div', { class: 'activity-grid', role: 'group', 'aria-label': '近一年 Token 活動日曆', style: { gridTemplateColumns: `repeat(${columns}, var(--activity-cell))` } });
    const months = h('div', { class: 'activity-months', style: { gridTemplateColumns: `repeat(${columns}, var(--activity-cell))` } });
    const tip = h('div', { class: 'tooltip activity-tip hidden', role: 'tooltip' });
    const detail = h('div', { class: 'activity-detail', role: 'status', 'aria-live': 'polite' }, NOTES[mode]);
    let lastMonth = '', lastColumn = -3;
    const cells = [];
    const describe = day => day.date + ' · ' + MODES[mode] + ' · ' +
      (mode === 'daily' && !day.recorded ? '尚無當日紀錄' : amount(day[mode]));
    const show = (day, cell) => {
      tip.textContent = describe(day);
      tip.classList.remove('hidden');
      const rect = cell.getBoundingClientRect(), bounds = card.getBoundingClientRect();
      tip.style.maxWidth = Math.max(100, Math.min(window.innerWidth, bounds.width) - 24) + 'px';
      const box = tip.getBoundingClientRect();
      tip.style.left = Math.max(8, bounds.left + 12, Math.min(bounds.right - box.width - 12,
        window.innerWidth - box.width - 8, rect.left - box.width / 2 + rect.width / 2)) + 'px';
      tip.style.top = Math.max(8, bounds.top + 12, rect.top - box.height - 8) + 'px';
    };
    days.forEach((day, index) => {
      const column = Math.floor((offset + index) / 7), row = (offset + index) % 7;
      const month = day.date.slice(0, 7);
      if (month !== lastMonth) {
        if (column === lastColumn) {
          months.lastChild.textContent = Number(day.date.slice(5, 7)) + '月';
        } else if (column - lastColumn >= 3) {
          months.append(h('span', { style: { gridColumn: column + 1 } }, Number(day.date.slice(5, 7)) + '月'));
          lastColumn = column;
        }
        lastMonth = month;
      }
      const value = day[mode] || 0;
      const level = value > 0 ? Math.min(4, 1 + Math.floor(value / Math.max(1, max) * 3.999)) : 0;
      const cell = h('button', { type: 'button', class: 'activity-cell level-' + level,
        tabIndex: index === days.length - 1 ? 0 : -1, 'data-date': day.date, 'aria-label': describe(day),
        style: { gridColumn: column + 1, gridRow: row + 1 },
        onmouseenter: () => show(day, cell), onmouseleave: () => tip.classList.add('hidden'),
        onfocus: () => {
          cells.forEach(button => { button.tabIndex = button === cell ? 0 : -1; });
          show(day, cell);
        }, onblur: () => tip.classList.add('hidden'), onclick: () => { detail.textContent = describe(day); },
        onkeydown: event => {
          const moves = { ArrowLeft: -7, ArrowRight: 7, ArrowUp: -1, ArrowDown: 1 };
          if (event.key in moves || event.key === 'Home' || event.key === 'End') {
            event.preventDefault();
            let next = event.key === 'Home' ? 0 : event.key === 'End' ? days.length - 1 : index + moves[event.key];
            cells[Math.max(0, Math.min(days.length - 1, next))].focus();
          } else if (event.key === 'Escape') tip.classList.add('hidden');
        } });
      cells.push(cell); grid.append(cell);
    });
    card.append(h('div', { class: 'activity-scroll' }, h('div', { class: 'activity-calendar' }, grid, months)),
      h('div', { class: 'activity-footer' }, detail, h('div', { class: 'activity-legend', 'aria-label': '顏色由少至多' },
        '少', [0, 1, 2, 3, 4].map(level => h('span', { class: 'activity-cell level-' + level })), '多')),
      h('p', { class: 'activity-note' }, '統計本機保存的輸入與輸出 token。' + NOTES[mode] + '；空白日期表示沒有當日紀錄。日期依記錄時的主機時區。'), tip);
    return card;
  }
  return { render };
})();
if (typeof window !== 'undefined') window.CCAActivity = CCAActivity;
