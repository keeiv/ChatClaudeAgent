// Profile badges are local decorations. Ownership comes from the host, display changes are atomic.
window.CCABadges = (() => {
  let api, state = { catalog: [], owned: [], order: [], hidden: [], display: [] }, editor = null, serial = 0;
  const find = id => state.catalog.find(badge => badge.id === id);
  const art = (id, size = 24) => {
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    const master = size <= 16 ? 16 : 24;
    svg.setAttribute('viewBox', `0 0 ${master} ${master}`);
    svg.setAttribute('width', size);
    svg.setAttribute('height', size);
    svg.setAttribute('aria-hidden', 'true');
    svg.classList.add('profile-badge-art');
    // Only checked-in vector markup can enter this SVG; profile data never supplies markup.
    svg.innerHTML = window.CCABadgeArt?.[id]?.[master] || '';
    return svg;
  };

  function row(who, draft = state, onOpen = open) {
    if (who !== 'user' || !api) return null;
    const { h } = api;
    const shown = draft.display || draft.order.filter(id => !draft.hidden.includes(id)).slice(0, 6);
    const extra = state.owned.length - shown.length;
    return h('div', { class: 'profile-badges', role: 'group', 'aria-label': '個人徽章' },
      shown.map(id => h('button', { class: 'profile-badge', type: 'button', 'data-badge': id,
        'data-tip': find(id)?.name, 'aria-label': find(id)?.name || '徽章', onclick: () => onOpen(id) }, art(id, 20))),
      extra > 0 || !shown.length ? h('button', { class: 'badge-overflow', type: 'button',
        'aria-label': '查看全部徽章', onclick: () => onOpen() }, shown.length ? '+' + extra : '徽章') : null);
  }

  function open(initial) {
    if (!api || editor) return;
    const { h, put, modal, send } = api;
    const trigger = document.activeElement;
    const scope = ['.full-profile', '.user-popout', '.preview-card'].find(selector => trigger?.closest(selector));
    const focusId = trigger?.dataset.badge;
    const draft = { order: [...state.order], hidden: [...state.hidden] };
    let selected = find(initial) ? initial : draft.order[0] || 'member';
    let saving = false, token = '';
    const grid = h('div', { class: 'badge-directory' });
    const preview = h('div', { class: 'badge-slot-preview' });
    const error = h('div', { class: 'badge-error', role: 'alert' });
    const save = h('button', { class: 'btn primary', type: 'button' }, '儲存顯示設定');
    const summary = h('div', { class: 'badge-summary', 'aria-live': 'polite' });
    const render = (focusId = '') => {
      put(preview, h('span', { class: 'up-label' }, '個人檔案預覽'), row('user', draft, id => { selected = id || selected; render(); }));
      const ids = [...draft.order, ...state.catalog.map(b => b.id).filter(id => !draft.order.includes(id))];
      const badge = find(selected);
      put(summary, `已持有 ${state.owned.length}／${state.catalog.length} 枚 · 展示上限 6 枚`);
      put(grid,
        h('div', { class: 'badge-list', role: 'group', 'aria-label': '徽章目錄' }, ids.map(id => {
          const owned = state.owned.includes(id), hidden = draft.hidden.includes(id);
          const position = draft.order.filter(key => !draft.hidden.includes(key)).indexOf(id);
          const status = !owned ? '未取得' : hidden ? '已隱藏' : position < 6 ? '展示中' : '目錄收藏';
          return h('button', { class: 'badge-entry' + (id === selected ? ' sel' : '') + (!owned ? ' unowned' : ''),
            type: 'button', 'data-badge-entry': id, 'aria-pressed': String(id === selected),
            onclick: () => { selected = id; render(id); } }, art(id),
          h('span', { class: 'badge-entry-copy' }, h('strong', {}, find(id)?.name), h('span', {}, status)));
        })),
        h('section', { class: 'badge-inspect', 'aria-label': '徽章詳情' }, art(selected, 64),
          h('h3', {}, badge?.name || ''), h('p', {}, badge?.description || ''),
          h('div', { class: 'up-label' }, '取得方式'), h('p', { class: 'muted' }, badge?.criteria || ''),
          state.owned.includes(selected) ? h('div', { class: 'badge-controls' },
            h('button', { class: 'btn secondary sm', type: 'button', disabled: saving, onclick: () => {
              draft.hidden = draft.hidden.includes(selected) ? draft.hidden.filter(id => id !== selected) : [...draft.hidden, selected];
              render(selected);
            } }, draft.hidden.includes(selected) ? '顯示徽章' : '隱藏徽章'),
            ...[-1, 1].map(delta => h('button', { class: 'btn outline sm', type: 'button', disabled: saving ||
              draft.order.indexOf(selected) + delta < 0 || draft.order.indexOf(selected) + delta >= draft.order.length,
              'aria-label': delta < 0 ? '徽章向前移' : '徽章向後移', onclick: () => {
                const index = draft.order.indexOf(selected), next = index + delta;
                [draft.order[index], draft.order[next]] = [draft.order[next], draft.order[index]];
                render(selected);
              } }, delta < 0 ? '向前移' : '向後移'))) : h('p', { class: 'hint' }, '此本機資料尚未取得這枚徽章。')));
      if (focusId) grid.querySelector(`[data-badge-entry="${CSS.escape(focusId)}"]`)?.focus();
    };
    save.addEventListener('click', () => {
      if (saving) return;
      saving = true;
      token = 'badges-' + (++serial);
      save.disabled = true;
      save.textContent = '儲存中…';
      error.textContent = '';
      render();
      send({ type: 'badges.display', order: draft.order, hidden: draft.hidden, requestToken: token });
    });
    let close;
    editor = { result(message) {
      if (!saving || message.requestToken !== token) return;
      saving = false;
      if (message.ok) { close(); api.toast('ok', '徽章顯示設定已儲存'); }
      else { error.textContent = message.error || '徽章設定未能儲存。'; save.disabled = false; save.textContent = '重試儲存'; render(); }
    } };
    close = modal({ title: '個人徽章', cls: 'badge-modal',
      desc: '身分、版本與參與紀念。排序及隱藏僅調整個人檔案的展示。',
      body: [summary, preview, grid, error, h('p', { class: 'badge-local-note' },
        '本機資料 · 徽章不提供權限，也不是連線帳號認證。未展示的徽章仍保留於目錄。')],
      canClose: () => !saving, onClose: () => {
        editor = null;
        queueMicrotask(() => {
          if (trigger?.isConnected) { trigger.focus(); return; }
          const parent = scope && document.querySelector(scope);
          const target = parent?.querySelector(focusId ? `[data-badge="${CSS.escape(focusId)}"]` : '.badge-overflow');
          (target || parent?.querySelector('.profile-badge') || document.querySelector('[data-manage-badges]'))?.focus();
        });
      },
      foot: finish => [h('button', { class: 'btn ghost', type: 'button', onclick: finish }, '取消'), save] });
    render();
  }

  return { setup(value) { api = value; }, update(value) {
    if (!value) return;
    state = value;
    for (const element of document.querySelectorAll('.profile-badges'))
      if (!element.closest('.badge-modal')) element.replaceWith(row('user'));
  },
    result(message) { editor?.result(message); }, row, open, art };
})();
