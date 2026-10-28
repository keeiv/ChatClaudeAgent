// CCA plugins: the settings page, the review-before-enable dialog and the per-conversation mode picker.
// Plugins are data the host has validated; nothing here runs plugin code.
'use strict';
window.CCAExtensions = {
  create({ S, h, send, modal, toast, schedule, renderSettings, onThemes }) {
    S.extensions = { folder: '', available: true, plugins: [], themes: [], modes: [], commands: [] };
    const SETTING = { permission: '權限', maxRounds: '來回上限', lead: '主刀', autoFallback: '額度用完時跳過',
      geminiMember: 'Gemini 加入', localMembers: '本地審查員', regressionGuard: '測試守門', daily: '日常模式' };
    const scopeLabel = { group: '群組', dm: '私訊', any: '群組與私訊' };
    const value = v => Array.isArray(v) ? v.join('、') || '無' : v === true ? '開' : v === false ? '關' : String(v);
    function settingsList(settings) {
      const entries = Object.entries(settings || {});
      return entries.length ? h('ul', { class: 'plugin-list' }, entries.map(([k, v]) => h('li', {}, `${SETTING[k] || k}：${value(v)}`)))
        : h('p', { class: 'hint' }, '不改變設定。');
    }
    // Everything the plugin would change is shown before the user approves this exact file.
    function review(p) {
      const confirm = h('input', { type: 'checkbox' });
      const enable = h('button', { class: 'btn primary', disabled: true, onclick: () => {
        send({ type: 'extensions.enable', id: p.id, hash: p.hash }); close?.();
      } }, '啟用插件');
      confirm.onchange = () => { enable.disabled = !confirm.checked; };
      let close;
      modal({ title: `檢視插件：${p.name}`, wide: true, body: [
        h('p', { class: 'hint' }, `${p.id} · ${p.version || '未標版本'} · ${p.author || '未署名'} · SHA-256 ${p.hash.slice(0, 16)}`),
        p.description ? h('p', {}, p.description) : null,
        h('h3', {}, `主題 ${p.themes.length} 款`),
        p.themes.length ? h('ul', { class: 'plugin-list' }, p.themes.map(t => h('li', {}, `${t.name}（${t.scheme === 'light' ? '亮色' : '暗色'}${t.base ? '，沿用 ' + t.base + ' 的裝置' : ''}）`)))
          : h('p', { class: 'hint' }, '沒有主題。'),
        h('h3', {}, `模式 ${p.modes.length} 個`),
        ...p.modes.map(m => h('section', { class: 'plugin-mode' }, h('strong', {}, `${m.name} · ${scopeLabel[m.scope]}`),
          m.description ? h('p', { class: 'hint' }, m.description) : null, settingsList(m.settings),
          m.instructions ? h('pre', { class: 'mono plugin-pre' }, m.instructions) : null)),
        h('h3', {}, `指令 ${p.commands.length} 個`),
        ...p.commands.map(c => h('section', { class: 'plugin-mode' }, h('strong', { class: 'mono' }, '/' + c.name),
          h('span', { class: 'hint' }, ` · ${c.description || ''} · ${scopeLabel[c.scope]}`), h('pre', { class: 'mono plugin-pre' }, c.template))),
        h('p', { class: 'hint' }, '模式的指示會附在送給 AI 的說明裡，但不能改變工具權限、測試要求或 VERDICT 規則；插件不能設定 bypass 權限或測試指令。'
          + 'plugin.json 之後若被修改，插件會自動停用，需要重新檢視。'),
        h('label', { class: 'check' }, confirm, '我已檢視以上內容，啟用這個插件')],
      foot: c => { close = c; return [h('button', { class: 'btn ghost', onclick: c }, '取消'), enable]; } });
    }
    function page(body) {
      const e = S.extensions;
      body.append(h('h2', {}, 'CCA 插件'),
        h('p', { class: 'hint' }, '插件是放在插件資料夾裡的 plugin.json，可以加入介面主題、對話模式與指令。CCA 只讀取資料、不執行插件程式；'
          + '主題只能用 CCA 既有的形狀與裝置，顏色要通過對比檢查。Claude Code／Codex 自己的插件仍在「插件」頁管理。'));
      if (!e.available) {
        body.append(h('p', { role: 'status' }, `${S.edition?.name || '這個版本'} 不載入插件。你在普通版本啟用的插件已保存，回到普通版本時恢復。`));
        return;
      }
      body.append(h('div', { class: 'desk-actions' },
        h('button', { class: 'btn secondary', onclick: () => send({ type: 'extensions.open' }) }, '開啟插件資料夾'),
        h('button', { class: 'btn secondary', onclick: () => send({ type: 'extensions.scan' }) }, '重新掃描')),
      h('p', { class: 'update-folder' }, e.folder || '%APPDATA%\\CCA\\plugins'));
      if (!e.plugins.length) body.append(h('p', { class: 'hint' }, '資料夾裡還沒有插件。每個插件放在自己的子資料夾，裡面是 plugin.json；格式見 docs/plugins.md。'));
      for (const p of e.plugins) {
        const errors = p.errors || [];
        body.append(h('section', { class: 'card plugin-card', 'data-plugin': p.id },
          h('div', { class: 'card-title' }, p.name, h('span', { class: 'tag' }, p.enabled ? '已啟用' : p.changed ? '內容已改變' : '未啟用')),
          h('p', { class: 'hint' }, `${p.id || p.folder} · ${p.version || '未標版本'} · ${p.author || '未署名'}`),
          p.description ? h('p', {}, p.description) : null,
          h('p', { class: 'hint' }, `主題 ${p.themes.length} · 模式 ${p.modes.length} · 指令 ${p.commands.length}`),
          p.changed ? h('p', { class: 'danger-text' }, 'plugin.json 在啟用後被修改，已自動停用；請重新檢視。') : null,
          errors.length ? h('ul', { class: 'danger-text plugin-list' }, errors.map(x => h('li', {}, x))) : null,
          h('div', { class: 'desk-actions' }, p.enabled
            ? h('button', { class: 'btn secondary', onclick: () => send({ type: 'extensions.disable', id: p.id }) }, '停用')
            : h('button', { class: 'btn primary', disabled: errors.length > 0, onclick: () => review(p) }, p.changed ? '重新檢視並啟用' : '檢視並啟用'))));
      }
      if (e.commands.length) body.append(h('h3', {}, '可用的插件指令'), h('ul', { class: 'plugin-list' },
        e.commands.map(c => h('li', {}, h('span', { class: 'mono' }, '/' + c.name), ` · ${c.description || ''}（${scopeLabel[c.scope]}）`))),
      h('p', { class: 'hint' }, '在輸入框打 /指令名稱 加上內容送出，CCA 會換成插件的範本再交給 AI。'));
    }
    // Modes that apply to this conversation, for the channel settings panel.
    function modesFor(c) {
      return (S.extensions.modes || []).filter(m => m.scope === 'any' || (m.scope === 'group') === (c.kind === 'group'));
    }
    function message(m) {
      if (m.type !== 'extensions') return false;
      S.extensions = m;
      onThemes?.();
      if (renderSettings) renderSettings();
      schedule();
      return true;
    }
    return { page, modesFor, message };
  },
};
