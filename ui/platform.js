// Optional multi-agent controls, using the host's existing DOM and modal helpers.
window.CCAPlatform = (() => {
  let api;
  let state = { providers: {}, servers: [], sandbox: {} };
  let refreshEditor;
  let requestSequence = 0;
  const modelRequests = new Map();
  const setup = hooks => { api = hooks; };
  const refreshModels = () => refreshEditor?.();
  const update = value => {
    for (const [id] of modelRequests) {
      const before = state.providers?.[id], after = value.providers?.[id];
      if (!after || before?.url !== after.url || before?.protocol !== after.protocol) modelRequests.delete(id);
    }
    state = value;
    refreshModels();
  };
  const requestModels = (id, force = false) => {
    if (!state.providers?.[id] || modelRequests.get(id)?.pending || (!force && modelRequests.has(id))) return;
    const requestToken = String(++requestSequence);
    modelRequests.set(id, { pending: true, error: '', requestToken });
    api.send({ type: 'provider.models', id, requestToken });
  };
  const pinLabel = { ok: '與釘住時相同', changed: '描述已變更，已阻擋', unpinned: '尚未釘住', missing: '伺服器已不提供', unlisted: '不在核准清單' };
  // Shows what the server advertises right now. Only tools the user ticks are pinned, and the host re-checks each
  // hash against a fresh listing, so a description that changes after this dialog opened is refused.
  const toolDialog = message => {
    const { h, send, modal } = api, found = message.result, server = (state.servers || []).find(s => s.id === message.id);
    const allowed = new Set(server?.allowedTools || []), boxes = new Map();
    const rows = (found.tools || []).map(tool => {
      const permitted = allowed.has(tool.name), status = permitted ? found.pinState?.[tool.name] || 'unpinned' : 'unlisted';
      const box = h('input', { type: 'checkbox', disabled: !permitted, checked: status === 'ok' });
      if (permitted) boxes.set(tool.name, { box, hash: found.hashes?.[tool.name] });
      return h('div', { class: 'card tool-pin', 'data-pin-state': status },
        h('label', { class: 'check' }, box, h('strong', {}, tool.name), ' · ' + pinLabel[status]),
        h('p', { class: 'hint' }, tool.description || '（沒有描述）'),
        h('pre', { class: 'mono', style: { maxHeight: '120px', overflow: 'auto' } }, JSON.stringify(tool.inputSchema || {}, null, 2)),
        h('div', { class: 'mono hint' }, 'SHA-256 ' + (found.hashes?.[tool.name] || '').slice(0, 16)));
    });
    const missing = [...allowed].filter(name => !(found.tools || []).some(tool => tool.name === name));
    modal({ title: 'MCP 工具與釘住：' + message.id, wide: true, body: [
      h('p', { class: 'hint' }, '釘住會記下工具的名稱、描述、參數格式與註記的雜湊。之後伺服器改動其中任何一項，該工具會被阻擋，直到你重新核准。釘住不能證明工具的行為安全，只能擋下「核准後偷偷改描述」。勾選的工具才會被釘住。'),
      ...rows, missing.length ? h('p', { class: 'hint' }, '核准清單中伺服器已不提供：' + missing.join('、')) : null],
    foot: close => [h('button', { class: 'btn sm ghost', onclick: close }, '取消'), h('button', { class: 'btn sm primary', onclick: () => {
      const pins = {};
      for (const [name, entry] of boxes) if (entry.box.checked && entry.hash) pins[name] = entry.hash;
      send({ type: 'toolserver.pin', id: message.id, pins }); close();
    } }, '釘住所選工具')] });
  };
  const result = message => {
    if (message.action === 'toolserver.test' && message.result.ok) { toolDialog(message); return true; }
    if (message.action === 'toolserver.pin') {
      const rejected = message.result.rejected || [];
      api.toast(message.result.ok && !rejected.length ? 'ok' : 'error', !message.result.ok ? message.result.error || '釘住失敗'
        : rejected.length ? rejected.length + ' 個工具在你檢視後又變了，沒有釘住：' + rejected.join('、') : '已釘住所選工具');
      return true;
    }
    if (message.action === 'provider.save' && message.result.ok) modelRequests.delete(message.id || message.result.id);
    if (message.action !== 'provider.models') return false;
    const id = message.id || message.result.id;
    if (message.requestToken && message.requestToken !== modelRequests.get(id)?.requestToken) return true;
    const automatic = modelRequests.get(id)?.pending;
    modelRequests.set(id, { pending: false, error: message.result.ok ? '' : message.result.error || '無法載入模型清單' });
    if (message.result.ok && state.providers?.[id]) state.providers[id].models = message.result.models || [];
    refreshModels();
    return !!automatic;
  };
  const field = (label, input) => api.h('div', { class: 'field' }, api.h('label', {}, label), input);
  const input = (value = '', type = 'text') => api.h('input', { class: 'input', type, value });
  const button = (text, click, cls = 'secondary') => api.h('button', { class: 'btn sm ' + cls, onclick: click }, text);

  function editor(c) {
    const { h, send, modal } = api;
    let nodes = JSON.parse(JSON.stringify(c.pipeline?.nodes || [
      { id: 'author', name: 'Claude', backend: 'claude', role: 'write', model: c.claudeModel || '' },
      { id: 'reviewer', name: 'GPT', backend: 'codex', role: 'review', model: c.codexModel || '' },
    ]));
    const list = h('div', { class: 'pipeline-editor' });
    let bindings = [], save, localRequested = false;
    const canSave = () => bindings.every(binding => binding.valid);
    const refresh = () => {
      bindings.forEach(binding => binding.update());
      if (save) save.disabled = !canSave();
    };
    const cycles = input(c.pipeline?.maxCycles || 3, 'number'); cycles.min = 1; cycles.max = 20;
    const acceptance = h('textarea', { class: 'input', rows: 3, value: c.pipeline?.acceptance || '', 'aria-label': '驗收標準' });
    const requireTests = h('input', { type: 'checkbox', checked: c.pipeline?.requireTests !== false });
    const routing = h('select', { class: 'select' }, ['auto', 'full', 'simple'].map(value => h('option', { value },
      { auto: '自動：短任務減少審查者，安全／資料／API 等維持完整', full: '完整流程', simple: '第一位撰寫者與第一位審查者' }[value])));
    routing.value = c.pipeline?.routing || 'auto';
    const draw = () => {
      list.replaceChildren();
      bindings = [];
      nodes.forEach((node, index) => {
        const name = input(node.name || node.id); name.onchange = () => { node.name = name.value; };
        const kind = h('select', { class: 'select' }, ['claude', 'codex', 'gemini', 'api', 'local'].map(value => h('option', { value },
          { claude: 'Claude Code 訂閱', codex: 'Codex 訂閱', gemini: 'Gemini Google 帳號（免費／付費）', api: 'API（獨立計費）', local: 'Ollama（唯讀）' }[value])));
        kind.value = node.backend; kind.onchange = () => {
          node.backend = kind.value; node.model = ''; delete node.provider; delete node.effort;
          if (kind.value === 'local' && node.role === 'write') node.role = 'review';
          draw();
        };
        const role = h('select', { class: 'select' }, ['write', 'review', 'verify'].filter(x => node.backend !== 'local' || x !== 'write').map(value =>
          h('option', { value }, node.backend === 'local' ? { review: '唯讀審查', verify: '唯讀驗證' }[value]
            : { write: '撰寫／修正', review: '審查／修正', verify: '驗證／修正' }[value])));
        role.value = node.role; role.onchange = () => { node.role = role.value; };
        const model = h('select', { class: 'select', 'aria-label': '模型' });
        const hint = h('div', { class: 'hint', role: 'status', 'aria-live': 'polite' });
        const provider = h('select', { class: 'select', 'aria-label': 'API 供應商' });
        const retry = button('更新清單', () => {
          if (node.backend === 'api') requestModels(node.provider, true);
          else if (node.backend === 'local') send({ type: 'local.status' });
          else send({ type: 'auth.check', agent: node.backend });
          refresh();
        });
        const binding = { valid: true, update: () => {
          let models = [], status = '', pending = false;
          const cli = ['claude', 'codex', 'gemini'].includes(node.backend);
          if (node.backend === 'api') {
            const providers = Object.values(state.providers || {});
            if (!node.provider && providers.length) node.provider = providers[0].id;
            provider.replaceChildren(h('option', { value: '', disabled: true }, '選擇 API 供應商'),
              ...providers.map(p => h('option', { value: p.id }, p.name || p.id)));
            if (node.provider && !state.providers?.[node.provider]) provider.append(h('option', { value: node.provider, disabled: true }, '原供應商已移除'));
            provider.value = node.provider || ''; provider.disabled = !providers.length;
            requestModels(node.provider);
            const request = modelRequests.get(node.provider);
            pending = !!request?.pending;
            models = (state.providers?.[node.provider]?.models || []).map(m => ({ value: m.id, label: m.name || m.id, isDefault: m.isDefault }));
            status = !state.providers?.[node.provider] ? '請先到設定的「多代理與 API 工具」新增 API 供應商。'
              : pending ? '正在向供應商載入模型清單…'
                : request?.error ? request.error + '；可按「更新清單」重試。'
                  : models.length ? '依此供應商的 API 清單選擇；使用獨立 API 額度。' : '供應商未回傳可用模型，請確認金鑰與 API 網址後重試。';
          } else if (node.backend === 'local') {
            const local = api.getLocal();
            models = (local.models || []).map(m => ({ value: m.name, label: m.name }));
            status = models.length ? '只列出已安裝的 Ollama 模型，維持唯讀審查。'
              : '尚無可用的本地模型，請確認 Ollama 已啟動並安裝模型，再更新清單。';
            if (!localRequested) { localRequested = true; send({ type: 'local.status' }); }
          } else if (node.backend === 'gemini') {
            status = '使用官方 CLI 預設模型。可審查並修改檔案；需確認的工具會拒絕執行，無介面模式無法代答權限卡。唯讀規劃模式不啟動 Gemini。';
          } else {
            models = (api.getModels()[node.backend] || []).map(m => ({ value: m.value, label: m.displayName || m.value, description: m.description }));
            status = models.length ? '使用已登入的 CLI 模型清單。' : '尚未取得模型清單，可使用 CLI 預設模型，或更新清單。';
          }
          models = models.filter((m, i, all) => m.value && (!cli || m.value !== 'default') && all.findIndex(x => x.value === m.value) === i);
          if (cli && node.model === 'default') node.model = '';
          if (!cli && !node.model && models.length) node.model = (models.find(m => m.isDefault) || models[0]).value;
          const available = models.some(m => m.value === node.model);
          binding.valid = (cli && !node.model) || available;
          model.replaceChildren(...(cli ? [h('option', { value: '' }, '使用 CLI 預設模型')] : []),
            ...models.map(m => h('option', { value: m.value, title: m.description || '' }, m.label)));
          if (node.model && !available) {
            model.prepend(h('option', { value: node.model, disabled: true }, node.model + '（目前清單未提供）'));
            status += ' 原模型保留顯示，請選擇目前可用的模型後儲存。';
          } else if (!models.length && !cli) model.append(h('option', { value: '' }, pending ? '正在載入模型…' : '尚無可選模型'));
          model.value = node.model || ''; model.disabled = !models.length && !cli;
          retry.disabled = node.backend === 'api' && (!state.providers?.[node.provider] || pending);
          hint.textContent = status;
        } };
        model.onchange = () => { node.model = model.value; delete node.effort; refresh(); };
        provider.onchange = () => { node.provider = provider.value; node.model = ''; refresh(); };
        bindings.push(binding);
        const focus = h('select', { class: 'select', 'aria-label': '審查關注點' }, h('option', { value: '' }, '一般審查'),
          (api.getProfiles?.() || []).map(p => h('option', { value: p.id }, p.name)));
        if (node.focus && !(api.getProfiles?.() || []).some(p => p.id === node.focus))
          focus.append(h('option', { value: node.focus }, node.focus + '（已移除，請重選）'));
        focus.value = node.focus || ''; focus.disabled = node.role === 'write';
        focus.onchange = () => { node.focus = focus.value; };
        const maxTokens = input(node.maxTokens || 16000, 'number'); maxTokens.onchange = () => { node.maxTokens = Number(maxTokens.value); };
        const maxUsd = input(node.maxUsd || 1, 'number'); maxUsd.step = 0.1; maxUsd.onchange = () => { node.maxUsd = Number(maxUsd.value); };
        const vision = h('input', { type: 'checkbox', checked: !!node.vision }); vision.onchange = () => { node.vision = vision.checked; };
        list.append(h('div', { class: 'card pipeline-node' }, h('div', { class: 'pipeline-order' }, String(index + 1),
          button('上移', () => { if (index) { [nodes[index - 1], nodes[index]] = [nodes[index], nodes[index - 1]]; draw(); } }),
          button('下移', () => { if (index + 1 < nodes.length) { [nodes[index + 1], nodes[index]] = [nodes[index], nodes[index + 1]]; draw(); } }),
          button('移除', () => { nodes.splice(index, 1); draw(); }, 'ghost')),
        field('名稱', name), field('後端', kind), field('角色', role),
        node.backend === 'api' ? field('供應商', provider) : null,
        field('模型', h('div', { class: 'pipeline-model-choice' }, model, retry)), hint, field('審查關注點', focus),
        node.backend === 'api' ? [field('Token 上限', maxTokens), field('費用上限（USD，依自填費率）', maxUsd),
          h('label', { class: 'check-row' }, vision, '模型支援讀取測試圖片（圖片會傳送至此供應商）')] : null));
      });
      refresh();
    };
    draw();
    refreshEditor = refresh;
    modal({ title: '多代理流程', wide: true,
      desc: '每位代理有獨立模型與會話；本地模型維持唯讀。流程變更會套用到下一個任務。',
      onClose: () => { if (refreshEditor === refresh) refreshEditor = null; },
      body: [list, button('新增 Gemini 審查者', () => {
        nodes.push({ id: 'gemini-' + Date.now(), name: 'Gemini', backend: 'gemini', role: 'review', model: '' });
        routing.value = 'full'; draw();
      }), button('新增代理', () => { const id = 'agent-' + Date.now(); nodes.push({ id, name: '新代理', backend: 'codex', role: 'review', model: '' }); draw(); }),
        field('完整循環上限', cycles), field('驗收標準', acceptance),
        h('label', { class: 'check' }, requireTests, '完成前必須有可確認結束碼的測試'),
        field('協作路由', routing)],
      foot: close => {
        save = button('儲存流程', () => {
          if (!canSave()) return;
          send({ type: 'pipeline.set', id: c.id, pipeline: { nodes, maxCycles: Number(cycles.value), acceptance: acceptance.value,
            requireTests: requireTests.checked, routing: routing.value } }); close();
        }, 'primary');
        save.disabled = !canSave();
        return [button('沿用原本雙方協作', () => { send({ type: 'pipeline.set', id: c.id, pipeline: {} }); close(); }, 'ghost'), save];
      },
    });
    send({ type: 'platform.get' });
  }

  function picture(artifact) {
    const { h, modal, mediaUrl } = api;
    return h('button', { class: 'task-picture', 'aria-label': '放大測試圖片', onclick: () => modal({ title: artifact.operation || '測試圖片', wide: true,
      body: [h('img', { class: 'task-picture-full', src: mediaUrl(artifact.file), alt: artifact.operation || '測試截圖' }),
        h('p', { class: 'hint' }, new Date(artifact.ts).toLocaleString('zh-TW') + ' · ' + (artifact.result || 'unknown') + ' · ' + (artifact.source || ''))] }) },
    h('img', { src: mediaUrl(artifact.file), alt: artifact.operation || '測試截圖', loading: 'lazy' }));
  }

  function controls(c, task) {
    const { h, send } = api;
    // Without a pipeline the setting lives in the channel settings panel; nothing to show here.
    if (!c.pipeline?.nodes?.length && !task?.pipeline?.nodes && !task?.artifacts?.length) return null;
    return h('div', { class: 'panel-section' }, h('div', { class: 'card' },
      h('div', { class: 'card-title' }, '多代理流程', button('設定', () => editor(c))),
      h('div', { class: 'hint' }, c.pipeline?.nodes?.length ? c.pipeline.nodes.map(n => n.name || n.id).join(' → ') : '目前使用原本的雙方協作'),
      task?.pipeline?.nodes ? [h('div', { class: 'kv' }, h('span', {}, '審查閘門'), h('span', {}, task.gate || '工作中')),
        task.blocked_reason ? h('div', { class: 'hint' }, task.blocked_reason) : null,
        task.conflict_reference ? h('div', { class: 'hint mono' }, task.conflict_reference) : null,
        h('div', { class: 'hint' }, '循環 ' + task.cycle + ' · ' + (task.pipeline.nodes[task.cursor]?.name || task.pipeline.nodes[task.cursor]?.id || '')),
        h('div', { class: 'hint' }, 'API Token：' + ((task.costs?.inputTokens || 0) + (task.costs?.outputTokens || 0)) +
          (task.costs?.costKnown ? ' · 估算 US$' + Number(task.costs.costUsd || 0).toFixed(4) : ' · 金額費率未設定')),
        button('讀取完整交接', () => send({ type: 'task.history', id: c.id, after: 0 }))] : null,
      task?.pipeline?.nodes ? h('div', { class: 'pipeline-order' },
        button('暫停', () => send({ type: 'task.pause', id: c.id })),
        button('繼續', () => send({ type: 'group.continue', id: c.id })),
        button('取消任務', () => send({ type: 'conv.stop', id: c.id }), 'ghost'),
        button('重新審查', () => send({ type: 'task.rereview', id: c.id }))) : null,
      task?.artifacts?.length ? h('div', { class: 'task-pictures' }, task.artifacts.map(picture)) : null));
  }

  function providersPage(box) {
    const { h, send, modal } = api;
    const providerForm = () => {
      const id = input(), name = input(), url = input('https://api.openai.com/v1'), key = input('', 'password');
      const protocol = h('select', { class: 'select' }, h('option', { value: 'responses' }, 'Responses API'), h('option', { value: 'chat' }, 'Chat Completions 相容 API'));
      const inRate = input('', 'number'), outRate = input('', 'number');
      modal({ title: '新增 API 供應商', body: [field('識別碼', id), field('名稱', name), field('API 網址（含 /v1）', url), field('協定', protocol), field('API Key', key),
        field('每百萬輸入 Token 的 USD 費率', inRate), field('每百萬輸出 Token 的 USD 費率', outRate),
        h('p', { class: 'hint' }, 'API 由供應商獨立計費，與 Claude／ChatGPT 訂閱不同。金鑰使用 Windows 加密保存；未填費率時只能限制 Token，不能估計金額。')],
      foot: close => [button('取消', close, 'ghost'), button('加密儲存', () => {
        send({ type: 'provider.save', provider: { id: id.value, name: name.value, url: url.value, protocol: protocol.value,
          inputRate: Number(inRate.value), outputRate: Number(outRate.value) }, apiKey: key.value }); key.value = ''; close();
      }, 'primary')] });
    };
    const serverForm = () => {
      const id = input(), command = input(), args = input('[]'), tools = input('[]');
      const approved = h('input', { type: 'checkbox' });
      modal({ title: 'MCP 工具伺服器', body: [field('識別碼', id), field('原生執行檔（不會自動安裝）', command), field('參數 JSON 陣列', args), field('核准的工具名稱 JSON 陣列', tools),
        h('p', { class: 'hint' }, '這是有宿主機權限的外部程序。請先確認來源可信，並只核准需要的工具；不繼承 API Key 環境變數。既有 Claude／Codex 的 MCP 仍由插件頁管理。'),
        h('label', { class: 'check' }, approved, '我已確認來源及允許的工具，核准啟動')],
      foot: close => [button('儲存並啟用', () => {
        try { send({ type: 'toolserver.save', server: { id: id.value, command: command.value, args: JSON.parse(args.value), allowedTools: JSON.parse(tools.value), enabled: true }, confirm: approved.checked }); close(); }
        catch (_) { api.toast('error', '參數及工具清單必須是 JSON 陣列'); }
      }, 'primary')] });
    };
    box.append(h('h2', {}, '模型供應商與隔離工具'), h('p', { class: 'lead' }, 'Claude Code／Codex 沿用原本登入。API 供應商使用獨立金鑰與預算。'), button('新增 API 供應商', providerForm));
    for (const p of Object.values(state.providers || {})) box.append(h('div', { class: 'card' }, h('div', { class: 'card-title' }, p.name || p.id),
      h('div', { class: 'mono hint' }, p.url + ' · ' + p.protocol + ' · API 計費'),
      button('連線與模型清單', () => send({ type: 'provider.models', id: p.id })),
      button('移除', () => send({ type: 'provider.remove', id: p.id }), 'ghost'),
      p.models?.length ? h('div', { class: 'hint' }, p.models.map(m => m.id).join('、')) : null));
    box.append(h('h3', {}, 'API 代理的 MCP 工具'), button('新增工具伺服器', serverForm));
    for (const s of state.servers || []) box.append(h('div', { class: 'card' }, s.id + (s.enabled ? ' · 已啟用' : ' · 已停用'),
      h('div', { class: 'hint', 'data-pin-summary': s.id }, '已釘住 ' + Object.keys(s.pins || {}).length + '／' + (s.allowedTools || []).length +
        ' 個核准的工具；未釘住的工具不會放行。'),
      button('連線與工具發現', () => send({ type: 'toolserver.test', id: s.id })),
      button(s.enabled ? '停用' : '啟用', () => send({ type: 'toolserver.save', server: { ...s, enabled: !s.enabled }, confirm: true })),
      button('移除', () => send({ type: 'toolserver.remove', id: s.id }), 'ghost')));
    box.append(h('h3', {}, '沙箱'), h('p', { class: 'hint' }, state.sandbox?.ok ? 'Docker 隔離後端可用。網路預設關閉，不掛載宿主工作區或憑證。' : (state.sandbox?.error || '尚未檢查隔離後端。')),
      button('檢查 Docker', () => send({ type: 'sandbox.status' })),
      h('p', { class: 'hint' }, '只使用已存在的映像，不自動安裝或下載。缺少後端時會阻擋執行；正式隔離測試需先有可用的 Docker。'));
  }
  return { setup, update, result, refreshModels, controls, providersPage, picture, edit: editor };
})();
