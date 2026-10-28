// Built-in theme previews and native-backed, per-provider automatic selection controls.
'use strict';
window.CCAAppearance = {
  create({ S, h, send, toast, schedule, renderSettings }) {
    S.appearance = { preview: '', choice: '', pending: '', sequence: 0, query: '', result: null, token: '' };
    // Built-in themes of this edition, then themes from enabled CCA plugins.
    const catalogue = () => [...(window.CCAThemeCatalog || []).filter(t => (t.edition || '') === (S.edition?.id || '')),
      ...(S.extensions?.themes || [])];
    function apply(id) {
      const list = catalogue();
      const theme = list.find(t => t.id === id) || list[0];
      if (!theme) return;
      const root = document.documentElement;
      for (const [key, value] of Object.entries(theme.tokens)) root.style.setProperty('--' + key, value);
      // A plugin theme borrows the devices of the built-in theme it names as its base; otherwise only colors change.
      root.dataset.theme = theme.plugin ? theme.base || 'plugin' : theme.id;
      root.dataset.shape = theme.shape;
      root.dataset.scheme = theme.scheme;
      root.style.colorScheme = theme.scheme;
    }
    function cancelPreview() {
      S.appearance.preview = ''; S.appearance.choice = '';
      apply(S.settings.theme || 'cca');
    }
    function mini(theme) {
      const t = theme.tokens;
      // Signature tokens travel as custom properties, so themes.css can draw each theme's own device in the card.
      const vars = { '--m-signal': t.signal, '--m-rail': t['bg-rail'], '--m-display': t['theme-display'] || t.font };
      for (let i = 1; i <= 6; i++) vars['--m-a' + i] = t['theme-a' + i] || t.signal;
      const root = h('div', { class: 'theme-mini', 'data-preview': theme.plugin ? theme.base || 'plugin' : theme.id, style: { background: t['bg-chat'], color: t.text } },
        h('div', { class: 'theme-mini-rail' },
          [0, 1, 2].map(i => h('i', { style: { background: i ? t['bg-selected'] : t.signal } }))),
        h('div', { class: 'theme-mini-side', style: { background: t['bg-side'] } },
          [0, 1, 2, 3].map(i => h('i', { style: { background: i === 1 ? t.signal : t.line } }))),
        h('div', { class: 'theme-mini-chat' },
          h('strong', {}, 'CCA'), h('i', { style: { background: t.line } }),
          h('i', { style: { background: t.line } }),
          h('span', { style: { background: t['bg-input'], borderColor: t.line } },
            h('b', { style: { background: t.brand, color: t['brand-ink'] } }, '↑'))),
        h('i', { class: 'theme-mini-motif', 'aria-hidden': 'true' }));
      for (const [key, value] of Object.entries(vars)) root.style.setProperty(key, value);
      return root;
    }
    function chooseTheme(id) {
      S.appearance.choice = id; S.appearance.preview = id; apply(id); renderSettings();
      requestAnimationFrame(() => document.querySelector('[data-theme-option="' + id + '"]')?.focus());
    }
    function themePage(body) {
      const current = S.settings.theme || 'cca', chosen = S.appearance.choice || current, list = catalogue();
      body.append(h('h2', {}, '介面主題'), h('p', { class: 'hint' },
        '主題調整介面材質、留白、控制項與配色。選擇後可先預覽，套用後保存在本機。'),
      h('div', { class: 'theme-gallery', role: 'radiogroup', 'aria-label': '介面主題' },
        list.map(theme => h('button', { class: 'theme-option' + (chosen === theme.id ? ' selected' : ''),
          type: 'button', disabled: !!S.appearance.pending, role: 'radio', 'aria-label': theme.name,
          tabIndex: chosen === theme.id ? 0 : -1, 'aria-checked': String(chosen === theme.id), 'data-theme-option': theme.id,
          onclick: () => chooseTheme(theme.id), onkeydown: e => {
            if (!['ArrowRight', 'ArrowLeft', 'ArrowDown', 'ArrowUp', 'Home', 'End'].includes(e.key)) return;
            e.preventDefault();
            const index = list.findIndex(t => t.id === theme.id);
            const next = e.key === 'Home' ? 0 : e.key === 'End' ? list.length - 1 :
              (index + (e.key === 'ArrowRight' || e.key === 'ArrowDown' ? 1 : -1) + list.length) % list.length;
            chooseTheme(list[next].id);
          },
        }, mini(theme), h('span', { class: 'theme-option-name' }, theme.name),
        h('span', { class: 'hint' }, theme.kicker), h('span', { class: 'theme-option-description' }, theme.description),
        theme.motif ? h('span', { class: 'theme-option-motif' }, theme.motif) : null))),
      h('div', { class: 'theme-apply' },
        h('span', { class: 'hint', role: 'status' }, S.appearance.preview ? '預覽中 · 尚未保存' : '目前主題已保存'),
        h('button', { class: 'btn secondary', disabled: !S.appearance.preview || !!S.appearance.pending,
          onclick: () => { cancelPreview(); renderSettings(); } }, '取消預覽'),
        h('button', { class: 'btn primary', disabled: !!S.appearance.pending || chosen === current,
          onclick: () => {
            const token = 'theme-' + (++S.appearance.sequence); S.appearance.pending = token;
            send({ type: 'theme.set', theme: chosen, requestToken: token }); renderSettings();
          } }, S.appearance.pending ? '保存中…' : '套用主題')));
    }
    function routingPage(body, c) {
      body.append(h('h2', {}, '自動模型與思考程度'), h('p', { class: 'hint' },
        '每輪開始前依需求特徵選擇。只使用後端回報的候選模型及思考程度，不在執行中切換；指定模型或程度後，可分別保留手動控制。'));
      if (!c) { body.append(h('p', {}, '先開啟一個對話，再調整自動選擇。')); return; }
      if (S.appearance.convId !== c.id) { S.appearance.convId = c.id; S.appearance.result = null; S.appearance.token = ''; }
      const busy = S.states[c.id]?.running || c.daily;
      if (c.daily) body.append(h('p', { class: 'hint' }, '日常模式沿用固定的省額度設定；以下偏好適用於工作模式。'));
      const configured = c.pipeline?.nodes || [];
      const agents = c.kind === 'group' ? configured.length ? [...new Set(configured.map(n => n.backend))]
        .filter(a => a === 'claude' || a === 'codex') : ['claude', 'codex']
        : [c.kind === 'codex' || c.lead === 'codex' ? 'codex' : 'claude'];
      if (configured.some(n => n.backend === 'gemini')) body.append(h('section', { class: 'routing-provider' },
        h('h3', {}, 'Gemini'), h('p', { class: 'hint' },
          '目前使用 Gemini CLI 預設模型，思考程度由 CLI 決定。未取得後端模型目錄前，不提供自動選擇或猜測的模型選項。')));
      if (!agents.length) { body.append(h('p', { class: 'hint' }, '此流程沒有可自動選擇的 Claude／GPT 節點。')); return; }
      for (const agent of agents) {
        const options = c.adaptive?.[agent] || { model: false, effort: false };
        const toggle = (field, label) => h('label', { class: 'check' },
          h('input', { type: 'checkbox', checked: !!options[field], disabled: busy,
            onchange: e => send({ type: 'routing.set', id: c.id, agent, [field]: e.target.checked,
              requestToken: 'route-' + (++S.appearance.sequence) }) }), label);
        body.append(h('section', { class: 'routing-provider' }, h('h3', {}, agent === 'claude' ? 'Claude' : 'GPT'),
          toggle('model', '依需求自動選擇模型'), toggle('effort', '依需求自動選擇思考程度'),
          h('p', { class: 'hint' }, `${S.models[agent]?.length || 0} 個目錄項目 · 手動選擇可從頻道設定的模型選單調整。`)));
      }
      const agent = agents[0];
      const query = h('textarea', { class: 'input', rows: 4, 'data-keep': 'route-query', value: S.appearance.query,
        'aria-label': '選擇預覽的需求', placeholder: '輸入需求，查看預計選擇；不會呼叫模型。',
        oninput: e => { S.appearance.query = e.target.value; S.appearance.result = null; S.appearance.token = ''; body.querySelector('.route-preview')?.remove(); } });
      body.append(h('div', { class: 'field' }, h('label', {}, '選擇預覽 · ' + (agent === 'claude' ? 'Claude' : 'GPT')), query),
        h('button', { class: 'btn secondary', onclick: () => {
          S.appearance.token = 'preview-' + (++S.appearance.sequence);
          send({ type: 'routing.preview', id: c.id, agent, text: query.value, requestToken: S.appearance.token });
        } }, '預覽選擇'));
      const result = S.appearance.result;
      if (result) body.append(h('section', { class: 'route-preview', role: 'status' },
        h('strong', {}, `${result.category} · ${result.modelName || '後端預設'} · ${result.effort || '預設思考程度'}`),
        h('p', {}, result.reason),
        result.factors?.length ? h('dl', { class: 'route-factors' }, result.factors.map(f =>
          [h('dt', {}, f.label), h('dd', {}, f.evidence)])) : null,
        result.effortNote ? h('p', { class: 'hint' }, result.effortNote) : null));
    }
    function message(m) {
      if (m.type === 'boot') { S.edition = m.edition || { id: '', name: '' }; apply(m.settings?.theme || 'cca'); }
      if (m.type === 'settings' && !S.appearance.preview) apply(m.settings?.theme || 'cca');
      if (m.type === 'theme.result') {
        S.appearance.pending = '';
        if (m.ok) { S.settings.theme = m.theme; S.appearance.preview = ''; S.appearance.choice = ''; }
        else { S.appearance.preview = ''; S.appearance.choice = ''; toast('error', m.error || '主題保存失敗'); }
        apply(S.settings.theme || 'cca'); renderSettings(); schedule(); return true;
      }
      if (m.type === 'routing.result') {
        if (!m.ok) toast('error', m.error || '自動選擇設定未能保存');
        else {
          const c = S.convs.find(c => c.id === m.id);
          if (c) c.adaptive = m.adaptive;
        }
        renderSettings(); schedule(); return true;
      }
      if (m.type === 'routing.preview') {
        if (m.requestToken !== S.appearance.token) return true;
        S.appearance.result = m.result; renderSettings(); return true;
      }
      return false;
    }
    return { apply, themePage, routingPage, cancelPreview, message, catalogue };
  },
};
