// Native-backed context receipts, instruction queues and workspace layout controls.
'use strict';
window.CCADesk = {
  create({ S, h, put, icon, send, modal, toast, schedule, conv, onLayout }) {
    S.desk = { contexts: {}, pending: {}, layout: null, update: {}, tab: 'context', body: null, convId: '' };
    const defaults = { preset: 'collaboration', side: 240, panel: 340, font: 15, density: 'comfortable' };
    const presets = {
      reading: { ...defaults, preset: 'reading', side: 200, panel: 300, font: 17 },
      collaboration: defaults,
      review: { ...defaults, preset: 'review', side: 200, panel: 460, font: 14, density: 'compact' },
    };
    let sequence = 0;
    function layout(value) {
      const v = { ...defaults, ...(value || {}) };
      v.side = Math.max(180, Math.min(380, Number(v.side) || 240));
      v.panel = Math.max(260, Math.min(520, Number(v.panel) || 340));
      v.font = Math.max(13, Math.min(19, Number(v.font) || 15));
      S.desk.layout = v;
      fit();
    }
    function fit() {
      const v = S.desk.layout || defaults;
      const root = document.documentElement;
      const width = window.innerWidth;
      const panel = document.querySelector('#panel');
      const docked = width >= 1200 && panel && !panel.classList.contains('collapsed');
      const side = Math.max(180, Math.min(v.side, width - 72 - 400 - (docked ? 260 : 0)));
      const right = Math.max(260, Math.min(v.panel, width - 72 - side - 400));
      root.style.setProperty('--desk-side', side + 'px');
      root.style.setProperty('--desk-panel', right + 'px');
      root.style.setProperty('--desk-font', v.font + 'px');
      root.dataset.density = v.density;
      root.dataset.layout = v.preset;
    }
    function saveLayout(value) {
      const token = 'layout-' + (++sequence);
      S.desk.pending[token] = { kind: 'layout', previous: S.settings.layout || defaults };
      layout(value);
      send({ type: 'layout.set', requestToken: token, layout: value });
    }
    function splitters() {
      const host = document.querySelector('#layout');
      if (!host || host.querySelector('.desk-split')) return;
      for (const key of ['side', 'panel']) {
        const splitter = h('div', { class: 'desk-split desk-split-' + key, role: 'separator', tabIndex: 0,
          'aria-label': key === 'side' ? '調整頻道欄寬' : '調整資訊欄寬', 'aria-orientation': 'vertical' });
        splitter.addEventListener('keydown', e => {
          if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(e.key)) return;
          e.preventDefault();
          const v = { ...(S.desk.layout || defaults), preset: 'custom' };
          const min = key === 'side' ? 180 : 260, max = key === 'side' ? 380 : 520;
          v[key] = e.key === 'Home' ? min : e.key === 'End' ? max :
            Math.max(min, Math.min(max, v[key] + (e.key === 'ArrowRight' ? 10 : -10) * (key === 'side' ? 1 : -1)));
          saveLayout(v);
        });
        splitter.addEventListener('pointerdown', e => {
          if (e.button !== 0) return;
          e.preventDefault(); splitter.setPointerCapture(e.pointerId);
          const original = { ...(S.desk.layout || defaults) }, start = e.clientX;
          const move = event => {
            const min = key === 'side' ? 180 : 260, max = key === 'side' ? 380 : 520;
            layout({ ...original, preset: 'custom', [key]: Math.max(min, Math.min(max,
              original[key] + (event.clientX - start) * (key === 'side' ? 1 : -1))) });
          };
          const done = event => {
            splitter.removeEventListener('pointermove', move);
            splitter.removeEventListener('pointerup', done);
            splitter.removeEventListener('pointercancel', cancel);
            splitter.releasePointerCapture(event.pointerId);
            saveLayout({ ...S.desk.layout });
          };
          const cancel = () => {
            splitter.removeEventListener('pointermove', move);
            splitter.removeEventListener('pointerup', done);
            splitter.removeEventListener('pointercancel', cancel);
            layout(original);
          };
          splitter.addEventListener('pointermove', move);
          splitter.addEventListener('pointerup', done);
          splitter.addEventListener('pointercancel', cancel);
        });
        host.append(splitter);
      }
      window.addEventListener('resize', fit);
      new MutationObserver(fit).observe(document.querySelector('#panel'), { attributes: true, attributeFilter: ['class'] });
      fit();
    }
    function action(type, c, extra = {}, callback, failure) {
      const token = 'desk-' + (++sequence);
      S.desk.pending[token] = { kind: 'queue', callback, failure };
      send({ type: 'queue.' + type, id: c.id, requestToken: token, ...extra });
    }
    function receipt(data, label) {
      return h('details', { class: 'desk-receipt' }, h('summary', {}, label,
        h('span', { class: 'hint' }, ` ${data?.bytes || 0} bytes${data?.truncated ? ' · 顯示已截短' : ' · 完整紀錄'}`)),
      h('pre', { class: 'desk-pre' }, data?.text || '無'));
    }
    function renderContext(body, id) {
      const entries = S.desk.contexts[id];
      body.append(h('p', { class: 'hint' },
        '記錄 CCA 交付的文字與規則。工具路徑由後端回報，未宣稱已讀取內容；外部 CLI 的內部記憶、截斷與注意力未回報，視為未知。最多保留最近 20 輪，每份文字最多 64 KiB。'));
      if (!entries) { body.append(h('p', {}, '正在讀取來源紀錄…')); return; }
      if (!entries.length) body.append(h('p', {}, '尚無來源紀錄。此版本開始送出的新一輪才會建立紀錄。'));
      for (const item of [...entries].reverse()) {
        body.append(h('details', { class: 'desk-receipt' },
          h('summary', {}, `${item.agent} · ${new Date(item.ts).toLocaleString()} · ${{ submitted: '已提交，尚待後端回報', running: '執行中', completed: '已完成', failed: '失敗', interrupted: '中斷' }[item.status] || item.status}`),
          h('div', { class: 'desk-meta' }, `模型：${item.model || '後端預設'} · 權限：${item.permission || '未知'}`,
            h('br'), `工作區：${item.cwd || '未設定'}`, h('br'), `來源檢查點：${item.snapshot || '未取得'}`,
            h('br'), `交接索引：${item.handoffCount || 0} 筆 · 外部記憶：${item.history === 'external-session' ? '接續既有工作階段，內容未知' : '新工作階段'}`),
          receipt(item.request, '本輪需求'), receipt(item.instructions, 'CCA 規則與角色指示'),
          receipt(item.input, '實際交付文字（含 CCA 交接及摘要）'),
          h('div', { class: 'hint' }, '以下僅為後端回報的工具活動；檔案版本未取得時標示未知。'),
          (item.observations || []).map(o => h('div', { class: 'desk-observation' },
            `${o.tool || o.title || '工具'} · ${o.path || '未回報路徑'} · 版本：${o.version || '未知'}`))));
      }
    }
    function renderQueue(body, id) {
      const c = S.convs.find(c => c.id === id);
      if (!c) return;
      const st = S.states[id] || {}, items = st.queue || [];
      body.append(h('p', { class: 'hint' }, '執行中的指令不會被插入新文字。待送項目在整個任務成功結束後依序傳送；暫停、失敗、重啟或手動停止後，需要重新啟用。中斷項目不會自動重送。'),
        h('div', { class: 'desk-actions' },
          h('span', {}, st.queueArmed ? '佇列已啟用' : '佇列已暫停'),
          h('button', { class: 'btn secondary sm', onclick: () => action(st.queueArmed ? 'pause' : 'resume', c) }, st.queueArmed ? '暫停待送' : '啟用待送'),
          h('button', { class: 'btn ghost sm', onclick: () => action('get', c) }, '重新整理')));
      if (!items.length) body.append(h('p', {}, '沒有待送指令。回覆期間可在輸入欄加入佇列。'));
      items.forEach((item, index) => {
        const active = item.status === 'dispatching';
        const status = { pending: '待送', dispatching: '已交付，處理中', delivered: '處理完成', interrupted: '中斷，需確認後重試' }[item.status] || item.status;
        body.append(h('section', { class: 'desk-queue-item', 'data-queue-id': item.id },
          h('div', { class: 'hint' }, `${index + 1} · ${status} · ${new Date(item.ts).toLocaleString()}`),
          h('div', { class: 'desk-queue-text' }, item.text),
          h('div', { class: 'desk-actions' },
            !active ? h('button', { class: 'btn ghost sm', onclick: () => {
              const input = h('textarea', { rows: 5, class: 'input', value: item.text });
              modal({ title: '編輯待送指令', body: input, foot: close => [h('button', { class: 'btn primary',
                onclick: () => action('edit', c, { itemId: item.id, text: input.value }, close) }, '儲存')] });
            } }, '編輯') : null,
            !active ? h('button', { class: 'btn ghost sm', disabled: index === 0,
              onclick: () => action('move', c, { itemId: item.id, to: index - 1 }) }, '上移') : null,
            !active ? h('button', { class: 'btn ghost sm', disabled: index === items.length - 1,
              onclick: () => action('move', c, { itemId: item.id, to: index + 1 }) }, '下移') : null,
            item.status === 'interrupted' ? h('button', { class: 'btn secondary sm',
              onclick: () => action('retry', c, { itemId: item.id }) }, '確認重試') : null,
            !active ? h('button', { class: 'btn ghost sm', onclick: () => action('remove', c, { itemId: item.id }) }, '取消') : null)));
      });
    }
    function redraw() {
      const body = S.desk.body;
      if (!body?.isConnected) return;
      put(body);
      const id = S.desk.convId;
      body.append(h('div', { class: 'desk-tabs', role: 'tablist', 'aria-label': '工作台' },
        ['context', 'queue'].map(tab => h('button', { class: 'btn ' + (S.desk.tab === tab ? 'primary' : 'ghost'),
          role: 'tab', 'aria-selected': String(S.desk.tab === tab), onclick: () => { S.desk.tab = tab; redraw(); } },
        tab === 'context' ? '上下文來源' : '待送指令'))));
      if (S.desk.tab === 'context') renderContext(body, id); else renderQueue(body, id);
    }
    function open(c, tab = 'context') {
      S.desk.convId = c.id; S.desk.tab = tab;
      S.desk.body = h('div', { class: 'desk-dialog' });
      modal({ title: '工作台', wide: true, body: S.desk.body });
      redraw(); send({ type: 'context.get', id: c.id }); action('get', c);
    }
    function settings(body) {
      const value = S.desk.layout || defaults;
      const font = h('input', { type: 'range', min: 13, max: 19, step: 1, value: value.font,
        'aria-label': '對話文字大小', onchange: e => saveLayout({ ...value, preset: 'custom', font: Number(e.target.value) }) });
      const density = h('select', { 'aria-label': '介面密度', onchange: e => saveLayout({ ...value, density: e.target.value }) },
        h('option', { value: 'comfortable', selected: value.density === 'comfortable' }, '舒適'),
        h('option', { value: 'compact', selected: value.density === 'compact' }, '緊湊'));
      body.append(h('h2', {}, '版面與閱讀'), h('p', { class: 'hint' }, '拖曳頻道欄及資訊欄的邊界調整寬度，也可聚焦分隔線後使用方向鍵。窄視窗會保留對話閱讀空間。'),
        h('div', { class: 'desk-actions' }, Object.entries(presets).map(([key, data]) => h('button', {
          class: 'btn ' + (value.preset === key ? 'primary' : 'secondary'), onclick: () => { saveLayout({ ...data }); onLayout(key); },
        }, { reading: '閱讀', collaboration: '協作', review: '審查' }[key]))),
        h('div', { class: 'field' }, h('label', {}, `對話文字 ${value.font} px`), font),
        h('div', { class: 'field' }, h('label', {}, '介面密度'), density));
    }
    // Edition switches are never automatic, and the dialog says what is kept aside and what comes back.
    function switchDialog(choice) {
      const target = choice.edition ? choice.editionName : '普通版本';
      const text = choice.kind === 'return'
        ? `回到普通版本 ${choice.version}。切換到特殊版本時保存的主題、性格、插件，以及各對話的日常模式、本地審查員、手動模型與思考程度、插件模式會還原；在特殊版本裡建立的對話與訊息都會保留。`
        : !S.edition?.id
          ? `${target} 是另一條產品路線，不是 CCA 的下一版。切換後，${target} 不支援的設定（主題、性格、插件，以及各對話的日常模式、本地審查員、手動模型與思考程度、插件模式）會保存在 editions\\ordinary-vault.json，回到 1.0.1-astra 以後的普通版本時還原。對話、訊息與專案不受影響；隨時可以退回普通版本。`
          : `改用 ${target} ${choice.version}。先前保存的普通版本設定會繼續保留，回到普通版本時還原。`;
      modal({ title: choice.kind === 'return' ? '退回普通版本' : `切換到 ${target}`, body: h('p', {}, text),
        foot: close => [h('button', { class: 'btn ghost', onclick: close }, '取消'),
          h('button', { class: 'btn primary', onclick: () => { close(); send({ type: 'update.install', choice: choice.edition || 'normal' }); } },
            choice.kind === 'return' ? '退回並重新開啟' : '切換並重新開啟')] });
    }
    function updateSettings(body) {
      const u = S.desk.update;
      const status = { unchecked: '尚未掃描', checking: '掃描本機更新資料夾', empty: '資料夾中沒有安裝包',
        downloading: '核對並準備安裝包', available: '有較新的安裝包', waiting: '等待目前的工作結束後自動更新',
        scheduled: '即將自動更新並重新開啟', current: '安裝包都不比目前版本新', error: '更新未完成' }[u.status] || '尚未掃描';
      const remote = u.remote || {};
      const here = S.edition?.id ? `${S.edition.name} ${S.version}` : S.version;
      body.append(...[h('h2', {}, '軟體更新'), h('p', {}, `本機版本 ${here} · ${status}`),
        u.version ? h('p', {}, `安裝包版本 ${u.version}${u.package ? ' · ' + u.package : ''}`) : null,
        u.notes ? h('pre', { class: 'desk-pre' }, u.notes) : null,
        u.error ? h('p', { class: 'danger-text', role: 'alert' }, u.error) : null,
        h('section', { class: 'update-source' },
          h('div', { class: 'update-source-head' }, h('strong', {}, '本機資料夾'), h('span', { class: 'tag' }, '已啟用')),
          h('p', { class: 'update-folder' }, u.folder || '啟動後自動建立'),
          h('p', { class: 'hint' }, '把 make_release.py 產生的發行資料夾，或單一 ChatClaudeAgent 執行檔放進這裡。CCA 每 5 秒檢查一次，'
            + '只接受產品名稱、殼層類型與版本都相符、而且比目前版本新的檔案；SHA-256 核對失敗或啟動失敗時保留舊版。'),
          h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: u.auto !== false, disabled: u.busy,
            onchange: e => send({ type: 'update.auto', auto: e.target.checked }) }), '發現新安裝包時自動更新（閒置 10 秒後重新開啟）'),
          h('div', { class: 'desk-actions' },
            h('button', { class: 'btn secondary', onclick: () => send({ type: 'update.open' }) }, '開啟資料夾'),
            h('button', { class: 'btn secondary', disabled: u.busy, onclick: () => send({ type: 'update.check' }) }, '立即掃描'),
            u.status === 'scheduled' ? h('button', { class: 'btn secondary', onclick: () => send({ type: 'update.cancel' }) }, '取消這次自動更新') : null,
            u.available ? h('button', { class: 'btn primary', disabled: u.busy, onclick: () => {
              modal({ title: '安裝更新並重新開啟', body: h('p', {}, '安裝包核對完成後，CCA 將關閉並顯示圖標動畫。所有任務與佇列需先停止，對話資料會保留。'),
                foot: close => [h('button', { class: 'btn primary', onclick: () => { close(); send({ type: 'update.install' }); } }, '更新並重新開啟')] });
            } }, '更新並重新開啟') : null)),
        (u.choices || []).length ? h('section', { class: 'update-source', 'data-update-choices': '' },
          h('div', { class: 'update-source-head' }, h('strong', {}, S.edition?.id ? '普通版本與其他特殊版本' : '特殊版本')),
          h('p', { class: 'hint' }, S.edition?.id ? `可以回到普通版本，或換成另一個特殊版本；${S.edition.name} 有新版時會照自動更新設定處理。`
            : '特殊版本（Haiku、Sonnet、Opus、Fable）是不同定位的產品路線，不是 CCA 的下一版，只在你選擇時切換，不會自動安裝。'),
          ...u.choices.map(choice => h('div', { class: 'update-choice', 'data-edition': choice.edition || 'normal' },
            h('p', {}, h('strong', {}, `${choice.edition ? choice.editionName : '普通版本'} ${choice.version}`),
              choice.package ? h('span', { class: 'hint' }, ' · ' + choice.package) : null),
            choice.notes ? h('pre', { class: 'desk-pre' }, choice.notes) : null,
            h('button', { class: 'btn secondary', disabled: u.busy, onclick: () => switchDialog(choice) },
              choice.kind === 'return' ? '退回普通版本' : `切換到 ${choice.editionName}`)))) : null,
        (u.android || []).length ? h('section', { class: 'update-source', 'data-update-android': '' },
          h('div', { class: 'update-source-head' }, h('strong', {}, 'Android 版本'), h('span', { class: 'tag' }, '只能在手機安裝')),
          ...u.android.map(a => h('div', { class: 'update-choice' },
            h('p', {}, h('strong', {}, `${a.editionName} ${a.version}`), h('span', { class: 'hint' }, ' · 只能在 Android 使用，這台電腦不會安裝')),
            h('p', { class: 'update-folder' }, a.path),
            h('p', { class: 'hint' }, '把這個 APK 傳到 Android 手機，在手機上開啟並允許安裝。')))) : null,
        h('section', { class: 'update-source disabled', 'aria-disabled': 'true' },
          h('div', { class: 'update-source-head' }, h('strong', {}, '遠端發行站'), h('span', { class: 'tag' }, '未啟用')),
          h('p', { class: 'update-folder' }, remote.source || '尚未設定 HTTPS manifest.json'),
          h('p', { class: 'hint' }, '遠端下載目前不啟用，CCA 不會連線檢查。保留的位址只供日後開放時沿用。')),
        h('p', { class: 'hint' }, '重大規範化的順序：無後綴 → oss → luna → terra → sol → astra。sol 或 astra 後的一般更新進入下一個修訂版。相同版本不會重複更新。'
          + '特殊版本（Haiku、Sonnet、Opus、Fable）自成一條版本線，只在你選擇時切換。')].filter(Boolean));
    }
    function message(m) {
      if (m.type === 'boot') { layout(m.settings?.layout); splitters(); }
      if (m.type === 'context') { S.desk.contexts[m.id] = m.items || []; redraw(); return true; }
      if (m.type === 'queue.result') {
        S.states[m.id] = { ...S.states[m.id], queue: m.queue || [], queueArmed: m.armed };
        const pending = S.desk.pending[m.requestToken]; delete S.desk.pending[m.requestToken];
        if (!m.ok) { pending?.failure?.(); toast('error', m.error || '佇列未能保存'); }
        else pending?.callback?.();
        redraw(); schedule(); return true;
      }
      if (m.type === 'layout.result') {
        const pending = S.desk.pending[m.requestToken]; delete S.desk.pending[m.requestToken];
        if (m.ok) { S.settings.layout = m.layout; layout(m.layout); }
        else { layout(pending?.previous); toast('error', m.error || '版面未能保存'); }
        return true;
      }
      if (m.type === 'update.state') { S.desk.update = m; return false; }
      return false;
    }
    return { open, action, settings, updateSettings, message, fit };
  },
};
