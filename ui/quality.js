// Host verification and browser observations share the plan's revision, without treating model claims as test results.
window.CCAQuality = (() => {
  let api, refresh;
  const setup = hooks => { api = hooks; };
  const data = c => api.state(c.id).collaboration || c.collaboration || {};
  const button = (text, onclick, cls = 'secondary') => api.h('button', { class: 'btn sm ' + cls, type: 'button', onclick }, text);
  const field = (label, el) => api.h('label', { class: 'field' }, api.h('span', {}, label), el);
  const input = (value, label, rows = 0) => api.h(rows ? 'textarea' : 'input', { class: 'input', 'aria-label': label, value, ...(rows ? { rows } : {}) });
  const evidenceLabel = e => e.stale ? '證據已過期' : e.source === 'mock' ? '模擬驗證' : e.passed ? '主機驗證通過' : '驗證未通過';
  const stepLabel = (id, evidenceId) => {
    const e = (data(api.conv(id)).evidence || []).find(e => e.id === evidenceId);
    return e ? evidenceLabel(e) : '人工核對，尚無主機驗證證據';
  };
  const update = () => refresh?.();
  const send = (c, type, extra = {}) => api.send({ type, id: c.id, ...extra });
  const revision = c => data(c).plan?.revision || 0;
  function evidenceDialog(c, seed = '', reproduce = false) {
    const { h, put } = api;
    const kind = h('select', { class: 'select', 'aria-label': '驗證種類' }, h('option', { value: 'test' }, '執行本機測試'), h('option', { value: 'repro' }, '修正前後執行相同案例'));
    const pick = h('select', { class: 'select', 'aria-label': '對應需求' }, h('option', { value: '' }, '整份任務'), (data(c).plan?.steps || []).map(s => h('option', { value: s.id }, s.text)));
    const label = input(seed, '證據名稱'), cmd = input('["python", "-m", "unittest"]', '驗證命令 JSON', 3);
    const paths = input('', '對應檔案', 2), base = input('HEAD', '修正前 Git 版本');
    const cases = input('', '相同重現測試檔案', 2), marker = input('', '修正前預期失敗訊息');
    const repro = h('div', {}, field('修正前 Git 版本', base), field('相同測試檔案（每行一個）', cases), field('修正前必須出現的失敗訊息', marker), h('p', { class: 'hint' }, '兩份暫存來源使用相同測試檔與命令。舊版須出現指定失敗，新版須成功；不複製套件資料夾與機密設定。'));
    kind.onchange = () => { repro.hidden = kind.value !== 'repro'; }; repro.hidden = true;
    if (reproduce) { kind.value = 'repro'; repro.hidden = false; }
    const list = h('div'), state = h('p', { class: 'hint', role: 'status' });
    refresh = () => {
      const q = api.quality(c.id) || {};
      state.textContent = q.error || (api.state(c.id).running ? '驗證或工作執行中' : '結果綁定來源指紋與計畫版本；來源變更後需重新核對。');
      put(list, ...(data(c).evidence || []).slice().reverse().map(e => h('details', { class: 'card quality-evidence' },
        h('summary', {}, h('strong', {}, e.label || e.kind), h('span', { class: 'tag' }, evidenceLabel(e))),
        h('p', { class: 'hint' }, new Date(e.ts).toLocaleString('zh-TW') + ' · 計畫版本 ' + (e.revision || 0) + ' · ' + (e.step || '整份任務')),
        h('div', { class: 'mono hint' }, '來源 ' + (e.binding || '').slice(0, 16)), h('p', { class: 'hint' }, (e.paths || []).map(p => p.path).join('、')),
        h('pre', { class: 'task-record' }, e.output || '沒有文字輸出'), e.note ? h('p', { class: 'hint' }, e.note) : null,
        e.image ? h('img', { class: 'quality-saved-image', src: api.mediaUrl(e.image), alt: '驗收當時的瀏覽器畫面' }) : null)));
    };
    api.modal({ title: '需求與驗收證據', desc: '執行結果由主機取得。AI 自述與人工勾選會保留各自的意義。', wide: true,
      body: [field('驗證種類', kind), field('對應需求', pick), field('證據名稱／審查疑慮', label), field('命令參數（JSON 陣列，不使用 shell）', cmd), field('對應檔案（每行一個）', paths), repro, state, list],
      onClose: () => { refresh = null; }, foot: close => [button('關閉', close, 'ghost'), button('請 AI 建立重現案例', () => {
        send(c, 'conv.send', { text: '【建立最小重現案例】\n疑慮：' + label.value + '\n只新增重現測試，不修改實作、既有測試或驗收條件。列出可直接執行的 JSON 命令陣列、測試檔案與預期失敗訊息，交回人工確認後才執行。' });
      }), button('重新核對來源', () => send(c, 'quality.get')), button('核對並執行', () => {
        let command;
        try { command = JSON.parse(cmd.value); if (!Array.isArray(command)) throw Error(); }
        catch { state.textContent = '命令需要 JSON 參數陣列'; return; }
        api.modal({ title: '確認執行驗證', desc: '專案：' + c.cwd, body: h('pre', { class: 'task-record' }, JSON.stringify(command, null, 2)),
          foot: done => [button('取消', done, 'ghost'), button('執行此命令', () => { done(); send(c, 'quality.run', { kind: kind.value, command, label: label.value, revision: revision(c), step: pick.value,
            paths: paths.value.split('\n').map(s => s.trim()).filter(Boolean), base: base.value, cases: cases.value.split('\n').map(s => s.trim()).filter(Boolean), expectedFailure: marker.value, confirm: true }); }, 'primary')] });
      }, 'primary')] });
    refresh(); send(c, 'quality.get');
  }
  function browserDialog(c) {
    const { h, put } = api;
    const url = input('http://127.0.0.1:3000', '預覽網址'), text = input('', '預覽輸入文字');
    const mode = h('select', { class: 'select', 'aria-label': '預覽點選模式' }, h('option', { value: 'inspect' }, '點選元素並留言'), h('option', { value: 'click' }, '操作網頁'));
    const frame = h('img', { class: 'quality-preview-image', alt: '本機瀏覽器即時預覽', draggable: false });
    const info = h('p', { class: 'hint', role: 'status' }), selected = h('div'), errors = h('pre', { class: 'task-record' });
    const notes = input('', '元素修改意見', 3), actions = input('[\n  {"action":"expectVisible", "selector":"body"}\n]', '操作驗收 JSON', 5);
    const pick = h('select', { class: 'select', 'aria-label': '網站驗收需求' }, h('option', { value: '' }, '整份任務'), (data(c).plan?.steps || []).map(s => h('option', { value: s.id }, s.text)));
    let current = {}, waiting = false;
    const act = (operation, extra = {}) => { if (waiting) return; waiting = true; send(c, 'browser.action', { operation, ...extra }); };
    frame.onclick = event => {
      if (!current.viewport) return;
      const r = frame.getBoundingClientRect();
      act(mode.value, { x: (event.clientX - r.left) * current.viewport.width / r.width, y: (event.clientY - r.top) * current.viewport.height / r.height });
    };
    refresh = () => {
      const q = api.quality(c.id) || {};
      if (q.error) { info.textContent = q.error; waiting = false; return; }
      if (q.result?.closed) { waiting = false; frame.removeAttribute('src'); info.textContent = '瀏覽器已結束'; return; }
      if (q.result?.screenshot) {
        current = q.result; frame.src = current.screenshot; waiting = false;
        info.textContent = `${current.title || ''} · ${current.url || ''} · ${current.viewport?.width || ''} px`;
        errors.textContent = (current.consoleErrors || []).join('\n') || '目前未收到瀏覽器錯誤';
        put(selected, current.selected ? h('p', { class: 'hint' }, `${current.selected.tag} ${current.selected.id ? '#' + current.selected.id : ''} · ${current.selected.text}`) : null);
      }
    };
    api.modal({ title: '網站預覽與操作驗收', desc: '獨立瀏覽器工作階段；點選模式可切換為操作網頁。畫面由本機傳回。', wide: true, cls: 'quality-browser',
      body: [field('網址', url), h('div', { class: 'collab-actions' }, button('開啟網站', () => act('open', { url: url.value })), button('桌面畫面', () => act('resize', { width: 1280 })), button('手機畫面', () => act('resize', { width: 390 })), button('更新畫面', () => act('capture')), mode), info,
        h('div', { class: 'quality-preview-surface' }, frame), h('div', { class: 'collab-actions' }, button('向下捲動', () => act('scroll', { delta: 600 })), button('向上捲動', () => act('scroll', { delta: -600 })), button('Enter', () => act('key', { key: 'Enter' }))),
        field('對焦欄位後輸入', text), button('輸入文字', () => { act('type', { text: text.value }); text.value = ''; }), selected, field('元素修改意見', notes), button('將元素與意見交給 AI', () => {
          send(c, 'conv.send', { text: '【網站元素修改】\n網址：' + (current.url || url.value) + '\n元素：' + JSON.stringify(current.selected || {}) + '\n要求：' + notes.value + '\n修改後以相同操作流程驗證，回報實際證據。' });
        }), h('details', {}, h('summary', {}, '瀏覽器錯誤'), errors), field('對應需求', pick), field('操作驗收（click／fill／expectText／expectVisible／expectUrl）', actions)],
      onClose: () => { refresh = null; }, foot: close => [button('關閉面板', close, 'ghost'), button('結束瀏覽器', () => { act('close'); close(); }), button('執行操作驗收並保存證據', () => {
        let steps;
        try { steps = JSON.parse(actions.value); if (!Array.isArray(steps)) throw Error(); }
        catch { info.textContent = '操作驗收需要 JSON 陣列'; return; }
        act('verify', { actions: steps, confirm: true, revision: revision(c), step: pick.value, label: '網站操作驗收' });
      }, 'primary')] });
    refresh();
  }
  function controls(c) {
    return api.h('section', { class: 'panel-section' }, api.h('div', { class: 'card' }, api.h('div', { class: 'card-title' }, '交付驗證'), api.h('div', { class: 'collab-actions' }, button('需求與驗收證據', () => evidenceDialog(c)), button('網站即時預覽', () => browserDialog(c)))));
  }
  return { setup, update, evidenceDialog, browserDialog, controls, stepLabel };
})();
