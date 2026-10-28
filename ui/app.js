'use strict';
(() => {
  // ================================================================ helpers
  const PROPS = new Set(['value', 'checked', 'disabled', 'selected', 'placeholder', 'rows', 'type', 'tabIndex',
    'spellcheck', 'autofocus', 'min', 'max', 'step', 'src', 'alt']);

  function h(tag, props, ...kids) {
    const el = document.createElement(tag);
    if (props) {
      for (const [k, v] of Object.entries(props)) {
        if (v == null || v === false) continue;
        if (k === 'class') el.className = v;
        else if (k === 'style') Object.assign(el.style, v);
        else if (k === 'text') el.textContent = v;
        else if (k === 'html') el.innerHTML = v;  // only ever given MD.render() output or static markup
        else if (k.startsWith('on')) el.addEventListener(k.slice(2).toLowerCase(), v);
        else if (PROPS.has(k)) el[k] = v;
        else el.setAttribute(k, v === true ? '' : v);
      }
    }
    for (const kid of kids.flat(Infinity)) {
      if (kid == null || kid === false || kid === '') continue;
      el.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
    }
    return el;
  }
  const $ = (s, r = document) => r.querySelector(s);
  const clear = (el) => { while (el.firstChild) el.removeChild(el.firstChild); return el; };
  // Replaces el's children, skipping null/false like h() does (Element.append would print "null").
  function put(el, ...kids) {
    clear(el);
    for (const k of kids.flat(Infinity)) if (k != null && k !== false && k !== '') el.append(k);
    return el;
  }

  const timeFmt = new Intl.DateTimeFormat('zh-TW', { hour: 'numeric', minute: '2-digit' });
  const dayFmt = new Intl.DateTimeFormat('zh-TW', { year: 'numeric', month: 'long', day: 'numeric' });
  const dateFmt = new Intl.DateTimeFormat('zh-TW', { year: 'numeric', month: '2-digit', day: '2-digit' });
  function stamp(ts) {
    const d = new Date(ts), now = new Date();
    const day = (x) => new Date(x.getFullYear(), x.getMonth(), x.getDate()).getTime();
    const diff = (day(now) - day(d)) / 86400000;
    if (diff === 0) return '今天 ' + timeFmt.format(d);
    if (diff === 1) return '昨天 ' + timeFmt.format(d);
    return dateFmt.format(d) + ' ' + timeFmt.format(d);
  }
  const sameDay = (a, b) => new Date(a).toDateString() === new Date(b).toDateString();
  function dur(ms) {
    if (!ms || ms < 0) return '';
    if (ms < 1000) return ms + ' 毫秒';
    const s = ms / 1000;
    if (s < 60) return s.toFixed(s < 10 ? 1 : 0) + ' 秒';
    const m = Math.floor(s / 60);
    return m + ' 分 ' + Math.round(s % 60) + ' 秒';
  }
  function tok(n) {
    n = Number(n) || 0;
    if (n >= 1e6) return (n / 1e6).toFixed(1) + 'M';
    if (n >= 1e3) return (n / 1e3).toFixed(n >= 1e4 ? 0 : 1) + 'k';
    return String(n);
  }
  function untilReset(sec) {
    if (!sec) return '';
    const ms = sec * 1000 - Date.now();
    if (ms <= 0) return '即將重置';
    const m = Math.round(ms / 60000);
    if (m < 60) return m + ' 分鐘後重置';
    const hrs = Math.floor(m / 60);
    if (hrs < 48) return hrs + ' 小時 ' + (m % 60) + ' 分後重置';
    return Math.round(hrs / 24) + ' 天後重置';
  }
  const baseName = (p) => (p || '').replace(/[\\/]+$/, '').split(/[\\/]/).pop() || p;
  function hashColor(s) {
    const colors = ['#5865f2', '#3ba55c', '#faa61a', '#ed4245', '#eb459e', '#4f7bd8', '#9b84ec', '#e67e22', '#1abc9c'];
    let x = 0;
    for (const c of s || '') x = (x * 31 + c.charCodeAt(0)) >>> 0;
    return colors[x % colors.length];
  }
  const store = {
    get(k, d) { try { const v = localStorage.getItem('cca.' + k); return v == null ? d : JSON.parse(v); } catch { return d; } },
    set(k, v) { try { localStorage.setItem('cca.' + k, JSON.stringify(v)); return true; } catch { return false; } },
  };

  // ================================================================ constants
  const AGENTS = {
    gemini: { name: 'Gemini', letter: 'G', vendor: 'Google', cli: 'Gemini CLI', role: '審查者', color: 'var(--gemini)',
      upgrade: 'https://geminicli.com/plans/', notice: 'https://geminicli.com/docs/resources/quota-and-pricing/',
      install: 'npm install -g @google/gemini-cli', installNote: '需要先安裝 Node.js，然後執行：' },
    claude: { name: 'Claude', letter: 'C', vendor: 'Anthropic', cli: 'Claude Code', role: '撰寫者', color: 'var(--claude)',
      upgrade: 'https://claude.ai/upgrade', install: 'irm https://claude.ai/install.ps1 | iex', installNote: '在 PowerShell 執行（官方安裝程式）：' },
    codex: { name: 'GPT', letter: 'G', vendor: 'OpenAI', cli: 'Codex', role: '審查者', color: 'var(--codex)',
      upgrade: 'https://chatgpt.com/#pricing', install: 'npm install -g @openai/codex', installNote: '需要先安裝 Node.js，然後執行：' },
    local1: { name: '本地模型 1', letter: 'L', vendor: '本機', cli: 'Ollama', role: '唯讀審查員', color: 'var(--local1)' },
    local2: { name: '本地模型 2', letter: 'L', vendor: '本機', cli: 'Ollama', role: '唯讀審查員', color: 'var(--local2)' },
  };
  const PROFILE_AGENTS = ['claude', 'codex', 'gemini', 'local1', 'local2'];
  const isLocalAgent = (a) => a === 'local1' || a === 'local2';
  const PERMS = {
    plan: { label: '規劃模式', icon: 'eye', desc: '只閱讀和分析，不修改檔案，先提出計畫給你核准。',
      claude: 'Claude：plan', codex: 'GPT：唯讀沙箱', gemini: 'Gemini：此模式不啟動，避免放寬唯讀限制' },
    ask: { label: '每次詢問', icon: 'shield', desc: '修改檔案、執行指令之前都會先問你。',
      claude: 'Claude：default（手動確認）', codex: 'GPT：唯讀沙箱＋需要時詢問', gemini: 'Gemini：default，需要互動確認的工具會被拒絕' },
    edit: { label: '自動編輯', icon: 'pencil', desc: '專案資料夾內的修改自動允許；其他指令和網路存取仍會先問你。',
      claude: 'Claude：acceptEdits', codex: 'GPT：工作區可寫＋需要時詢問', gemini: 'Gemini：auto_edit，需要互動確認的工具會被拒絕' },
    bypass: { label: '跳過所有確認', icon: 'bolt', danger: true, desc: '完全不詢問。AI 可以直接改任何檔案、執行任何指令、連上網路。',
      claude: 'Claude：bypassPermissions', codex: 'GPT：完全存取，不詢問', gemini: 'Gemini：yolo，不詢問' },
  };
  const PHASES = { selfcheck: '自查', write: '撰寫', review: '審查', recheck: '複審', verify: '驗證', advise: '唯讀審查' };
  const PHASE_SHORT = { ...PHASES, advise: '唯讀' };  // narrow columns (the timeline)
  const TOOL_ICONS = { command: 'terminal', read: 'file', search: 'search', edit: 'pencil', web: 'globe', mcp: 'puzzle',
    task: 'users', tool: 'wrench' };
  // Used until Claude Code reports its own list (it does after the first sign-in check).
  const FALLBACK_CLAUDE_MODELS = [
    { value: '', displayName: '預設', description: 'Claude Code 的預設模型', supportedEffortLevels: ['low', 'medium', 'high', 'xhigh', 'max'] },
    { value: 'opus', displayName: 'Opus', description: '最強的程式能力', supportedEffortLevels: ['low', 'medium', 'high', 'xhigh', 'max'] },
    { value: 'sonnet', displayName: 'Sonnet', description: '速度與能力平衡', supportedEffortLevels: ['low', 'medium', 'high'] },
    { value: 'haiku', displayName: 'Haiku', description: '最快、最省額度' },
  ];
  // Thinking levels, named the way Claude Code and Codex name them.
  const EFFORTS = {
    minimal: ['Minimal', '幾乎不思考，最快'],
    low: ['Low', '快速回應，思考較少'],
    medium: ['Medium', '速度和思考深度平衡'],
    high: ['High', '想得更深，適合複雜問題'],
    xhigh: ['XHigh', '非常深入的思考'],
    max: ['Max', '最大思考深度'],
    ultra: ['Ultra', '最大思考深度，並自動把工作分派給子代理（用量最高）'],
    ultracode: ['Ultracode', '在 XHigh 之上開啟動態工作流程，自動分派子代理處理大型任務（用量最高）'],
  };
  // Ultracode is Claude Code's `--effort ultracode`: offered where the model reaches XHigh.
  function effortsFor(agent, info) {
    const list = ((info && info.supportedEffortLevels) || []).slice();
    if (agent === 'claude' && (list.includes('xhigh') || list.includes('max')) && !list.includes('ultracode')) list.push('ultracode');
    return list;
  }

  // ================================================================ state
  const S = {
    boot: false,
    review: null,       // the open review pane (renderReview)
    reviewNotes: {},    // conversation -> line comments not yet sent
    reviewDrafts: {},
    reviewSending: {},
    reviewSaveFailed: {},
    reviewRequest: 0,
    inbox: { items: [], total: 0 },
    inboxJump: null,
    quality: {},
    user: { name: '使用者' },
    auth: { claude: { state: 'checking' }, codex: { state: 'checking' }, gemini: { state: 'checking' }, ok: false },
    convs: [],
    groupOrder: [],
    lastDm: '',
    railDrag: null,
    railReorder: null,
    railFocus: '',
    states: {},
    msgs: {},
    loaded: {},
    unread: {},
    view: 'chat',
    current: null,
    models: { claude: [], codex: [], gemini: [] },
    modelDates: [],
    modelDateAliases: {},
    crop: null,
    groupEditor: null,
    usage: { claude: {}, codex: {}, gemini: {} },
    settings: {},
    plugins: { claude: null, codex: null },
    mcp: { claude: null, codex: null },
    busy: {},
    pluginTab: 'claude',
    pluginQuery: '',
    pluginLimit: 60,
    panelOpen: store.get('panelOpen', true),
    panelDrawer: false,
    winMax: false,
    expanded: new Set(),
    fold: new Map(),  // activity groups and long text: key -> open?, overrides the default
    feedback: {},
    answers: {},
    drafts: {},
    pending: {},
    version: '',
    personas: [],
    local: null,
    pulls: {},
    usageTable: false,
    activityMode: 'daily',
  };

  // ================================================================ bridge
  const native = window.MockBackend ? null : window.chrome && window.chrome.webview;
  function send(msg) {
    if (native) native.postMessage(msg);
    else if (window.MockBackend) window.MockBackend.receive(msg);
  }
  const desk = window.CCADesk.create({ S, h, put, icon, send, modal, toast, schedule, conv: () => conv(),
    onLayout: key => { S.panelOpen = key !== 'reading'; S.panelDrawer = false; schedule(); } });
  const appearance = window.CCAAppearance.create({ S, h, send, toast, schedule, renderSettings });
  const extensions = window.CCAExtensions.create({ S, h, send, modal, toast, schedule, renderSettings,
    onThemes: () => { if (!S.appearance.preview) appearance.apply(S.settings.theme || 'cca'); } });
  // A special edition is its own product line: some ordinary features are absent there.
  const special = () => !!S.edition?.id;
  const git = window.CCAGit.create({ S, h, put, icon, send, toast, modal, conv: () => conv(), onChange: () => renderPanel() });
  let reqSeq = 0;
  const waiters = {};
  function request(msg) {
    const reqId = 'r' + (++reqSeq);
    return new Promise((resolve) => { waiters[reqId] = resolve; send({ ...msg, reqId }); });
  }

  // ---- profiles: names, avatars and banners shared by all native participants
  const prof = (who) => ((S.settings && S.settings.profiles) || {})[who] || {};
  function displayName(who) {
    const n = (prof(who).name || '').trim();
    if (n) return n;
    if (who === 'user') return S.user.name;
    if (isLocalAgent(who) && personaOf(who)) return personaOf(who).name;
    return AGENTS[who] ? AGENTS[who].name : who;
  }
  const convTitle = (c) => (c.kind === 'group' ? c.name : displayName(c.kind));
  function mediaUrl(file) {
    if (!file) return '';
    if (/^(data:image\/|https?:)/.test(file)) return file;
    return 'https://media.cca.app/' + encodeURIComponent(file);
  }
  const defaultBanner = (who) => (window.CCAThemeCatalog?.find(t => t.id === (S.settings.theme || 'cca'))?.tokens || {})[who] || '#5865f2';
  function bannerStyle(who) {
    const p = prof(who);
    if (p.banner) return { backgroundImage: `url("${mediaUrl(p.banner)}")`, backgroundSize: 'cover', backgroundPosition: 'center' };
    return { background: p.bannerColor || defaultBanner(who) };
  }
  const personaOf = (agent) => {
    const id = ((S.settings && S.settings.persona) || {})[agent] || 'none';
    return S.personas.find((p) => p.id === id) || null;
  };

  const conv = (id = S.current) => S.convs.find((c) => c.id === id);
  const stateOf = (id = S.current) => S.states[id] || { typing: [], group: {} };
  const running = (id = S.current) => !!stateOf(id).running;

  function pendingCount(id) {
    let n = 0;
    for (const m of S.msgs[id] || []) {
      for (const b of m.blocks || []) if ((b.type === 'approval' || b.type === 'question') && b.state === 'pending') n++;
    }
    return n;
  }
  const totalPending = () => S.convs.reduce((a, c) => a + pendingCount(c.id), 0);

  function modelInfo(agent, value) {
    const list = S.models[agent] && S.models[agent].length ? S.models[agent] : (agent === 'claude' ? FALLBACK_CLAUDE_MODELS : []);
    const v = value || '';
    return list.find((m) => (m.value || '') === v || (v === '' && (m.value === 'default' || m.isDefault))) || null;
  }
  function modelLabel(agent, value) {
    const m = modelInfo(agent, value);
    if (m) return m.displayName || m.value || '預設';
    return value || '預設';
  }

  // ================================================================ incoming
  function onMessage(m) {
    if (appearance.message(m)) return;
    if (extensions.message(m)) return;
    if (git.message(m)) return;
    if (desk.message(m)) {
      if (m.type === 'layout.result' && settingsUi.open) renderSettings();
      return;
    }
    if (m.type === 'update.state') { if (settingsUi.open && settingsUi.section === 'updates') renderSettings(); schedule(); return; }
    switch (m.type) {
      case 'inbox':
        S.inbox = m;
        schedule();
        break;
      case 'platform':
        window.CCAPlatform?.update(m);
        if (settingsUi.open && settingsUi.section === 'platform') renderSettings();
        break;
      case 'platform.result':
        if (!window.CCAPlatform?.result(m)) {
          toast(m.result.ok ? 'ok' : 'error', m.result.ok ? '管理操作完成' : m.result.error || '操作失敗');
          if (m.result.tools) modal({ title: 'MCP 已發現的工具', body: h('pre', {}, JSON.stringify(m.result.tools, null, 2)) });
        }
        break;
      case 'task.history':
        modal({ title: '結構化交接紀錄', wide: true, body: h('pre', { class: 'task-record' }, JSON.stringify(m.items, null, 2)),
          foot: close => [h('button', { class: 'btn secondary', onclick: () => { close(); send({ type: 'task.history', id: m.id, after: m.items.at(-1)?.seq || 0 }); } }, '下一頁')] });
        break;
      case 'boot': {
        S.boot = true;
        S.user = m.user || S.user;
        S.version = m.version || '';
        S.convs = m.convs || [];
        S.groupOrder = m.groupOrder || [];
        S.settings = m.settings || {};
        S.lastDm = S.settings.lastDm || '';
        window.CCABadges?.update(m.badges);
        S.personas = m.personas || [];
        S.models = m.models || S.models;
        window.CCAPlatform?.refreshModels();
        S.modelDates = m.modelDates || [];
        S.modelDateAliases = m.modelDateAliases || {};
        S.usage = m.usage || S.usage;
        for (const st of m.states || []) S.states[st.id] = st;
        const last = S.settings.lastConv;
        openConv(S.convs.some((c) => c.id === last) ? last : (S.convs[0] && S.convs[0].id));
        break;
      }
      case 'auth':
        S.auth = m;
        renderGate();
        schedule();
        break;
      case 'convs':
        S.convs = m.items || [];
        S.groupOrder = m.groupOrder || S.groupOrder;
        S.groupEditor?.refresh();
        if (!conv()) openHome();
        schedule();
        break;
      case 'groups.order.result':
        if (S.railReorder?.token === m.requestToken) {
          S.railFocus = 'group:' + S.railReorder.focus;
          S.railReorder = null;
          toast(m.ok ? 'ok' : 'error', m.ok ? '群組順序已儲存' : m.error || '群組排序未能儲存');
          schedule();
        }
        break;
      case 'conv.created':
        openConv(m.id);
        break;
      case 'history':
        // History is independent of the verification panel's observations.
        S.msgs[m.id] = m.messages || [];
        S.loaded[m.id] = true;
        if (m.id === S.current && S.view === 'chat') renderMessages(true);
        if (S.inboxJump?.convId === m.id && m.id === S.current) { jumpToMsg(S.inboxJump.id); S.inboxJump = null; }
        schedule();
        break;
      case 'msg':
        upsertMsg(m.convId, m.msg);
        break;
      case 'review':
        if (S.review && S.review.convId === m.id && S.review.scope === m.scope && S.review.requestToken === m.requestToken) {
          Object.assign(S.review, { loading: false, error: m.error || '', files: m.files || [], base: m.base || '', head: m.head || '', truncated: !!m.truncated, reverting: false });
          renderReview();
        }
        break;
      case 'review.reverted':
        if (S.review && S.review.convId === m.id && S.review.revertToken === m.requestToken) {
          S.review.reverting = false;
          if (m.ok) { toast('ok', '已還原。聊天裡的系統訊息可以復原這次還原。'); openReview(conv(m.id), S.review.scope); }
          else { toast('error', m.error || '無法還原'); renderReview(); }
        }
        break;
      case 'convState': {
        const was = !!((S.states[m.state.id] || {}).group || {}).paused;
        S.states[m.state.id] = m.state;
        window.CCACollaboration?.update();
        window.CCAQuality?.update();
        // The pause buttons depend on the group state, which can arrive after the pause message.
        if (m.state.id === S.current && S.view === 'chat' && chat.list && was !== !!(m.state.group || {}).paused) {
          const last = (S.msgs[S.current] || []).filter((x) => x.kind === 'paused').pop();
          if (last) rerenderMsg(last.id);
        }
        schedule();
        break;
      }
      case 'models':
        S.models = m.models;
        window.CCAPlatform?.refreshModels();
        schedule();
        break;
      case 'image.crop':
        cropDialog(m);
        break;
      case 'image.result':
        if (S.crop && S.crop.token === m.token) {
          S.crop.saving = false;
          if (m.ok) { S.crop.saved = true; S.crop.close(); }
          else { S.crop.error.textContent = m.error || '無法儲存頭像'; S.crop.save.disabled = false; S.crop.cancel.disabled = false; }
        }
        break;
      case 'local':
        S.local = m;
        window.CCAPlatform?.refreshModels();
        if (settingsUi.open) renderSettings();
        break;
      case 'local.pull':
        if (m.done) {
          delete S.pulls[m.model];
          if (m.error) toast('error', m.model + '：' + m.error);
        } else {
          S.pulls[m.model] = m;
        }
        if (settingsUi.open) renderSettings();
        break;
      case 'settings':
        // Names, avatars or personas changed: everything that shows them is redrawn.
        S.settings = m.settings || S.settings;
        S.groupOrder = m.groupOrder || S.groupOrder;
        window.CCABadges?.update(m.badges);
        S.personas = m.personas || S.personas;
        S.humanizeRules = m.humanizeRules || S.humanizeRules;
        if (S.view === 'chat' && chat.list) rerenderCurrent();
        if (settingsUi.open) renderSettings();
        schedule();
        break;
      case 'badges.result':
        window.CCABadges?.result(m);
        break;
      case 'quality.state':
        S.quality[m.id] = m;
        window.CCAQuality?.update();
        break;
      case 'usage':
        S.usage = m.usage;
        schedule();
        break;
      case 'plugins':
        S.plugins[m.agent] = m;
        schedule();
        break;
      case 'mcp':
        S.mcp[m.agent] = m;
        schedule();
        break;
      case 'busy':
        if (m.busy) S.busy[m.key] = true; else delete S.busy[m.key];
        schedule();
        break;
      case 'toast':
        toast(m.level, m.text);
        break;
      case 'folder.picked':
        if (waiters[m.reqId]) { waiters[m.reqId](m.path); delete waiters[m.reqId]; }
        break;
      case 'win.state':
        S.winMax = !!m.maximized;
        renderTitlebar();
        break;
    }
  }

  function upsertMsg(convId, msg) {
    const submitted = S.reviewSending[convId];
    if (submitted && msg.author === 'user' && msg.text === submitted.text) {
      S.reviewNotes[convId] = (S.reviewNotes[convId] || []).filter(n => !submitted.notes.includes(n));
      delete S.reviewSending[convId];
      saveReviewDraft(convId);
      if (S.review?.convId === convId) closeReview();
    }
    const list = S.msgs[convId] || (S.msgs[convId] = []);
    const i = list.findIndex((x) => x.id === msg.id);
    const isNew = i < 0;
    // A finished reply folds itself away again, whatever was opened or closed while it streamed.
    if (!isNew && list[i].streaming && !msg.streaming) clearFold(msg.id);
    if (isNew) list.push(msg); else list[i] = msg;
    if (convId !== S.current || S.view !== 'chat') {
      if (!msg.streaming && msg.author !== 'user') S.unread[convId] = true;
      schedule();
      return;
    }
    const atBottom = chat.stick;
    const el = chat.list && chat.list.querySelector(`[data-id="${CSS.escape(msg.id)}"]`);
    const idx = isNew ? list.length - 1 : i;
    const prev = idx > 0 ? list[idx - 1] : null;
    const fresh = renderMsg(msg, prev);
    if (el) {
      el.replaceWith(fresh);
    } else if (chat.list) {
      if (prev && !sameDay(prev.ts, msg.ts)) chat.list.append(dayDivider(msg.ts));
      chat.list.append(fresh);
    }
    if (atBottom || (isNew && msg.author === 'user')) scrollToBottom();
    schedule();
  }

  // ================================================================ navigation
  function openConv(id) {
    if (!id) return;
    closePop();
    S.view = 'chat';
    S.current = id;
    if (conv(id)?.kind !== 'group') S.lastDm = id;
    delete S.unread[id];
    send({ type: 'conv.open', id });
    chat.convId = null;  // force a fresh chat skeleton
    renderAll();
  }
  function openHome() {
    const dms = S.convs.filter(c => c.kind !== 'group');
    const home = dms.find(c => c.id === S.lastDm) || dms[0];
    if (home) openConv(home.id);
    else { S.current = null; openView('account'); }
  }
  function openView(view) {
    if (chat.workflow) { chat.workflow.destroy(); chat.workflow = null; }
    S.view = view;
    chat.convId = null;
    if (view === 'plugins') {
      if (!S.plugins.claude) send({ type: 'plugins.list', agent: 'claude' });
      if (!S.plugins.codex) send({ type: 'plugins.list', agent: 'codex' });
      if (!S.mcp.claude) send({ type: 'mcp.list' });
    }
    if (view === 'account') send({ type: 'usage.refresh' });
    if (view === 'inbox') send({ type: 'inbox.get' });
    renderAll();
  }

  // ================================================================ render scheduling
  let queued = false;
  function schedule() {
    if (queued) return;
    queued = true;
    requestAnimationFrame(() => { queued = false; renderAll(true); });
  }
  function renderAll(light) {
    renderTitlebar();
    renderRail();
    renderSidebar();
    renderUserbar();
    renderMain(light);
    renderPanel();
    desk.fit();
  }

  // ================================================================ title bar
  function renderTitlebar() {
    const tb = $('#titlebar');
    const c = conv();
    let title = 'ChatClaudeAgent', ic = 'logo';
    if (S.view === 'plugins') { title = '插件商店'; ic = 'puzzle'; }
    else if (S.view === 'account') { title = '訂閱與用量'; ic = 'chart'; }
    else if (c) { title = c.kind === 'group' ? convTitle(c) : '私人訊息'; ic = c.kind === 'group' ? 'users' : 'at'; }
    const pend = totalPending();
    put(tb,
      h('div', { class: 'tb-left' }, icon('logo', 18), 'ChatClaudeAgent',
        special() ? h('span', { class: 'edition-mark', 'data-tip': '為了您最艱難的任務而存在' }, S.edition.name) : null),
      h('div', { class: 'tb-title' }, icon(ic, 16), title),
      h('div', { class: 'tb-right' },
        h('button', { class: 'tb-icon', 'aria-label': '收件匣', 'data-tip': pend ? `${pend} 個請求等你處理` : '收件匣', onclick: () => openView('inbox') },
          icon('inbox', 18), pend ? h('span', { class: 'badge' }, pend) : null),
        h('button', { class: 'tb-icon', 'data-tip': '說明', onclick: showHelp }, icon('help', 18)),
        h('button', { class: 'win-btn', 'aria-label': '最小化', onclick: () => send({ type: 'win', action: 'min' }) }, icon('min', 16)),
        h('button', { class: 'win-btn', 'aria-label': '最大化', onclick: () => send({ type: 'win', action: 'max' }) }, icon(S.winMax ? 'restore' : 'max', 16)),
        h('button', { class: 'win-btn close', 'aria-label': '關閉', onclick: () => send({ type: 'win', action: 'close' }) }, icon('close', 16)),
      ),
    );
  }
  // Fallback for WebView2 builds without app-region support: let the host drag the window.
  document.addEventListener('mousedown', (e) => {
    if (e.button !== 0 || !native) return;
    if (e.target.closest('#titlebar') && !e.target.closest('button')) send({ type: 'win', action: 'drag' });
  });
  document.addEventListener('dblclick', (e) => {
    if (native && e.target.closest('#titlebar') && !e.target.closest('button')) send({ type: 'win', action: 'max' });
  });

  function jumpToPending() {
    for (const c of S.convs) {
      if (pendingCount(c.id)) {
        if (c.id !== S.current || S.view !== 'chat') openConv(c.id);
        setTimeout(() => {
          const el = chat.list && chat.list.querySelector('.embed.yellow');
          if (el) el.scrollIntoView({ block: 'center', behavior: 'smooth' });
        }, 80);
        return;
      }
    }
    toast('info', '目前沒有需要你處理的請求');
  }

  // ================================================================ rail
  const activeGroup = () => S.view === 'chat' && conv()?.kind === 'group' ? conv() : null;
  function orderedGroups() {
    const remaining = new Map(S.convs.filter(c => c.kind === 'group').map(c => [c.id, c]));
    const groups = [];
    for (const id of S.groupOrder) if (remaining.has(id)) { groups.push(remaining.get(id)); remaining.delete(id); }
    return groups.concat([...remaining.values()]);
  }
  function saveGroupOrder(order, focus) {
    if (S.railReorder) return;
    const current = orderedGroups().map(c => c.id);
    if (order.length !== current.length || order.every((id, index) => id === current[index])) return;
    const token = 'group-order-' + (++reqSeq);
    S.railReorder = { token, focus };
    send({ type: 'groups.reorder', order, requestToken: token });
    schedule();
  }
  function moveGroup(id, delta) {
    const order = orderedGroups().map(c => c.id), index = order.indexOf(id), next = index + delta;
    if (index < 0 || next < 0 || next >= order.length) return;
    [order[index], order[next]] = [order[next], order[index]];
    saveGroupOrder(order, id);
  }
  function groupMenu(anchor, c) {
    const groups = orderedGroups(), index = groups.findIndex(group => group.id === c.id);
    const action = (text, run, disabled = false, danger = false) => h('button', {
      class: 'group-menu-action' + (danger ? ' danger' : ''), type: 'button', disabled,
      onclick: () => { closePop(); run(); } }, text);
    popover(anchor, [h('div', { class: 'pop-title' }, c.name),
      action('開啟協作對話', () => openConv(c.id)),
      action('編輯群組資料', () => renameDialog(c)),
      action('群組設定', () => opsPanel(anchor, c)),
      h('div', { class: 'group-menu-separator' }),
      action('向上移動', () => moveGroup(c.id, -1), !!S.railReorder || index <= 0),
      action('向下移動', () => moveGroup(c.id, 1), !!S.railReorder || index === groups.length - 1),
      h('div', { class: 'group-menu-separator' }),
      action('建立協作群組', createGroupDialog),
      action('清除或刪除群組', () => clearDialog(c), false, true)], 'group-menu');
  }
  function railKey(event, id) {
    if (id && event.altKey && ['ArrowUp', 'ArrowDown'].includes(event.key)) {
      event.preventDefault();
      moveGroup(id, event.key === 'ArrowUp' ? -1 : 1);
      return;
    }
    if (id && (event.key === 'ContextMenu' || event.shiftKey && event.key === 'F10')) {
      event.preventDefault(); groupMenu(event.currentTarget, conv(id)); return;
    }
    if (!['ArrowUp', 'ArrowDown', 'Home', 'End'].includes(event.key)) return;
    const buttons = [...$('#rail').querySelectorAll('.rail-btn')], index = buttons.indexOf(event.currentTarget);
    const next = event.key === 'Home' ? 0 : event.key === 'End' ? buttons.length - 1 :
      Math.max(0, Math.min(buttons.length - 1, index + (event.key === 'ArrowUp' ? -1 : 1)));
    event.preventDefault(); buttons[next]?.focus(); buttons[next]?.scrollIntoView({ block: 'nearest' });
  }
  function groupRailButton(c) {
    const picture = c.avatar ? h('img', { src: mediaUrl(c.avatar), alt: '', draggable: 'false' }) :
      h('span', {}, Array.from(c.name.trim() || '群組').slice(0, 2).join(''));
    return h('button', { class: 'rail-btn rail-group', type: 'button', 'data-nav-id': 'group:' + c.id,
      'data-group-id': c.id, 'aria-label': c.name, 'aria-current': activeGroup()?.id === c.id ? 'page' : null,
      'aria-description': 'Alt 加上／下方向鍵調整位置；Shift 加 F10 開啟選項。', draggable: S.railReorder ? 'false' : 'true',
      onclick: () => openConv(c.id), onkeydown: event => railKey(event, c.id),
      oncontextmenu: event => { event.preventDefault(); groupMenu(event.currentTarget, c); },
      ondragstart: event => {
        if (S.railReorder) { event.preventDefault(); return; }
        S.railDrag = c.id;
        event.dataTransfer.setData('application/x-cca-group', c.id);
        event.dataTransfer.effectAllowed = 'move';
        closePop();
      },
      ondragover: event => {
        if (!S.railDrag || S.railDrag === c.id || S.railReorder) return;
        event.preventDefault(); event.dataTransfer.dropEffect = 'move';
        const before = event.clientY < event.currentTarget.getBoundingClientRect().top + 24;
        event.currentTarget.closest('.rail-item').classList.toggle('drop-before', before);
        event.currentTarget.closest('.rail-item').classList.toggle('drop-after', !before);
      },
      ondragleave: event => event.currentTarget.closest('.rail-item').classList.remove('drop-before', 'drop-after'),
      ondrop: event => {
        if (!S.railDrag || event.dataTransfer.getData('application/x-cca-group') !== S.railDrag) return;
        event.preventDefault(); event.stopPropagation();
        const source = S.railDrag;
        S.railDrag = null;
        if (source !== c.id) {
          const order = orderedGroups().map(group => group.id).filter(id => id !== source);
          const after = event.clientY >= event.currentTarget.getBoundingClientRect().top + 24;
          order.splice(order.indexOf(c.id) + (after ? 1 : 0), 0, source);
          saveGroupOrder(order, source);
        }
        schedule();
      },
      ondragend: () => { S.railDrag = null; schedule(); },
    }, picture);
  }
  function renderRail() {
    const rail = $('#rail');
    // Streaming redraws must not destroy the native element while a drag is in progress.
    if (S.railDrag) return;
    const scroll = rail.scrollTop, focus = S.railFocus || document.activeElement?.closest('#rail [data-nav-id]')?.dataset.navId;
    const groups = orderedGroups(), selected = activeGroup();
    const dms = S.convs.filter(c => c.kind !== 'group');
    const pend = dms.reduce((count, c) => count + pendingCount(c.id), 0);
    const item = (active, btn, tip, unread, count = 0) => h('div', {
      class: `rail-item${active ? ' active' : ''}${unread ? ' unread' : ''}`, 'data-tip': tip, 'data-tip-side': 'right' },
    h('div', { class: 'rail-pill' }), btn,
    count ? h('span', { class: 'badge rail-pending', 'aria-label': count + ' 項待處理' }, count > 99 ? '99+' : count) : null);
    put(rail,
      item(!selected, h('button', { class: 'rail-btn', type: 'button', 'data-nav-id': 'home',
        'aria-label': '私人訊息', 'aria-current': !selected ? 'page' : null, onclick: openHome, onkeydown: event => railKey(event) }, icon('logo', 28)),
      '私人訊息', dms.some(c => S.unread[c.id]), pend),
      h('div', { class: 'rail-sep' }),
      groups.map(c => item(selected?.id === c.id, groupRailButton(c), c.name, !!S.unread[c.id], pendingCount(c.id))),
      groups.length ? h('div', { class: 'rail-sep' }) : null,
      item(false, h('button', { class: 'rail-btn rail-create', type: 'button', 'data-nav-id': 'new',
        'aria-label': '建立協作群組', onclick: createGroupDialog, onkeydown: event => railKey(event) }, icon('plus', 24)), '建立協作群組'),
    );
    rail.scrollTop = scroll;
    S.railFocus = '';
    if (focus) rail.querySelector(`[data-nav-id="${CSS.escape(focus)}"]`)?.focus({ preventScroll: true });
  }

  // ================================================================ sidebar
  function avatar(kind, size = 32, status) {
    const p = kind === 'system' ? {} : prof(kind);
    const letter = kind === 'system' ? '' : (Array.from(displayName(kind))[0] || '?').toUpperCase();
    const style = { width: size + 'px', height: size + 'px', fontSize: Math.round(size * 0.42) + 'px' };
    if (kind !== 'system' && p.bannerColor && !p.avatar) style.background = p.bannerColor;
    const el = h('div', { class: `avatar ${kind}`, style },
      kind === 'system' ? icon('logo', Math.round(size * 0.6))
        : p.avatar ? h('img', { class: 'avatar-img', src: mediaUrl(p.avatar), alt: '', draggable: 'false' }) : letter);
    if (status) el.append(h('div', { class: `status ${status}` }));
    return el;
  }
  function agentStatus(agent, convId) {
    const a = S.auth[agent] || {};
    if (a.state !== 'ok') return a.state === 'checking' || a.state === 'loggingIn' ? 'wait' : 'bad';
    if (convId && (stateOf(convId).typing || []).includes(agent)) return 'busy';
    return 'ok';
  }

  function groupAvatar(c, size = 32) {
    return c.avatar ? h('div', { class: 'avatar group', style: { width: size + 'px', height: size + 'px' } },
      h('img', { class: 'avatar-img', src: mediaUrl(c.avatar), alt: '', draggable: 'false' }))
      : h('div', { class: 'group-avatar', style: { width: size + 'px', height: size + 'px' } },
        (c.pipeline?.nodes?.length ? [...new Set(c.pipeline.nodes.map(n => n.backend))].slice(0, 2) : ['claude', 'codex']).map(a => avatar(a, Math.round(size * 0.65))));
  }

  function currentProfileModel(who, c = conv()) {
    if (isLocalAgent(who)) return (((S.settings.locals || {})[who] || {}).model || '');
    if (!c) return '';
    const pick = c.daily && c.kind !== 'group' ? dailyPick(who).model : c[who + 'Model'] || '';
    const canonical = pick && pick !== 'default' && !S.modelDateAliases[pick];
    if (canonical) return pick;
    const reply = [...(S.msgs[c.id] || [])].reverse().find(m => m.author === who && m.resolvedModel &&
      (m.model || 'default') === (pick || 'default'));
    if (reply) return reply.resolvedModel;
    if (pick && pick !== 'default') return pick;
    const def = (S.models[who] || []).find(m => m.isDefault);
    if (def && def.value && def.value !== 'default') return def.value;
    const defaultEntry = (S.models[who] || []).find(m => m.value === 'default');
    const description = ((defaultEntry?.displayName || '') + ' ' + (defaultEntry?.description || '')).replace(/\s/g, '').toLowerCase();
    const known = S.modelDates.filter(e => description.includes(e.name.replace(/^Claude /, '').replace(/\s/g, '').toLowerCase()));
    return known.length === 1 ? known[0].id : '';
  }

  function profileFacts(who, c = conv()) {
    const persona = personaOf(who), model = currentProfileModel(who, c);
    const info = window.CCAProfileMedia.modelInfo(model, S.modelDates, S.modelDateAliases);
    const date = prof(who).joinedAt || info?.date || '';
    const about = persona ? (persona.summary || '使用「' + persona.name + '」性格，尚未填寫自介。')
      : isLocalAgent(who) ? '唯讀審查員，針對程式與變更提出建議。' : '協助撰寫、審查與執行專案工作。';
    return { persona, model, info, date, about };
  }

  // Profiles work like Discord's: clicking a member or an avatar opens a popout next to it;
  // the avatar, the name or "檢視完整個人資料" open the full profile.
  function profileDialog(who, convId = S.current, anchor = null) {
    if (!AGENTS[who] && who !== 'user') return;
    if (anchor) profilePopout(who, anchor, convId); else profileFull(who, convId);
  }

  function userAbout() {
    return PROFILE_AGENTS.map((a) => (personaOf(a) && personaOf(a).address ? displayName(a) + ' 叫你「' + personaOf(a).address + '」' : null))
      .filter(Boolean).join('，') || '直接叫你的名字';
  }

  function profileHandle(who) {
    if (who === 'user') return '你';
    const persona = personaOf(who);
    return (persona ? persona.name + ' · ' : '') + AGENTS[who].vendor + ' ' + AGENTS[who].cli;
  }

  function profileRoles(who, c) {
    if (who === 'user') return [{ name: c && c.kind === 'group' ? '下指令' : '使用者', color: 'var(--brand)' }];
    if (isLocalAgent(who)) return [{ name: '唯讀審查員', color: AGENTS[who].color }];
    if (c?.pipeline?.nodes?.some(n => n.backend === who)) return [...new Set(c.pipeline.nodes.filter(n => n.backend === who).map(n => PHASES[n.role] || n.role))].map(name => ({ name, color: AGENTS[who].color }));
    if (c && c.kind === 'group') return [{ name: (c.lead || 'claude') === who ? '主刀手' : '審查者', color: AGENTS[who].color }];
    return [{ name: AGENTS[who].role, color: AGENTS[who].color }];
  }

  const sharedGroups = (who) => S.convs.filter((g) => g.kind === 'group' &&
    (who === 'gemini' ? g.geminiMember || g.pipeline?.nodes?.some(n => n.backend === 'gemini') : !isLocalAgent(who) || (g.localMembers || []).includes(who)));
  const dmFor = (who) => (AGENTS[who] && !isLocalAgent(who) ? S.convs.find((x) => x.kind === who) : null);

  function profileSections(who, c, facts) {
    const section = (title, ...kids) => h('div', { class: 'up-section' }, h('div', { class: 'up-label' }, title), ...kids);
    const about = who === 'user' ? userAbout() : facts.about;
    return [
      about ? section(who === 'user' ? 'AI 怎麼稱呼你' : '關於我', h('div', { class: 'up-text' }, about)) : null,
      facts ? section('加入時間', h('div', { class: 'up-text' }, facts.date || '未收錄日期'),
        facts.info && !prof(who).joinedAt ? h('a', { href: facts.info.url, class: 'profile-source' }, '來源：' + facts.info.source) : null) : null,
      facts ? section('模型', h('div', { class: 'up-text mono' }, facts.model || '尚未回報模型')) : null,
      section('身分組', h('div', { class: 'up-roles' }, profileRoles(who, c).map((r) =>
        h('span', { class: 'up-role' }, h('i', { style: { background: r.color } }), r.name)))),
    ];
  }

  function profilePopout(who, anchor, convId = S.current) {
    const c = conv(convId), ai = who !== 'user';
    const facts = ai ? profileFacts(who, c) : null;
    const openFull = () => { closePop(); profileFull(who, convId); };
    const dm = dmFor(who);
    const input = dm ? h('input', { class: 'up-message', placeholder: '傳送訊息給 @' + displayName(who), 'aria-label': '傳送訊息給 ' + displayName(who) }) : null;
    if (input) {
      input.addEventListener('keydown', (e) => {
        if (e.key !== 'Enter' || e.isComposing || !input.value.trim()) return;
        const text = input.value.trim();
        closePop();
        openConv(dm.id);
        send({ type: 'conv.send', id: dm.id, text });
      });
    }
    const card = h('div', { class: 'popover user-popout', role: 'dialog', 'aria-label': displayName(who) + ' 的個人資料' },
      h('div', { class: 'up-banner', style: bannerStyle(who) },
        h('button', { class: 'up-icon', type: 'button', 'data-tip': '編輯個人資料', 'aria-label': '編輯個人資料',
          onclick: () => { closePop(); openSettings(who === 'user' ? 'me' : who); } }, icon('pencil', 16))),
      h('button', { class: 'up-avatar', type: 'button', 'aria-label': '檢視完整個人資料', onclick: openFull },
        avatar(who, 80, who === 'user' ? 'ok' : agentStatus(who, convId))),
      h('div', { class: 'up-body' },
        h('button', { class: 'up-name', type: 'button', onclick: openFull }, displayName(who)),
        h('div', { class: 'up-handle' }, profileHandle(who), ai ? h('span', { class: 'app-tag' }, 'APP') : null),
        window.CCABadges?.row(who),
        h('div', { class: 'up-inner' }, profileSections(who, c, facts)),
        h('button', { class: 'up-full', type: 'button', onclick: openFull }, '檢視完整個人資料'),
        input || (isLocalAgent(who) ? h('div', { class: 'up-note' }, '本地審查員只做唯讀審查，沒有私訊。') : null)));
    closePop();
    popEl = card;
    $('#layers').append(card);
    // Beside what was clicked: to the left of the member list, to the right of a message avatar.
    const r = anchor.getBoundingClientRect();
    const w = card.offsetWidth, hh = card.offsetHeight, gap = 12;
    const left = r.left + r.width / 2 > window.innerWidth / 2 ? r.left - w - gap : r.right + gap;
    card.style.left = Math.max(8, Math.min(window.innerWidth - w - 8, left)) + 'px';
    card.style.top = Math.max(8, Math.min(window.innerHeight - hh - 8, r.top)) + 'px';
  }

  function profileFull(who, convId = S.current) {
    const c = conv(convId), ai = who !== 'user';
    const facts = ai ? profileFacts(who, c) : null;
    const dm = dmFor(who);
    const groups = sharedGroups(who);
    let tab = 'about';
    const tabs = h('div', { class: 'fp-tabs', role: 'tablist' });
    const pane = h('div', { class: 'fp-pane' });
    let close = () => {};
    const render = () => {
      put(tabs, [['about', '關於我'], ['groups', '共同群組' + (groups.length ? ' ' + groups.length : '')]].map(([id, label]) =>
        h('button', { class: 'fp-tab' + (tab === id ? ' sel' : ''), type: 'button', role: 'tab', 'aria-selected': String(tab === id),
          onclick: () => { tab = id; render(); } }, label)));
      if (tab === 'about') put(pane, profileSections(who, c, facts));
      else put(pane, groups.length ? groups.map((g) => h('button', { class: 'fp-group', type: 'button', onclick: () => { close(); openConv(g.id); } },
        groupAvatar(g, 40), h('div', {}, h('div', { class: 'fp-group-name' }, g.name),
          h('div', { class: 'fp-group-sub' }, (g.cwd ? g.cwd.split(/[\\/]/).filter(Boolean).pop() + ' · ' : '') + (g.pipeline?.nodes?.length ? 1 + new Set(g.pipeline.nodes.map(n => n.backend)).size : 3 + (g.geminiMember ? 1 : 0) + (g.localMembers || []).length) + ' 位成員'))))
        : h('div', { class: 'up-note' }, '還沒有共同的群組。'));
    };
    render();
    close = modal({
      bare: true, cls: 'full-profile', label: displayName(who) + ' 的個人資料',
      body: [
        h('div', { class: 'fp-banner', style: bannerStyle(who) }),
        h('div', { class: 'fp-head' },
          h('div', { class: 'fp-avatar' }, avatar(who, 120, who === 'user' ? 'ok' : agentStatus(who, convId))),
          h('div', { class: 'fp-actions' },
            dm ? h('button', { class: 'btn primary', type: 'button', onclick: () => { close(); openConv(dm.id); } }, '傳送訊息') : null,
            h('button', { class: 'btn secondary', type: 'button', onclick: () => { close(); openSettings(who === 'user' ? 'me' : who); } },
              icon('pencil', 14), '編輯個人資料'))),
        h('div', { class: 'fp-id' }, h('div', { class: 'fp-name' }, displayName(who)),
          h('div', { class: 'up-handle' }, profileHandle(who), ai ? h('span', { class: 'app-tag' }, 'APP') : null),
          window.CCABadges?.row(who)),
        tabs, pane],
    });
  }

  function cropDialog(message) {
    if (S.crop) { send({ type: 'image.cancel', token: message.token }); return; }
    const body = h('div', { class: 'crop-editor' });
    const error = h('div', { class: 'crop-error', role: 'alert' });
    const save = h('button', { class: 'btn primary', disabled: true }, '儲存頭像');
    const cancel = h('button', { class: 'btn ghost' }, '取消');
    const state = { token: message.token, saved: false, saving: false, error, save, cancel };
    S.crop = state;
    let editor;
    const close = modal({
      title: '裁切頭像', desc: '調整位置與縮放，確認後才會替換原本的頭像。', body: [body, error],
      canClose: () => !state.saving,
      onClose: () => {
        editor?.destroy();
        if (!state.saved) send({ type: 'image.cancel', token: state.token });
        S.crop = null;
      },
      foot: () => [cancel, save],
    });
    state.close = close;
    cancel.onclick = close;
    editor = new window.CCAProfileMedia.Cropper(body, h, mediaUrl(message.file), () => { save.disabled = false; }, text => { error.textContent = text; });
    save.onclick = () => {
      try {
        const data = editor.png();
        if (!data) return;
        state.saving = true; save.disabled = cancel.disabled = true; error.textContent = '';
        send({ type: 'image.commit', token: state.token, data });
      } catch (_) { error.textContent = '裁切圖片無法匯出，請重新選擇圖片'; }
    };
  }

  function renderSidebar() {
    const side = $('#sidebar');
    const group = activeGroup();
    const context = group?.id || 'home';
    const scroll = side.dataset.context === context ? $('.side-list', side)?.scrollTop || 0 : 0;
    side.dataset.context = context;
    if (group) { renderGroupSidebar(side, group); $('.side-list', side).scrollTop = scroll; return; }
    const dms = S.convs.filter((c) => c.kind !== 'group');
    const pendAll = totalPending();
    const list = h('div', { class: 'side-list scroll-thin' },
      h('button', { class: `side-nav${S.view === 'account' ? ' active' : ''}`, onclick: () => openView('account') }, icon('chart', 22), '訂閱與用量',
        !S.auth.ok ? h('span', { class: 'badge' }, '!') : null),
      h('button', { class: `side-nav${S.view === 'plugins' ? ' active' : ''}`, onclick: () => openView('plugins') }, icon('puzzle', 22), '插件商店'),
      h('button', { class: 'side-nav', onclick: () => openView('inbox') }, icon('inbox', 22), '收件匣',
        S.inbox.total || pendAll ? h('span', { class: 'badge' }, S.inbox.total || pendAll) : null),
      h('div', { class: 'side-head' }, '私人訊息'),
      dms.map((c) => dmRow(c)),
    );
    put(side,
      h('div', { class: 'side-search' }, h('button', { onclick: quickSwitcher }, '尋找或開始對話')),
      list,
    );
    list.scrollTop = scroll;
  }
  function renderGroupSidebar(side, c) {
    const st = stateOf(c.id), g = st.group || {};
    const link = (name, symbol, run, selected = false) => h('button', {
      class: 'group-nav-link' + (selected ? ' active' : ''), type: 'button',
      'aria-current': selected ? 'page' : null, onclick: run }, icon(symbol, 19), h('span', {}, name));
    const tools = (event, module) => {
      const content = module?.controls(c);
      if (content) popover(event.currentTarget, [h('div', { onclick: click => {
        if (click.target.closest('button')) closePop();
      } }, content)], 'group-tools-menu');
    };
    put(side,
      h('button', { class: 'group-nav-heading', type: 'button', 'aria-label': c.name + ' 群組選項',
        onclick: event => groupMenu(event.currentTarget, c) }, h('strong', {}, c.name), icon('chevron', 16)),
      h('div', { class: 'side-list group-sidebar scroll-thin' },
        h('div', { class: 'group-nav-project', 'data-tip': c.cwd || '尚未選擇專案資料夾' }, icon('folder', 14),
          h('span', {}, c.cwd ? baseName(c.cwd) : '未設定專案')),
        h('div', { class: 'side-head' }, '對話'),
        link('協作對話', 'hash', () => openConv(c.id), true),
        h('div', { class: 'side-head' }, '工作區'),
        link('審查變更', 'diff', () => openReview(c, 'task')),
        link('版本控制', 'branch', () => openGit(c)),
        link('任務控制', 'list', event => tools(event, window.CCACollaboration)),
        link('交付驗證', 'check', event => tools(event, window.CCAQuality)),
        link('成員名單', 'users', () => {
          if (S.review?.convId === c.id) closeReview();
          if (!panelVisible()) togglePanel();
          $('#panel .member-head')?.scrollIntoView({ block: 'nearest' });
        }),
        h('div', { class: 'side-head' }, '群組'),
        link('群組設定', 'sliders', event => opsPanel(event.currentTarget, c)),
        h('div', { class: 'group-nav-status', role: 'status' }, h('i', { class: st.running ? 'busy' : '' }),
          h('span', {}, g.active && g.paused ? '協作已暫停' : st.running ? '協作進行中' : '等待工作')),
        h('p', { class: 'group-nav-hint' }, 'Ctrl + K 快速切換', h('br'), '拖曳左側圖標可調整群組順序')));
  }
  function dmRow(c) {
    const st = stateOf(c.id);
    const active = S.view === 'chat' && S.current === c.id;
    const pend = pendingCount(c.id);
    let sub;
    if (c.kind === 'group') {
      const g = st.group || {};
      if (g.active && g.paused) sub = '等你決定下一步';
      else if (g.active) sub = `第 ${g.round || 0} 輪 · ${g.current === 'codex' ? 'GPT 審查中' : g.phase === 'write' ? 'Claude 撰寫中' : 'Claude 複審中'}`;
      else sub = (c.cwd ? baseName(c.cwd) + ' · ' : '') + (3 + (c.geminiMember ? 1 : 0) + (c.localMembers || []).length) + ' 位成員' + (c.cwd ? '' : ' · 尚未選資料夾');
    } else {
      const agent = c.kind;
      if ((st.typing || []).includes(agent)) sub = '正在輸入…';
      else if ((S.auth[agent] || {}).state !== 'ok') sub = '尚未通過訂閱檢查';
      else sub = modelLabel(agent, agent === 'claude' ? c.claudeModel : c.codexModel) + (c.cwd ? ' · ' + baseName(c.cwd) : '');
    }
    const av = c.kind === 'group'
      ? groupAvatar(c)
      : avatar(c.kind, 32, agentStatus(c.kind, c.id));
    return h('button', { class: `dm${active ? ' active' : ''}${S.unread[c.id] ? ' unread' : ''}`, onclick: () => openConv(c.id) },
      av,
      h('div', { class: 'dm-text' }, h('div', { class: 'dm-name' }, convTitle(c)), h('div', { class: 'dm-sub' }, sub)),
      pend ? h('span', { class: 'badge' }, pend) : null);
  }

  function renderUserbar() {
    const ub = $('#userbar');
    const ok = S.auth.ok;
    put(ub,
      h('button', { class: 'who', type: 'button', 'aria-label': '開啟個人資料', onclick: e => profileDialog('user', S.current, e.currentTarget) }, avatar('user', 32, ok ? 'ok' : 'wait'),
        h('div', { style: { minWidth: 0 } },
          h('div', { class: 'name' }, displayName('user')),
          h('div', { class: 'sub' }, ['claude', 'codex', 'gemini'].filter(a => S.auth[a]?.state === 'ok').map(a => S.auth[a].planLabel || AGENTS[a].name).join(' · ') || '帳號檢查未完成'))),
      h('button', { class: 'icon-btn', 'data-tip': '訂閱與用量', onclick: () => openView('account') }, icon('chart', 20)),
      S.desk.update.available ? h('button', { class: 'icon-btn on', 'aria-label': '有可用更新', 'data-tip': '有可用更新',
        onclick: () => { openSettings('updates'); send({ type: 'update.get' }); } }, icon('download', 20)) : null,
      h('button', { class: 'icon-btn', 'aria-label': '使用者設定', 'data-tip': '使用者設定', onclick: () => openSettings() }, icon('sliders', 20)),
    );
  }

  // ================================================================ main area
  const chat = { convId: null };

  function renderMain(light) {
    const main = $('#main');
    if (S.view === 'plugins') { renderPluginsPage(main, light); return; }
    if (S.view === 'account') { renderAccountPage(main); return; }
    if (S.view === 'inbox' && window.CCACollaboration) { window.CCACollaboration.inbox(main, S.inbox); return; }
    const c = conv();
    if (!c) { clear(main); return; }
    if (chat.convId !== c.id || !main.contains(chat.list || null)) buildChat(main, c);
    renderChatHead(c);
    renderWorkflow(c);
    renderTyping(c);
    renderComposerState(c);
    renderReview();
    git.render();
  }

  function buildChat(main, c) {
    if (chat.workflow) chat.workflow.destroy();
    chat.convId = c.id;
    chat.head = h('div', { class: 'chat-head' });
    chat.flow = h('div', { class: 'workflow-host' });
    chat.workflow = new CCAWorkflow.View(chat.flow, {
      // Before any run this session the chart is one line; expanding it then is a peek, not a saved preference.
      onMode: (mode) => {
        if (!flowLive(conv())) { S.flowPeek = mode === 'auto'; renderMain(); return; }
        send({ type: 'visual.set', mode });
      },
      onMotion: (motion) => send({ type: 'visual.set', motion }),
    });
    chat.list = h('div', { class: 'messages' });
    chat.scroll = h('div', { class: 'chat-scroll' }, chat.list);
    // Stay pinned to the newest message while the reader is at the bottom, even as
    // streaming text, cards or the composer change the layout.
    chat.stick = true;
    chat.scroll.addEventListener('scroll', () => {
      const sc = chat.scroll;
      chat.stick = sc.scrollHeight - sc.scrollTop - sc.clientHeight < 40;
    });
    if (chat.observer) chat.observer.disconnect();
    chat.observer = new ResizeObserver(() => { if (chat.stick) chat.scroll.scrollTop = chat.scroll.scrollHeight; });
    chat.observer.observe(chat.list);
    chat.observer.observe(chat.scroll);
    chat.typing = h('div', { class: 'typing' });
    chat.banner = h('div');
    chat.textarea = h('textarea', { rows: 1, spellcheck: false });
    chat.textarea.value = S.drafts[c.id] || '';
    chat.textarea.addEventListener('input', () => { S.drafts[c.id] = chat.textarea.value; autoGrow(); });
    chat.textarea.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' && !e.shiftKey && !e.isComposing && e.keyCode !== 229) { e.preventDefault(); submit(); }
    });
    chat.addBtn = h('button', { class: 'c-btn', 'data-tip': '插入檔案路徑', onclick: insertPath }, icon('plus', 22));
    chat.composer = h('div', { class: 'composer' }, chat.addBtn, chat.textarea, h('div', { style: { width: '12px' } }));
    chat.hint = h('div', { class: 'composer-hint' });
    put(main, chat.head, chat.flow, chat.scroll, chat.typing,
      h('div', { class: 'composer-wrap' }, chat.banner, chat.composer, chat.hint));
    renderMessages(true);
    setTimeout(() => chat.textarea && chat.textarea.focus(), 0);
    autoGrow();
  }
  function autoGrow() {
    const t = chat.textarea;
    if (!t) return;
    t.style.height = 'auto';
    t.style.height = Math.min(t.scrollHeight, window.innerHeight * 0.5) + 'px';
  }
  async function insertPath() {
    const c = conv();
    const p = await request({ type: 'folder.pick', initial: c && c.cwd });
    if (!p) return;
    const t = chat.textarea;
    const rel = c && c.cwd && p.toLowerCase().startsWith(c.cwd.toLowerCase()) ? p.slice(c.cwd.length).replace(/^[\\/]/, '') || '.' : p;
    t.setRangeText(rel, t.selectionStart, t.selectionEnd, 'end');
    S.drafts[c.id] = t.value;
    t.focus();
    autoGrow();
  }
  function submit() {
    const c = conv();
    if (!c) return;
    const text = chat.textarea.value.trim();
    if (!text) return;
    const verdict = canSend(c);
    if (!verdict.ok) { toast('info', verdict.why === '回覆中' ? `等 ${convTitle(c)} 回覆完再傳下一則` : verdict.why); return; }
    if (running(c.id)) {
      if (S.desk.composerPending) return;
      S.desk.composerPending = true;
      desk.action('add', c, { text }, () => {
        if ((S.drafts[c.id] || '').trim() === text) S.drafts[c.id] = '';
        if (chat.convId === c.id && chat.textarea.value.trim() === text) { chat.textarea.value = ''; autoGrow(); }
        S.desk.composerPending = false;
        toast('ok', '指令已保存到待送佇列');
      }, () => { S.desk.composerPending = false; });
      return;
    }
    send({ type: 'conv.send', id: c.id, text });
    chat.textarea.value = '';
    S.drafts[c.id] = '';
    autoGrow();
  }
  function canSend(c) {
    const pipeline = c.kind === 'group' && c.pipeline?.nodes?.length;
    if (c.kind === 'gemini') {
      const g = S.auth.gemini || {};
      if (g.state !== 'ok') return { ok: false, why: g.message || 'Gemini 帳號尚未通過檢查' };
      if (c.permission === 'plan' && !c.daily) return { ok: false, why: 'Gemini CLI 不提供唯讀規劃權限，請改用「每次詢問」或更高權限' };
      if (!c.cwd && !c.daily) return { ok: false, why: '先選擇專案資料夾' };
      return c.folderMissing && !c.daily ? { ok: false, why: '找不到專案資料夾' } : { ok: true };
    }
    if (!S.auth.ok && !pipeline) return { ok: false, why: '請先登入 Claude 或 GPT，或配置 API 流程' };
    const lead = c.kind === 'codex' ? 'codex' : (c.lead || 'claude');
    const ready = (S.auth[lead] || {}).state === 'ok';
    const fallback = c.autoFallback !== false && c.claudeLimited && (S.auth.codex || {}).state === 'ok';
    if (!pipeline && !ready && !(lead === 'claude' && fallback)) return { ok: false, why: '目前主刀手尚未通過登入檢查，可改由 GPT 主導' };
    const daily = c.kind !== 'group' && c.daily;  // daily chats need no project folder
    if (!c.cwd && !daily) return { ok: false, why: '先選擇專案資料夾' };
    if (c.folderMissing && !daily) return { ok: false, why: '找不到專案資料夾' };

    return { ok: true };
  }

  function chip(props, ...kids) { return h('button', { class: 'chip', ...props }, ...kids); }

  // The header names the channel and says in one line how it is set up; every control lives in the
  // channel settings panel (opsPanel), so the header never overflows into cut-off chips.
  function headFacts(c) {
    const daily = c.kind !== 'group' && c.daily;
    if (daily) {
      if (c.kind === 'gemini') return ['日常模式', 'Gemini CLI 預設模型'];
      const d = dailyPick(c.kind === 'claude' && c.lead === 'codex' ? 'codex' : c.kind);
      return ['日常模式', modelWithEffort(c.kind, d.model, d.effort)];
    }
    const facts = [];
    const nodes = c.pipeline?.nodes?.length;
    if (c.kind === 'group') facts.push(nodes ? `多代理流程 · ${nodes} 位` : `${displayName(c.lead || 'claude')} 主刀`);
    else {
      const agent = c.kind === 'claude' && c.lead === 'codex' ? 'codex' : c.kind;
      facts.push(selectionLabel(c, agent));
    }
    facts.push((PERMS[c.permission] || PERMS.edit).label);
    facts.push(c.cwd ? baseName(c.cwd) : '未選擇資料夾');
    return facts;
  }

  function renderChatHead(c) {
    const head = clear(chat.head);
    const st = stateOf(c.id);
    const g = st.group || {};
    const perm = PERMS[c.permission] || PERMS.edit;
    const daily = c.kind !== 'group' && c.daily;
    const status = [];
    if (!daily && perm.danger) status.push(h('span', { class: 'chip keep danger', 'data-tip': perm.desc }, icon('bolt', 14), h('span', {}, '跳過確認')));
    if (!daily && (!c.cwd || c.folderMissing)) {
      status.push(chip({ class: 'chip keep warn', onclick: () => pickFolder(c) }, icon('folder', 14), h('span', {}, c.folderMissing ? '找不到資料夾' : '選擇資料夾')));
    } else if (!daily && !c.isGit) {
      status.push(h('span', { class: 'chip warn icon-only', 'data-tip': '這個資料夾沒有 git。AI 的修改無法用 git 還原，建議先執行 git init 並提交一次。' }, icon('branch', 14)));
    }
    if (c.kind === 'group' && g.active) {
      status.push(h('span', { class: `chip keep ${g.paused ? 'warn' : 'live'}` },
        g.paused ? icon('alert', 14) : h('span', { class: 'spinner', style: { width: '12px', height: '12px' } }),
        h('span', {}, g.paused ? '等你決定' : `第 ${g.round} 輪 · ${PHASES[g.phase] || ''}中`)));
    }
    const facts = headFacts(c);
    head.append(
      c.kind === 'group'
        ? h('button', { class: 'title', 'data-tip': '編輯群組資料與頭像', onclick: () => renameDialog(c) }, groupAvatar(c, 26), h('span', {}, convTitle(c)))
        : h('div', { class: 'title' }, icon('at', 22), h('span', {}, convTitle(c))),
      h('div', { class: 'head-sep' }),
      h('button', { class: 'head-facts', type: 'button', 'data-tip': '頻道設定：模型、權限、資料夾', onclick: (ev) => opsPanel(ev.currentTarget, c) },
        facts.map((f, i) => [i ? h('i', {}, '/') : null, h('span', {}, f)])),
      h('div', { class: 'head-status' }, status),
      h('div', { class: 'head-right' },
        h('button', { class: 'icon-btn', 'aria-label': '上下文與待送指令', 'data-tip': '上下文來源與待送指令', onclick: () => desk.open(c) }, icon('list', 20)),
        daily ? null : h('button', { class: 'icon-btn', 'aria-label': '審查變更', 'data-tip': '審查變更', onclick: () => openReview(c, 'task') }, icon('diff', 20)),
        daily ? null : h('button', { class: 'icon-btn', 'aria-label': '版本控制', 'data-tip': '版本控制：選檔提交、推送與提交紀錄', onclick: () => openGit(c) }, icon('branch', 20)),
        h('button', { class: 'icon-btn', 'aria-label': '頻道設定', 'data-tip': '頻道設定', onclick: (ev) => opsPanel(ev.currentTarget, c) }, icon('sliders', 20)),
        c.cwd ? h('button', { class: 'icon-btn', 'data-tip': '在檔案總管開啟資料夾', onclick: () => send({ type: 'open', target: c.cwd }) }, icon('external', 20)) : null,
        h('button', { class: 'icon-btn danger', 'data-tip': c.kind === 'group' ? '清除對話或刪除群組' : '清除對話（開始新的工作階段）', onclick: () => clearDialog(c) }, icon('trash', 20)),
        h('button', { class: `icon-btn${panelVisible() ? ' on' : ''}`, 'data-tip': panelVisible() ? '隱藏側欄' : '顯示側欄', onclick: togglePanel },
          icon(c.kind === 'group' ? 'users' : 'info', 20)),
      ),
    );
  }

  // Channel settings: everything the old header chips did, as labelled rows.
  function opsPanel(anchor, c) {
    const busy = running(c.id);
    const daily = c.kind !== 'group' && c.daily;
    const nodes = c.pipeline?.nodes?.length;
    const set = (settings) => { send({ type: 'conv.settings', id: c.id, settings }); closePop(); };
    const row = (label, ...kids) => h('div', { class: 'ops-row' }, h('div', { class: 'ops-label' }, label), h('div', { class: 'ops-value' }, ...kids));
    const toggle = (on, label, onclick, props = {}) => h('button', { class: `chip keep toggle${on ? ' on' : ''}`, type: 'button', onclick, ...props },
      h('span', { class: 'switch' }, h('i')), h('span', {}, label));
    const seg = (options, value, onpick) => h('div', { class: 'seg' },
      options.map(([v, label]) => h('button', { class: v === value ? 'sel' : '', type: 'button', disabled: busy, onclick: () => onpick(v) }, label)));
    const rows = [h('div', { class: 'pop-title' }, convTitle(c) + ' · 頻道設定')];
    if (c.kind === 'group') {
      if (!nodes) rows.push(row('主刀', seg([['claude', displayName('claude')], ['codex', displayName('codex')]], c.lead || 'claude', (v) => set({ lead: v }))));
      rows.push(row('多代理流程', chip({ class: 'chip keep', onclick: () => { closePop(); window.CCAPlatform.edit(c); } },
        icon('users', 14), h('span', {}, nodes ? `${nodes} 位代理` : '雙方協作'), icon('chevron', 14))));
    } else if (!special()) {
      rows.push(row('日常模式', toggle(daily, daily ? '開啟：最省額度的模型' : '關閉', () => set({ daily: !daily }))));
    }
    if (special() && !nodes) {
      rows.push(row(S.edition.name, h('span', { class: 'chip keep' }, '自動選擇時用最高推理 · 測試守門固定開啟' + (c.kind === 'group' ? ' · 完成前必須通過測試' : ''))));
    }
    if (!daily && !nodes) {
      const agents = c.kind === 'group' ? ['claude', 'codex'] : [c.kind === 'claude' && c.lead === 'codex' ? 'codex' : c.kind];
      for (const a of agents) {
        // The Gemini CLI reports no model catalogue, so its row states the default instead of offering guesses.
        rows.push(a === 'gemini' ? row(displayName(a) + ' 模型', h('span', { class: 'chip keep' }, avatar(a, 16), h('span', {}, 'Gemini CLI 預設模型')))
          : row(displayName(a) + ' 模型', chip({ class: 'chip keep', onclick: (ev) => modelPicker(ev.currentTarget, c, a) }, avatar(a, 16),
            h('span', {}, selectionLabel(c, a)), icon('chevron', 14))));
      }
    }
    if (c.kind === 'group' && !nodes) {
      const g = S.auth.gemini || {};
      rows.push(row('Gemini 成員', toggle(!!c.geminiMember, c.geminiMember ? '已加入：第三位審查者' : '未加入',
        () => set({ geminiMember: !c.geminiMember }), { disabled: busy, 'data-tip': g.state === 'ok' ? '在 GPT 之後審查並可直接修改'
          : (g.message || stateText(g.state)) + '；加入後輪到時會跳過' })));
    }
    if (c.kind !== 'codex' && c.kind !== 'gemini' && !nodes && !daily) {
      const group = c.kind === 'group';
      rows.push(row('額度用完時',
        toggle(c.autoFallback !== false, c.autoFallback !== false ? (group ? '跳過，交給下一位' : `交給 ${displayName('codex')}`) : '先問我',
          () => set({ autoFallback: c.autoFallback === false }),
          { 'data-tip': group ? '全部成員都沒有額度時會強制停止，並記錄做到哪一步' : '' }),
        chip({ class: 'chip keep', 'data-tip': '停止 ' + displayName('claude') + ' 並附上目前工作紀錄交接', onclick: () => { send({ type: 'conv.takeover', id: c.id }); closePop(); } },
          '現在交接')));
    }
    if (!daily) {
      const perm = PERMS[c.permission] || PERMS.edit;
      rows.push(row('權限', chip({ class: `chip keep${perm.danger ? ' danger' : ''}`, onclick: (ev) => permPicker(ev.currentTarget, c) },
        icon(perm.icon, 14), h('span', {}, perm.label), icon('chevron', 14))));
      rows.push(row('專案資料夾', chip({ class: `chip keep${c.cwd && !c.folderMissing ? '' : ' warn'}`, 'data-tip': c.cwd || '', onclick: () => { closePop(); pickFolder(c); } },
        icon('folder', 14), h('span', {}, c.cwd ? baseName(c.cwd) : '選擇資料夾'))));
    }
    if (c.kind === 'group' && !nodes) {
      rows.push(row('來回上限', chip({ class: 'chip keep', onclick: (ev) => roundsPicker(ev.currentTarget, c) }, h('span', {}, `${c.maxRounds} 輪`), icon('chevron', 14))));
      rows.push(row('測試守門', special() ? h('span', { class: 'chip keep' }, c.testCommand ? '固定開啟' : '需要測試指令才能開始') :
        h('button', { class: `chip keep toggle${c.regressionGuard ? ' on' : ''}`, type: 'button', disabled: busy || (!c.regressionGuard && !c.testCommand),
          'data-tip': c.testCommand ? '審查者改了檔案後執行測試；轉紅就還原它的修改' : '先設定測試指令',
          onclick: () => set({ regressionGuard: !c.regressionGuard }) }, h('span', {}, c.regressionGuard ? '開啟' : '關閉')),
        chip({ class: 'chip keep', 'data-tip': c.testCommand || '', onclick: () => { closePop(); testGuardDialog(c); } },
          h('span', {}, c.testCommand ? '指令：' + c.testCommand.slice(0, 18) + (c.testCommand.length > 18 ? '…' : '') : '設定測試指令'))));
      const invited = new Set(c.localMembers || []), locals = S.settings.locals || {};
      if (!special()) rows.push(row('本地審查員', ...['local1', 'local2'].map((l) => {
        const model = (locals[l] || {}).model, on = invited.has(l);
        return h('button', { class: `chip keep toggle${on ? ' on' : ''}`, type: 'button', disabled: !model || busy, 'data-tip': model || '尚未設定模型',
          onclick: () => set({ localMembers: ['local1', 'local2'].filter((x) => (x === l ? !on : invited.has(x))) }) },
        avatar(l, 16), h('span', {}, displayName(l)));
      }), chip({ class: 'chip keep', onclick: () => { closePop(); openSettings('local'); } }, '模型')));
      const mode = (S.settings.visual || {}).mode || 'auto';
      rows.push(row('流程圖', seg([['auto', '自動'], ['compact', '一行'], ['off', '隱藏']], mode, (v) => { send({ type: 'visual.set', mode: v }); closePop(); })));
    }
    const modes = special() ? [] : extensions.modesFor(c);
    if (modes.length || c.mode) {
      rows.push(row('插件模式', chip({ class: 'chip keep', onclick: (ev) => modePicker(ev.currentTarget, c, modes) },
        h('span', {}, modes.find(m => m.id === c.mode)?.name || (c.mode ? '模式已停用' : '無')), icon('chevron', 14))));
    }
    if (window.CCACollaboration) rows.push(row('任務控制',
      chip({ onclick: () => { closePop(); window.CCACollaboration.planDialog(c); } }, '計畫與驗收'),
      chip({ onclick: () => { closePop(); window.CCACollaboration.opinionDialog(c); } }, '多模型意見'),
      chip({ onclick: () => { closePop(); window.CCACollaboration.focusDialog(c); } }, '審查關注點'),
      chip({ onclick: () => { closePop(); window.CCACollaboration.workspaceDialog(c); } }, '隔離工作區')));
    popover(anchor, rows, 'wide ops');
  }

  // A run exists in this session (running, paused or just finished): the chart is worth its space.
  const flowLive = (c) => !!(c && (stateOf(c.id).workflow || {}).runId);

  function renderWorkflow(c) {
    if (!chat.workflow) return;
    chat.flow.hidden = false;
    const visual = S.settings.visual || {};
    let mode = visual.mode || 'auto';
    if (mode === 'auto' && !flowLive(c) && !S.flowPeek) mode = 'compact';
    chat.workflow.update(stateOf(c.id).workflow || { nodes: [], label: '等待工作' }, {
      names: displayName, conversationId: c.id, mode, motion: visual.motion !== false,
    });
  }

  function renderTyping(c) {
    const t = clear(chat.typing);
    const st = stateOf(c.id);
    const typing = st.typing || [];
    if (typing.length) {
      const names = typing.map((a) => displayName(a));
      const thinking = typing.every((a) => st.thinking && st.thinking[a]);
      t.append(h('span', { class: 'dots' }, h('i'), h('i'), h('i')),
        h('span', {}, h('b', {}, names.join(' 和 ')), typing.every(isLocalAgent) ? ' 正在唯讀審查…' : thinking ? ' 正在思考…' : ' 正在輸入…'));
    }
    if (st.preparing) t.append(h('span', { class: 'muted' }, st.preparing === 'guard' ? '正在執行測試，檢查審查者的修改…' : '正在建立檢查點…'));
    if (st.running) {
      t.append(h('button', { class: 'stop', onclick: () => send({ type: 'conv.stop', id: c.id }) }, icon('stop', 12),
        c.kind === 'group' ? '停止協作' : '停止'));
    }
  }

  function renderComposerState(c) {
    const b = clear(chat.banner);
    const verdict = canSend(c);
    if (c.kind === 'gemini' && (S.auth.gemini || {}).state !== 'ok') {
      b.append(h('div', { class: 'banner warn' }, icon('alert', 18), (S.auth.gemini || {}).message || 'Gemini 帳號尚未通過檢查。',
        h('button', { class: 'link-btn', onclick: () => openView('account') }, '查看狀態')));
    } else if (c.kind === 'gemini' && c.daily) {
      b.append(h('div', { class: 'banner info' }, icon('chart', 18), '日常模式：用 Gemini CLI 預設模型聊天；無介面的 Gemini 不能逐項詢問，因此不會修改檔案或執行指令。',
        h('button', { class: 'link-btn', onclick: () => send({ type: 'conv.settings', id: c.id, settings: { daily: false } }) }, '回到工作模式')));
    } else if (!S.auth.ok && !c.pipeline?.nodes?.length) {
      b.append(h('div', { class: 'banner warn' }, icon('alert', 18), '請登入 Claude 或 GPT；GPT 主導時可獨立工作。',
        h('button', { class: 'link-btn', onclick: () => openView('account') }, '查看狀態')));
    } else if (c.kind !== 'group' && c.daily) {
      const d = dailyPick(c.kind === 'claude' && c.lead === 'codex' ? 'codex' : c.kind);
      b.append(h('div', { class: 'banner info' }, icon('chart', 18),
        `日常模式：用 ${modelWithEffort(c.kind, d.model, d.effort)} 聊天，最省額度；修改檔案或執行指令前會先問你。`,
        h('button', { class: 'link-btn', onclick: () => send({ type: 'conv.settings', id: c.id, settings: { daily: false } }) }, '回到工作模式')));
    } else if (!c.cwd || c.folderMissing) {
      b.append(h('div', { class: 'banner info' }, icon('folder', 18),
        c.folderMissing ? '找不到原本的專案資料夾：' + c.cwd : 'AI 會在你選的資料夾裡讀寫檔案。',
        h('button', { class: 'link-btn', onclick: () => pickFolder(c) }, '選擇資料夾')));
    } else if (c.permission === 'bypass') {
      b.append(h('div', { class: 'banner danger' }, icon('bolt', 18), '已跳過所有確認：AI 不會詢問就直接修改檔案和執行指令。',
        h('button', { class: 'link-btn', onclick: () => send({ type: 'conv.settings', id: c.id, settings: { permission: 'edit' } }) }, '改回自動編輯')));
    }
    const disabled = (!canSend(c).ok && !running(c.id)) || ((!c.cwd || c.folderMissing) && !(c.kind !== 'group' && c.daily));
    chat.composer.classList.toggle('disabled', disabled);
    chat.textarea.disabled = disabled;
    chat.textarea.placeholder = disabled ? (verdict.why || '') :
      c.kind === 'group' ? `對 ${convTitle(c)} 下指令：${c.pipeline?.nodes?.length ? '依設定的多代理流程執行' : displayName(c.lead || 'claude') + ' 主導實作與驗證'}`
        : c.daily ? `和 ${convTitle(c)} 聊聊天` : `傳訊息給 @${convTitle(c)}`;
    put(chat.hint,
      h('span', {}, h('kbd', {}, 'Enter'), ' 傳送'),
      h('span', {}, h('kbd', {}, 'Shift + Enter'), ' 換行'),
      h('button', { class: 'link-btn', onclick: () => desk.open(c, 'queue') }, `待送指令 ${(stateOf(c.id).queue || []).filter(q => q.status === 'pending').length}`),
      running(c.id) ? h('span', {}, 'Enter 加入佇列，任務成功結束後依序送出') : null,
    );
  }

  // ================================================================ messages
  function renderMessages(toBottom) {
    if (!chat.list) return;
    const c = conv();
    const list = S.msgs[c.id] || [];
    put(chat.list, welcome(c));
    let prev = null;
    for (const m of list) {
      if (prev && !sameDay(prev.ts, m.ts)) chat.list.append(dayDivider(m.ts));
      chat.list.append(renderMsg(m, prev));
      prev = m;
    }
    if (toBottom) scrollToBottom();
  }
  function scrollToBottom() {
    chat.stick = true;
    requestAnimationFrame(() => { if (chat.scroll) chat.scroll.scrollTop = chat.scroll.scrollHeight; });
  }
  const dayDivider = (ts) => h('div', { class: 'day-divider' }, h('span', {}, dayFmt.format(new Date(ts))));

  function welcome(c) {
    if (c.kind === 'group' && c.pipeline?.nodes?.length) {
      return h('div', { class: 'welcome' }, groupAvatar(c, 80), h('h3', {}, '歡迎來到 ' + convTitle(c)),
        h('p', {}, '代理依設定順序接手，交接時保留任務、檔案變更與審查紀錄。'),
        h('div', { class: 'flow' }, c.pipeline.nodes.flatMap((n, i) => [i ? icon('arrow', 16) : null,
          h('span', { class: 'step' }, avatar(n.backend, 18), pipelineName(n) + ' ' + (PHASES[n.role] || n.role))])),
        h('p', { class: 'small' }, '流程與權限可在頻道設定中調整；需要測試證據時，交付前仍須通過驗證。'));
    }
    if (c.kind === 'group') {
      return h('div', { class: 'welcome' },
        groupAvatar(c, 80),
        h('h3', {}, '歡迎來到 ' + convTitle(c)),
        h('p', {}, '選擇主刀手後開始工作；額度耗盡時可交接給 GPT：'),
        h('div', { class: 'flow' },
          h('span', { class: 'step' }, avatar('user', 18), '你下指令'), icon('arrow', 16),
          h('span', { class: 'step' }, avatar(c.lead || 'claude', 18), displayName(c.lead || 'claude') + ' 撰寫'), icon('arrow', 16),
          h('span', { class: 'step' }, avatar(c.lead === 'codex' ? 'claude' : 'codex', 18), '審查或自查'), icon('arrow', 16),
          h('span', { class: 'step' }, avatar(c.lead || 'claude', 18), '驗證與交付'), icon('refresh', 16),
          h('span', { class: 'step' }, '來回直到雙方都沒有意見')),
        h('p', {}, `最多來回 ${c.maxRounds} 輪後會先停下來問你。每一輪改了哪些檔案、加了幾行、刪了幾行都會列出來。`),
        h('div', { class: 'row' },
          h('button', { class: 'btn primary', onclick: () => pickFolder(c) }, icon('folder', 16), c.cwd ? '更換專案資料夾' : '選擇專案資料夾'),
          c.cwd ? h('span', { class: 'mono muted', style: { fontSize: '13px' } }, c.cwd) : null));
    }
    const a = AGENTS[c.kind];
    return h('div', { class: 'welcome' },
      avatar(c.kind, 80),
      h('h3', {}, convTitle(c)),
      h('p', {}, `這是你和 ${convTitle(c)} 私訊的開頭。${convTitle(c)} 透過 ${a.vendor} 的 ${a.cli} 在你的電腦上工作，使用你自己的訂閱額度。`,
        personaOf(c.kind) ? ` 目前的性格是「${personaOf(c.kind).name}」。` : ''),
      h('div', { class: 'row' },
        h('button', { class: 'btn primary', onclick: () => pickFolder(c) }, icon('folder', 16), c.cwd ? '更換專案資料夾' : '選擇專案資料夾'),
        c.cwd ? h('span', { class: 'mono muted', style: { fontSize: '13px' } }, c.cwd) : null));
  }

  function shouldGroup(m, prev) {
    if (!prev || prev.author !== m.author || prev.author === 'system') return false;
    if (m.ts - prev.ts > 7 * 60 * 1000) return false;
    if (m.author !== 'user') return false;  // every agent turn gets its own header
    return true;
  }

  function messageText(m) {
    if (m.author === 'user' || m.author === 'system') return m.text || '';
    return (m.blocks || []).filter((b) => b.type === 'text').map((b) => b.text).join('\n\n');
  }

  // Result of CCA's compliance checks on this reply (src/guard.cpp).
  function guardTag(m) {
    const g = m.guard;
    if (!g || !g.status) return null;
    const labels = (g.issues || []).map((i) => i.label || i.text).filter(Boolean);
    if (g.status === 'rejected') {
      return h('span', { class: 'guard-tag bad', 'data-tip': '未通過規範審查：' + (labels.join('、') || '不符合規範') }, '未通過規範');
    }
    if (g.status === 'accepted') return h('span', { class: 'guard-tag warn', 'data-tip': '未通過規範審查，由你接受' }, '已接受');
    if (g.status === 'passed') {
      return h('span', { class: 'guard-tag ok', 'data-tip': `退回 ${g.attempt || 1} 次後通過規範審查` }, '已修正');
    }
    return null;
  }

  function renderMsg(m, prev) {
    if (m.author === 'system') return renderSystem(m);
    const grouped = shouldGroup(m, prev);
    const isAgent = !!AGENTS[m.author];
    // The author hook lets a theme shape the user's turns differently from agent replies.
    const el = h('div', { class: `msg${grouped ? '' : ' head'}`, 'data-id': m.id, 'data-author': isAgent ? m.author : 'user' });
    if (!grouped) {
      el.append(avatar(isAgent ? m.author : 'user', 40));
      const portrait = el.lastChild;
      portrait.classList.add('inspect-avatar');
      portrait.setAttribute('role', 'button'); portrait.tabIndex = 0;
      portrait.setAttribute('aria-label', '查看 ' + displayName(isAgent ? m.author : 'user') + ' 的資料卡');
      portrait.onclick = () => profileDialog(isAgent ? m.author : 'user', S.current, portrait);
      portrait.onkeydown = e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); portrait.click(); } };
      const header = h('div', { class: 'msg-header' },
        h('span', { class: `msg-author ${isAgent ? m.author : ''}` }, m.agent_label ? pipelineName({ name: m.agent_label, backend: m.author }) : displayName(isAgent ? m.author : 'user')),
        m.billing ? h('span', { class: 'phase-tag' }, { api: 'API 計費', local: '本地唯讀', subscription: 'CLI 訂閱', 'google-account': 'Google 帳號' }[m.billing] || m.billing) : null,
        isAgent ? h('span', { class: 'app-tag' }, 'APP') : null,
        isLocalAgent(m.author) ? h('span', { class: 'phase-tag advise', 'data-tip': '本地模型：只能看、不能改，建議要經過查證' }, '唯讀') : null,
        m.phase ? h('span', { class: `phase-tag ${m.phase}` }, PHASES[m.phase] + (m.round ? ` · 第 ${m.round} 輪` : '')) : null,
        m.daily ? h('span', { class: 'daily-tag', 'data-tip': '日常模式（最省額度的模型）' }, '日常') : null,
        guardTag(m),
        h('span', { class: 'msg-time', 'data-tip': new Date(m.ts).toLocaleString('zh-TW') }, stamp(m.ts)),
        isAgent ? h('span', { class: 'msg-model' }, modelBadge(m)) : null);
      el.append(header);
    } else {
      el.append(h('span', { class: 'gutter-time' }, timeFmt.format(new Date(m.ts))));
    }
    if (isAgent && m.routing && (m.routing.autoModel || m.routing.autoEffort)) {
      el.append(h('details', { class: 'route-note' }, h('summary', {}, `依需求選擇 · ${m.routing.modelName || '後端預設'} · ${m.routing.effort || '預設思考程度'}`),
        h('p', {}, m.routing.reason || ''), m.routing.effortNote ? h('p', {}, m.routing.effortNote) : null));
    }
    if (!isAgent) {
      el.append(h('div', { class: 'msg-body', html: MD.render(m.text || '') }));
    } else {
      el.append(...renderBlocks(m));
    }
    const text = messageText(m);
    const canRollback = !isLocalAgent(m.author) && !m.streaming;
    if (text || canRollback) {
      el.append(h('div', { class: 'msg-tools' },
        text ? h('button', { 'data-tip': '複製文字', onclick: () => copyText(text) }, icon('copy', 18)) : null,
        canRollback ? h('button', { 'data-tip': '回滾到這則之前', onclick: () => {
          if (running()) { toast('info', 'AI 正在工作，停止後才能回滾'); return; }
          rollbackDialog(m);
        } }, icon('refresh', 18)) : null));
    }
    return el;
  }

  function modelBadge(m) {
    const agent = m.author;
    const parts = [m.resolvedModel || modelLabel(agent, m.model)];
    if (m.effort) parts.push(effortLabel(m.effort));
    return parts.join(' · ');
  }
  // Mirrors App::dailyModel / dailyEffort in profile.cpp.
  function dailyPick(agent) {
    const d = (S.settings && S.settings.daily) || {};
    let model = d[agent + 'Model'] || '';
    if (!model) {
      const list = S.models[agent] || [];
      const hit = (kw) => list.find((m) => ((m.value || '') + ' ' + (m.displayName || '')).toLowerCase().includes(kw));
      const found = agent === 'claude' ? hit('haiku') : (hit('luna') || hit('nano') || hit('mini'));
      model = found ? found.value : agent === 'claude' ? 'haiku' : '';
    }
    let effort = d[agent + 'Effort'] || '';
    if (!effort) {
      const order = ['minimal', 'low', 'medium', 'high', 'xhigh', 'max', 'ultra'];
      const levels = ((modelInfo(agent, model) || {}).supportedEffortLevels || []).slice().sort((a, b) => order.indexOf(a) - order.indexOf(b));
      effort = levels[0] || (agent === 'codex' && !modelInfo(agent, model) ? 'low' : '');
    }
    return { model, effort };
  }

  function effortLabel(e) {
    return EFFORTS[e] ? EFFORTS[e][0] : e;
  }
  // "Opus 5.5 Ultracode", "GPT-6.1-Sol Max": the model and its thinking level as one name.
  function modelWithEffort(agent, model, effort) {
    return modelLabel(agent, model) + (effort ? ' ' + effortLabel(effort) : '');
  }
  const shortModel = (agent, model, effort) => modelLabel(agent, model).replace(/s*[（(].*$/, '') + (effort ? ' ' + effortLabel(effort) : '');
  function selectionLabel(c, agent) {
    if (agent === 'gemini') return 'Gemini CLI 預設模型';
    const auto = c.adaptive?.[agent] || {};
    const model = agent === 'claude' ? c.claudeModel : c.codexModel;
    const effort = agent === 'claude' ? c.claudeEffort : c.codexEffort;
    if (!auto.model && !auto.effort) return shortModel(agent, model, effort);
    return (auto.model ? '自動模型' : shortModel(agent, model)) + ' · ' +
      (auto.effort ? '自動思考' : effort ? effortLabel(effort) : '預設思考');
  }

  const foldOpen = (key, def) => (S.fold.has(key) ? S.fold.get(key) : def);
  function foldToggle(key, def) { S.fold.set(key, !foldOpen(key, def)); }
  function clearFold(id) {
    for (const k of [...S.fold.keys()]) if (k.startsWith(id + ':')) S.fold.delete(k);
  }

  // Keep intermediate commentary with the commands it describes. Only the last
  // text block is the reply; short progress notes must fold away too.
  function activityGroup(m, run, n) {
    const key = m.id + ':act:' + n;
    const def = !!m.streaming;
    const open = foldOpen(key, def);
    const tools = run.filter((b) => b.type === 'tool'), files = run.filter((b) => b.type === 'file');
    const notes = run.filter((b) => b.type === 'text');
    const failed = tools.filter((b) => b.status && b.status !== 'running' && b.status !== 'ok').length;
    const label = [tools.length ? `${tools.length} 個指令與工具` : '', files.length ? `${files.length} 個檔案變更` : '',
      notes.length ? `${notes.length} 段進度說明` : ''].filter(Boolean).join('、');
    const head = h('button', { type: 'button', class: `act-row expandable act-fold${open ? '' : ' closed'}`, 'aria-expanded': String(open),
      onclick: () => { foldToggle(key, def); rerenderMsg(m.id); } },
      icon('chevron', 14),
      h('span', { class: 'act-title' }, label),
      h('span', { class: 'act-meta' },
        tools.some((b) => b.status === 'running') ? h('span', { class: 'spinner' }) : null,
        failed ? h('span', { class: 'err' }, failed + ' 個失敗') : null,
        open ? '收合' : '展開'));
    const box = h('div', { class: 'activity' }, head);
    if (open) for (const b of run) {
      if (b.type === 'text') box.append(h('div', { class: 'msg-body', html: MD.render(b.text) }));
      else box.append(...(b.type === 'tool' ? toolRow(m, b) : fileRow(m, b)));
    }
    return box;
  }

  // The reply itself is never folded: only the commands, file changes and progress
  // notes before it collapse (see activityGroup).
  function textBlock(m, text) {
    return [h('div', { class: 'msg-body', html: MD.render(text) })];
  }

  function renderBlocks(m) {
    const out = [];
    let run = [], actN = 0, textN = 0;
    const flush = () => { if (run.length) { out.push(activityGroup(m, run, actN++)); run = []; } };
    const blocks = m.blocks || [];
    let replyAt = -1;
    for (let i = blocks.length - 1; i >= 0; i--) {
      if (blocks[i].type === 'text' && blocks[i].text) { replyAt = i; break; }
    }
    for (let i = 0; i < blocks.length; i++) {
      const b = blocks[i];
      if (b.type === 'tool' || b.type === 'file') { run.push(b); continue; }
      if (b.type === 'text' && i !== replyAt) {
        if (b.text) run.push(b);
        textN++;
        continue;
      }
      flush();
      if (b.type === 'text') {
        if (b.text) out.push(...textBlock(m, b.text));
        textN++;
      } else if (b.type === 'approval') out.push(approvalCard(m, b));
      else if (b.type === 'image' && window.CCAPlatform) out.push(window.CCAPlatform.picture(b.artifact));
      else if (b.type === 'question') out.push(questionCard(m, b));
      else if (b.type === 'todo') out.push(todoCard(b));
      else if (b.type === 'notice') out.push(h('div', { class: `notice-line ${b.level === 'warn' ? 'warn' : ''}` }, icon(b.level === 'warn' ? 'alert' : 'info', 15), b.text));
      else if (b.type === 'error') out.push(h('div', { class: 'embed red' }, h('div', { class: 'embed-title' }, icon('alert', 16), '發生錯誤'),
        h('div', { class: 'embed-sub', style: { whiteSpace: 'pre-wrap', color: 'var(--text)' } }, b.text)));
    }
    flush();
    if (m.streaming && !blocks.some((b) => b.type === 'text' && b.text)) {
      const waiting = blocks.some((b) => (b.type === 'approval' || b.type === 'question') && b.state === 'pending');
      if (!waiting) out.push(h('div', { class: 'status-line' }, blocks.length ? '' : '正在處理…'));
    }
    if (m.retry) {
      out.push(h('div', { class: 'retry-line' }, h('span', { class: 'spinner' }),
        m.retry.attempt ? `連線不穩，正在重試（第 ${m.retry.attempt}/${m.retry.max} 次）` : '連線不穩，正在重試', m.retry.error ? '：' + m.retry.error : ''));
    }
    if (m.verdict) {
      const v = { approve: ['check', '結論：同意，沒有修改'], changed: ['pencil', '結論：修改了檔案'], issues: ['alert', '結論：有疑慮'] }[m.verdict];
      if (v) out.push(h('div', { class: `verdict ${m.verdict}` }, icon(v[0], 14), v[1]));
    }
    if (!m.streaming) {
      const foot = [];
      const files = blocks.filter((b) => b.type === 'file');
      if (files.length) {
        const add = files.reduce((a, f) => a + (f.added || 0), 0), rem = files.reduce((a, f) => a + (f.removed || 0), 0);
        foot.push(h('span', {}, `${files.length} 個檔案 `, h('span', { class: 'plus' }, '+' + add), ' ', h('span', { class: 'minus' }, '−' + rem)));
      }
      const u = m.usage;
      if (u) {
        if (u.inputTokens || u.outputTokens) foot.push(h('span', {}, `輸入 ${tok(u.inputTokens)} · 輸出 ${tok(u.outputTokens)}`));
        if (u.contextWindow && u.contextUsed) foot.push(h('span', { 'data-tip': `${tok(u.contextUsed)} / ${tok(u.contextWindow)} tokens` }, `上下文 ${Math.min(100, Math.round(u.contextUsed / u.contextWindow * 100))}%`));
        if (u.costUsd) foot.push(h('span', { 'data-tip': 'Claude Code 估算的等值 API 價格。你用的是訂閱額度，不會另外收費。' }, `約 US$${u.costUsd.toFixed(u.costUsd < 1 ? 3 : 2)}`));
        if (u.durationMs) foot.push(h('span', {}, dur(u.durationMs)));
      }
      if (m.status === 'interrupted') foot.push(h('span', { style: { color: 'var(--yellow)' } }, '已中斷'));
      if (m.status === 'failed') foot.push(h('span', { style: { color: 'var(--red)' } }, '沒有完成'));
      if (foot.length) out.push(h('div', { class: 'msg-foot' }, foot));
    }
    return out;
  }

  function toggleKey(key) {
    if (S.expanded.has(key)) S.expanded.delete(key); else S.expanded.add(key);
  }

  function toolRow(m, b) {
    const key = m.id + ':' + b.id;
    const open = S.expanded.has(key);
    const statusEl = b.status === 'running' ? h('span', { class: 'spinner' })
      : b.status === 'ok' ? h('span', { class: 'ok' }, icon('check', 15))
        : b.status === 'declined' ? h('span', { class: 'dec' }, '已拒絕')
          : b.status === 'interrupted' ? h('span', { class: 'dec' }, '已中斷')
            : h('span', { class: 'err' }, icon('x', 15));
    const detail = b.command ? '$ ' + b.command.split('\n')[0] : b.path || b.detail || '';
    const body = b.output || b.input || b.command || '';
    const expandable = !!body;
    const row = h('div', { class: `act-row${expandable ? ' expandable' : ''}`, onclick: expandable ? () => { toggleKey(key); rerenderMsg(m.id); } : null },
      icon(TOOL_ICONS[b.kind] || 'wrench', 15),
      h('span', { class: 'act-title' }, b.title || b.tool),
      h('span', { class: `act-detail${b.command || b.path ? ' mono' : ''}`, 'data-tip': detail.length > 60 ? detail : null }, detail),
      h('span', { class: 'act-meta' },
        b.exitCode != null && b.exitCode !== 0 ? h('span', { class: 'err' }, '結束碼 ' + b.exitCode) : null,
        b.status !== 'running' && b.durationMs ? dur(b.durationMs) : null,
        statusEl,
        expandable ? icon('chevron', 14) : null));
    const out = [row];
    if (open && body) {
      const text = b.command ? b.command + (b.output ? '\n\n' + b.output : b.input ? '\n\n' + b.input : '') : body;
      out.push(h('div', { class: 'act-out' }, h('pre', {}, text)));
    }
    return out;
  }

  const CHANGE = { add: '新增', update: '修改', delete: '刪除' };
  function fileRow(m, b) {
    const key = m.id + ':' + b.id;
    const open = S.expanded.has(key);
    const row = h('div', { class: 'act-row expandable', onclick: () => { toggleKey(key); rerenderMsg(m.id); } },
      icon('file', 15),
      h('span', { class: `kind ${b.change}` }, CHANGE[b.change] || '修改'),
      h('span', { class: 'act-detail mono', 'data-tip': b.path }, b.path),
      h('span', { class: 'act-meta' }, h('span', { class: 'plus' }, '+' + (b.added || 0)), h('span', { class: 'minus' }, '−' + (b.removed || 0)), icon('chevron', 14)));
    return open ? [row, diffView(b.diff, b.truncated, key)] : [row];
  }

  function diffView(diff, truncated, key) {
    const lines = String(diff || '').replace(/\n$/, '').split('\n');
    const full = S.expanded.has(key + ':all');
    const limit = full ? lines.length : 400;
    const box = h('div', { class: 'diff' });
    for (const line of lines.slice(0, limit)) {
      const cls = line.startsWith('@@') ? 'h' : line.startsWith('+') ? 'a' : line.startsWith('-') ? 'r' : '';
      box.append(h('div', { class: cls }, line || ' '));
    }
    if (lines.length > limit) {
      box.append(h('div', { class: 'more', onclick: (e) => { e.stopPropagation(); S.expanded.add(key + ':all'); rerenderCurrent(); } },
        `還有 ${lines.length - limit} 行，全部顯示`));
    }
    if (truncated) box.append(h('div', { class: 'h' }, '（diff 太長，只顯示前段；完整內容請直接開檔案）'));
    if (!diff) box.append(h('div', { class: 'h' }, '（沒有內容差異）'));
    return box;
  }

  function rerenderMsg(id) {
    const list = S.msgs[S.current] || [];
    const i = list.findIndex((x) => x.id === id);
    if (i < 0) return;
    const el = chat.list.querySelector(`[data-id="${CSS.escape(id)}"]`);
    if (el) el.replaceWith(renderMsg(list[i], list[i - 1] || null));
  }
  function rerenderCurrent() {
    const top = chat.scroll ? chat.scroll.scrollTop : 0;
    renderMessages(false);
    if (chat.scroll) chat.scroll.scrollTop = top;
  }

  function approvalCard(m, b) {
    const pending = b.state === 'pending';
    const allowed = b.state === 'allow' || b.state === 'allowSession';
    const color = pending ? 'yellow' : allowed ? 'green' : b.state === 'cancelled' ? '' : 'red';
    const agentLabel = displayName(AGENTS[b.agent] ? b.agent : 'claude');
    const card = h('div', { class: `embed ${color}` },
      h('div', { class: 'embed-title' }, icon(b.kind === 'plan' ? 'list' : 'shield', 17), (b.title || '').replace(/^(Claude|GPT)(?=s)/, agentLabel)),
      h('div', { class: 'embed-sub' }, pending ? `${agentLabel} 正在等你決定` : '權限請求'));
    if (b.command) {
      card.append(h('div', { class: 'embed-field' }, h('div', { class: 'embed-field-name' }, '指令' + (b.cwd ? `（在 ${b.cwd}）` : '')),
        h('div', { class: 'codeblock' }, h('pre', {}, h('code', {}, b.command)))));
    }
    if (b.path) {
      card.append(h('div', { class: 'embed-field' }, h('div', { class: 'embed-field-name' }, '檔案'),
        h('div', { class: 'mono', style: { fontSize: '13px' } }, b.path,
          b.added != null ? h('span', { style: { marginLeft: '8px' } }, h('span', { style: { color: '#3ba55c' } }, '+' + b.added), ' ', h('span', { style: { color: 'var(--red)' } }, '−' + b.removed)) : null)));
    }
    if (b.diff) card.append(diffView(b.diff, false, m.id + ':' + b.id + ':diff'));
    if (b.files && b.files.length) {
      const box = h('div', { class: 'activity', style: { marginTop: '8px' } });
      for (const f of b.files) box.append(...fileRow(m, { ...f, id: b.id + ':' + f.path, type: 'file' }));
      card.append(box);
    }
    if (b.plan) card.append(h('div', { class: 'embed-field' }, h('div', { class: 'msg-body', html: MD.render(b.plan) })));
    if (b.detail && !b.command) card.append(h('div', { class: 'embed-field' }, h('div', { class: 'embed-field-name' }, '內容'), h('div', { class: 'mono', style: { fontSize: '13px' } }, b.detail)));
    if (b.input) card.append(h('div', { class: 'embed-field' }, h('div', { class: 'embed-field-name' }, '參數'), h('div', { class: 'codeblock' }, h('pre', {}, b.input))));
    if (b.note) card.append(h('div', { class: 'notice-line' }, icon('info', 15), b.note));
    if (b.description && b.description !== b.detail) card.append(h('div', { class: 'notice-line' }, icon('info', 15), b.description));
    if (b.reason) card.append(h('div', { class: 'notice-line' }, icon('info', 15), '原因：' + b.reason));
    if (b.blockedPath) card.append(h('div', { class: 'notice-line warn' }, icon('alert', 15), '涉及專案外的路徑：' + b.blockedPath));

    if (pending) {
      const fb = h('textarea', { class: 'feedback', rows: 1, placeholder: `（選填）拒絕時告訴 ${agentLabel} 原因或該怎麼做` });
      fb.value = S.feedback[b.id] || '';
      fb.addEventListener('input', () => { S.feedback[b.id] = fb.value; });
      const respond = (choice) => {
        send({ type: 'approval', convId: S.current, msgId: m.id, blockId: b.id, choice, feedback: fb.value.trim() });
        delete S.feedback[b.id];
      };
      card.append(fb, h('div', { class: 'actions' },
        (b.options || []).map((o) => h('button', { class: `btn sm ${o.style || 'secondary'}`, onclick: () => respond(o.id) }, o.label))));
    } else {
      const label = { allow: '已允許', allowSession: '已允許（本次對話不再詢問同類操作）', deny: '已拒絕', abort: '已拒絕並停止', cancelled: '已取消（AI 不再等待這個請求）' }[b.state] || b.state;
      card.append(h('div', { class: `resolved ${allowed ? 'ok' : b.state === 'cancelled' ? '' : 'no'}` },
        icon(allowed ? 'check' : b.state === 'cancelled' ? 'info' : 'x', 15), label, b.feedback ? '：' + b.feedback : ''));
    }
    return card;
  }

  function questionCard(m, b) {
    const pending = b.state === 'pending';
    const agentLabel = displayName(AGENTS[b.agent] ? b.agent : 'claude');
    const card = h('div', { class: `embed ${pending ? 'blue' : ''}` },
      h('div', { class: 'embed-title' }, icon('question', 17), `${agentLabel} 想問你`));
    const ans = S.answers[b.id] || (S.answers[b.id] = {});
    const keyOf = (q) => q.id || q.question;
    const submitBtn = h('button', { class: 'btn sm primary' }, '送出回答');
    const refresh = () => {
      submitBtn.disabled = !(b.questions || []).every((q) => {
        const v = ans[keyOf(q)];
        return Array.isArray(v) ? v.length > 0 : !!(v && String(v).trim());
      });
    };
    for (const q of b.questions || []) {
      const k = keyOf(q);
      const qb = h('div', { class: 'q-block' }, q.header ? h('span', { class: 'q-head' }, q.header) : null, h('div', { class: 'q-text' }, q.question));
      if (pending) {
        const opts = h('div', { class: 'q-opts' });
        const other = h('input', { class: 'q-other', placeholder: '其他（自己輸入）', type: q.isSecret ? 'password' : 'text' });
        const paint = () => {
          for (const btn of opts.children) {
            const v = ans[k];
            const sel = Array.isArray(v) ? v.includes(btn.dataset.label) : v === btn.dataset.label;
            btn.classList.toggle('sel', sel);
          }
        };
        for (const o of q.options || []) {
          opts.append(h('button', {
            class: 'q-opt', 'data-label': o.label, onclick: () => {
              if (q.multiSelect) {
                const cur = Array.isArray(ans[k]) ? ans[k] : [];
                ans[k] = cur.includes(o.label) ? cur.filter((x) => x !== o.label) : [...cur, o.label];
              } else {
                ans[k] = o.label;
                other.value = '';
              }
              paint();
              refresh();
            },
          }, h('div', {}, o.label), o.description ? h('div', { class: 'q-desc' }, o.description) : null));
        }
        if (typeof ans[k] === 'string' && !(q.options || []).some((o) => o.label === ans[k])) other.value = ans[k];
        other.addEventListener('input', () => {
          if (other.value.trim()) ans[k] = q.multiSelect ? [other.value.trim()] : other.value;
          else delete ans[k];
          paint();
          refresh();
        });
        paint();
        qb.append(opts, other);
      } else {
        const a = (b.answers || {})[k];
        qb.append(h('div', { class: 'muted', style: { fontSize: '14px' } }, '你的回答：' + (Array.isArray(a) ? a.join('、') : a || '（未回答）')));
      }
      card.append(qb);
    }
    if (pending) {
      submitBtn.addEventListener('click', () => {
        send({ type: 'question', convId: S.current, msgId: m.id, blockId: b.id, answers: ans });
        delete S.answers[b.id];
      });
      refresh();
      card.append(h('div', { class: 'actions' }, submitBtn));
    } else if (b.state === 'cancelled') {
      card.append(h('div', { class: 'resolved' }, icon('info', 15), '已取消'));
    }
    return card;
  }

  function todoCard(b) {
    const items = b.items || [];
    const done = items.filter((i) => i.status === 'completed').length;
    return h('div', { class: 'embed blue' },
      h('div', { class: 'embed-title' }, icon('list', 16), '待辦清單', h('span', { class: 'muted', style: { fontWeight: 500, fontSize: '13px' } }, `${done}/${items.length}`)),
      h('div', { style: { marginTop: '6px' } }, items.map((i) => h('div', { class: `todo-item ${i.status}` },
        icon(i.status === 'completed' ? 'check' : i.status === 'inProgress' ? 'dot' : 'circle', 16), i.text))));
  }

  // Re-worded from the structured fields so renamed AIs show their current names.
  function handoffText(m) {
    if (!m.from || !m.to || !m.reason) return m.text || '';
    const a = displayName(m.from), b = displayName(m.to);
    switch (m.reason) {
      case 'first': return `${a} 把第一版交給 ${b} 審查`;
      case 'changed': return `${a} 修改了 ${m.files || 0} 個檔案，交給 ${b} 複審`;
      case 'answered': return `${a} 回應了 ${b} 的疑慮（沒有修改），交給 ${b} 確認`;
      case 'issues': return `${a} 提出了疑慮，交給 ${b} 處理`;
      default: return m.text || '';
    }
  }

  function renderSystem(m) {
    const el = h('div', { class: 'sys', 'data-id': m.id });
    if (m.kind === 'artifact') { el.append(...renderBlocks(m)); return el; }
    const time = h('span', { class: 'msg-time' }, stamp(m.ts));
    if (m.kind === 'handoff') {
      el.append(h('span', { class: 'sys-icon blue' }, icon('arrow', 16)), h('span', {}, handoffText(m)), time);
    } else if (m.kind === 'paused') {
      const st = stateOf().group || {};
      const latest = (S.msgs[S.current] || []).filter((x) => x.kind === 'paused').pop();
      const live = st.paused && latest && latest.id === m.id;
      el.append(h('span', { class: 'sys-icon yellow' }, icon('alert', 16)), h('span', { style: { color: 'var(--text)' } }, m.text), time);
      if (live) {
        const guard = m.action === 'guard';
        el.append(h('div', { class: 'sys-block' }, h('div', { class: 'actions', style: { display: 'flex', gap: '8px', marginTop: '6px' } },
          h('button', { class: 'btn sm primary', onclick: () => send({ type: 'group.continue', id: S.current }) },
            m.action === 'more' ? '再來回 2 輪' : guard ? '再退回一次' : '重試這一輪'),
          guard ? h('button', { class: 'btn sm secondary', 'data-tip': '不再要求修正，照這則回覆繼續協作',
            onclick: () => send({ type: 'group.accept', id: S.current }) }, '接受並繼續') : null,
          h('button', { class: 'btn sm secondary', onclick: () => send({ type: 'group.end', id: S.current }) }, '結束協作'))));
      }
    } else if (m.kind === 'guard') {
      // CCA's compliance checks: what failed and whether the reply went back for a rewrite.
      el.append(h('span', { class: `sys-icon ${m.final ? 'red' : 'yellow'}` }, icon('shield', 16)),
        h('span', { style: { color: 'var(--text)' } }, m.text), time);
      const issues = m.issues || [];
      if (issues.length) {
        el.append(h('div', { class: 'sys-block' }, h('ul', { class: 'guard-list' }, issues.map((i) => h('li', {}, i.text)))));
      }
    } else if (m.kind === 'regression') {
      // The reviewer test guard: what the host measured after a reviewer edited files, and whether it undid them.
      const tone = { passed: 'green', reverted: 'red', 'restore-failed': 'red', 'gate-red': 'red' }[m.status] || 'yellow';
      const tag = m.gate ? { passed: '完成閘門通過', 'gate-red': '完成閘門未過' }[m.status] || '沒有驗證'
        : { passed: '測試通過', reverted: '已還原', 'baseline-red': '基準也是紅', 'restore-failed': '還原失敗' }[m.status] || '沒有驗證';
      el.append(h('span', { class: `sys-icon ${tone}` }, icon(m.status === 'passed' ? 'check' : 'shield', 16)),
        h('span', { class: 'skip-tag' }, tag),
        h('span', { style: { color: 'var(--text)' } }, m.text), time);
      if (m.tail && m.status !== 'passed') {
        el.append(h('details', { class: 'sys-block route-note' }, h('summary', {}, `測試指令 ${m.command || ''}（結束碼 ${m.afterExit}）的輸出結尾`),
          h('pre', { class: 'mono' }, m.tail)));
      }
    } else if (m.kind === 'skip') {
      // A member without quota is passed over; the tag keeps the reason visible after names change.
      el.classList.add('sys-skip');
      el.append(h('span', { class: 'sys-icon yellow' }, icon('arrow', 16)), h('span', { class: 'skip-tag' }, '跳過'),
        m.agent ? h('span', { class: `skip-who ${m.agent}` }, displayName(m.agent)) : null,
        h('span', { style: { color: 'var(--text)' } }, m.text), time);
    } else if (m.kind === 'summary') {
      const ok = m.outcome === 'consensus', quota = m.outcome === 'quota';
      el.append(h('span', { class: `sys-icon ${ok ? 'green' : quota ? 'red' : ''}` }, icon(ok ? 'check' : 'stop', 16)),
        h('span', { style: { color: 'var(--text)' } }, ok ? '協作完成：' : quota ? '額度用完，強制停止：' : '協作結束：', m.text),
        h('span', {}, ` · ${m.rounds || 0} 輪審查`), time);
      const files = m.files || [];
      const box = h('div', { class: `embed ${ok ? 'green' : ''}`, style: { maxWidth: '560px' } },
        h('div', { class: 'embed-title' }, files.length ? `共變更 ${files.length} 個檔案` : '沒有檔案變更',
          files.length ? h('span', { style: { fontWeight: 500, fontSize: '13px' } }, h('span', { style: { color: '#3ba55c' } }, ' +' + (m.added || 0)), ' ', h('span', { style: { color: 'var(--red)' } }, '−' + (m.removed || 0))) : null));
      for (const f of files) {
        box.append(h('div', { class: 'file-sum' }, h('span', { class: `kind` }, CHANGE[f.change] || '修改'),
          h('span', { class: 'p' }, '‎' + f.path), h('span', { class: 'plus' }, '+' + (f.added || 0)), h('span', { class: 'minus' }, '−' + (f.removed || 0))));
      }
      // Only the latest run can be reviewed as "this task"; older summaries stay a record.
      const latest = (S.msgs[S.current] || []).filter((x) => x.kind === 'summary').pop();
      if (files.length && latest && latest.id === m.id) {
        box.append(h('div', { class: 'actions' }, h('button', { class: 'btn sm secondary', onclick: () => openReview(conv(), 'task') },
          icon('diff', 14), '審查這次的變更')));
      }
      if (quota && latest && latest.id === m.id && stateOf().task?.status === 'interrupted') {
        box.append(h('div', { class: 'actions' }, h('button', { class: 'btn sm primary', onclick: () => send({ type: 'task.resume', id: S.current }) },
          icon('refresh', 14), '額度恢復後接續任務')));
      }
      el.append(h('div', { class: 'sys-block' }, box));
    } else if (m.kind === 'rollback') {
      el.append(h('span', { class: 'sys-icon blue' }, icon('refresh', 16)), h('span', { style: { color: 'var(--text)' } }, m.text), time);
      const changed = (m.restored || []).concat(m.removed || []);
      if (changed.length || m.backup) {
        el.append(h('div', { class: 'sys-block' },
          changed.length ? h('div', { class: 'muted', style: { fontSize: '13px' } }, changed.slice(0, 10).join('、') + (changed.length > 10 ? ' …' : '')) : null,
          m.backup ? h('div', { class: 'actions', style: { marginTop: '6px' } },
            h('button', { class: 'btn sm secondary', onclick: () => send({ type: 'rollback.undo', convId: S.current, backup: m.backup }) }, '復原檔案')) : null));
      }
    } else if (m.kind === 'error') {
      el.append(h('span', { class: 'sys-icon red' }, icon('alert', 16)), h('span', { style: { color: '#f59b9c' } }, m.text), time);
    } else {
      el.append(h('span', { class: 'sys-icon' }, icon('info', 16)), h('span', {}, m.text || ''), time);
    }
    return el;
  }

  // ================================================================ right panel
  // Wide windows dock the side panel; narrow ones show it as a drawer on demand.
  const panelDocked = () => window.innerWidth >= 1200;
  function panelVisible() { return panelDocked() ? S.panelOpen : !!S.panelDrawer; }
  function togglePanel() {
    if (panelDocked()) { S.panelOpen = !S.panelOpen; store.set('panelOpen', S.panelOpen); }
    else S.panelDrawer = !S.panelDrawer;
    renderAll();
  }
  function renderPanel() {
    const p = $('#panel');
    const c = conv();
    // The review pane needs the width; the side panel comes back when it closes.
    const show = S.view === 'chat' && c && panelVisible() && !(S.review && S.review.convId === c.id) && !git.isOpen(c.id);
    p.classList.toggle('collapsed', !show);
    p.classList.toggle('drawer', !!show && !panelDocked());
    clear(p);
    if (!show) return;
    if (c.kind === 'group') renderGroupPanel(p, c); else renderAgentPanel(p, c);
    if (window.CCAQuality) p.append(window.CCAQuality.controls(c));
    if (window.CCACollaboration) p.append(window.CCACollaboration.controls(c));
  }

  function meter(label, pct, right) {
    const v = Math.max(0, Math.min(100, Math.round(pct || 0)));
    return h('div', { class: 'meter' },
      h('div', { class: 'meter-top' }, h('span', {}, label), h('span', {}, right || v + '%')),
      h('div', { class: 'meter-bar' }, h('div', { class: `meter-fill${v >= 90 ? ' full' : v >= 70 ? ' warn' : ''}`, style: { width: v + '%' } })));
  }

  function usageBlock(agent) {
    const u = S.usage[agent] || {};
    const out = [];
    if (agent === 'codex') {
      const l = u.limits || {};
      if (l.primary) out.push(meter('5 小時額度', l.primary.usedPercent, `已用 ${l.primary.usedPercent}% · ${untilReset(l.primary.resetsAt)}`));
      if (l.secondary) out.push(meter('每週額度', l.secondary.usedPercent, `已用 ${l.secondary.usedPercent}% · ${untilReset(l.secondary.resetsAt)}`));
    } else if (agent === 'gemini') {
      const quota = u.quota || {};
      for (const b of quota.buckets || []) {
        const label = [b.modelId, b.tokenType].filter(Boolean).join(' · ') || 'Google 額度';
        const remaining = b.remainingFraction == null ? '剩餘比例未知' : `剩餘 ${Math.round(b.remainingFraction * 100)}%`;
        const detail = remaining + (b.remainingAmount == null ? '' : ` · 剩餘量 ${b.remainingAmount}`) +
          (b.resetTime ? ' · 重置 ' + new Date(b.resetTime).toLocaleString('zh-TW') : '');
        if (b.remainingFraction == null) out.push(h('div', { class: 'hint' }, label + '：' + detail));
        else out.push(meter(label, (1 - b.remainingFraction) * 100, detail));
      }
      if (quota.error || !quota.buckets?.length) out.push(h('div', { class: 'hint' }, quota.error || '剩餘額度未知，請按重新檢查。'));
      if (quota.checkedAt) out.push(h('div', { class: 'hint' }, '查詢時間：' + new Date(quota.checkedAt).toLocaleTimeString('zh-TW')));
    } else {
      const names = { five_hour: '5 小時額度', seven_day: '每週額度', seven_day_opus: '每週 Opus 額度', seven_day_sonnet: '每週 Sonnet 額度' };
      for (const [k, v] of Object.entries(u.limits || {})) {
        let pct = v.utilization;
        if (pct != null && pct <= 1) pct *= 100;
        const state = v.status === 'rejected' ? '已達上限' : v.status === 'allowed_warning' ? '接近上限' : '正常';
        out.push(meter(names[k] || k, pct != null ? pct : (v.status === 'rejected' ? 100 : 0),
          (pct != null ? `已用 ${Math.round(pct)}% · ` : state + ' · ') + untilReset(v.resetsAt)));
      }
      if (!out.length) out.push(h('div', { class: 'muted', style: { fontSize: '13px' } }, 'Claude 會在對話進行時回報額度狀態。'));
      if (u.live) out.push(h('div', { class: 'hint' },
        (u.live.ok ? '額度查詢時間：' : '額度查詢失敗，保留上次數值：') + new Date(u.live.checkedAt).toLocaleTimeString('zh-TW') +
        (u.live.error ? ' · ' + u.live.error : '')));
    }
    const s = u.session;
    if (s && s.turns) {
      out.push(h('div', { class: 'kv' }, h('span', {}, '這次開啟後'), h('span', {}, `${s.turns} 次回覆 · 輸入 ${tok(s.inputTokens)} · 輸出 ${tok(s.outputTokens)}`)));
      if (s.costUsd) out.push(h('div', { class: 'kv', 'data-tip': '等值 API 價格的估算。訂閱用戶不會另外被收費。' }, h('span', {}, '等值 API 估算'), h('span', {}, 'US$' + s.costUsd.toFixed(2))));
    }
    if (u.context && u.context.window && u.context.convId === S.current) {
      out.push(meter('上下文使用量', u.context.used / u.context.window * 100, `${tok(u.context.used)} / ${tok(u.context.window)}`));
    }
    return out;
  }

  function permList(c) {
    return Object.entries(PERMS).map(([k, p]) => h('button', {
      class: `perm-opt${c.permission === k ? ' sel' : ''}${p.danger ? ' danger' : ''}`, onclick: () => setPermission(c, k),
    }, icon(p.icon, 18), h('div', {}, h('div', { class: 'pt' }, p.label), h('div', { class: 'pd' }, p.desc),
      h('div', { class: 'pd', style: { marginTop: '2px', color: 'var(--text-faint)' } }, c.kind === 'group' ? [...new Set(c.pipeline?.nodes?.map(n => n.backend) || ['claude', 'codex'])].map(a => p[a]).filter(Boolean).join('；') : p[c.kind])),
    h('span', { class: 'radio' })));
  }

  function renderAgentPanel(p, c) {
    const agent = c.kind;
    const a = AGENTS[agent];
    const au = S.auth[agent] || {};
    const okPlan = au.state === 'ok';
    p.append(
      h('div', { class: 'profile-banner', style: bannerStyle(agent) }, avatar(agent, 88, agentStatus(agent, c.id))),
      h('div', { class: 'profile-body' },
        h('div', { class: 'profile-name' }, convTitle(c)),
        h('div', { class: 'profile-handle' }, `${a.vendor} · ${a.cli}${au.version ? ' ' + au.version : ''}`,
          h('span', { class: `plan-badge ${okPlan ? 'ok' : 'bad'}` }, au.planLabel || (okPlan ? '已訂閱' : '未通過檢查'))),
        h('div', { class: 'pe-row', style: { marginTop: '12px' } },
          h('button', { class: 'btn sm secondary', onclick: () => openSettings(agent) }, icon('pencil', 14), '編輯個人資料與性格'),
          personaOf(agent) ? h('span', { class: 'tag' }, '性格：' + personaOf(agent).name) : null),
        h('div', { class: 'card' }, h('div', { class: 'card-title' }, '目前設定'),
          agent === 'gemini' ? h('div', { class: 'kv' }, h('span', {}, '模型'), h('span', {}, 'Gemini CLI 預設（CLI 未提供模型目錄）')) : [
            c.daily ? h('div', { class: 'kv' }, h('span', {}, '日常模式'), h('span', {}, modelWithEffort(agent, dailyPick(agent).model, dailyPick(agent).effort))) : null,
            h('div', { class: 'kv' }, h('span', {}, c.daily ? '工作模式的模型' : '模型'), h('span', {}, modelLabel(agent, agent === 'claude' ? c.claudeModel : c.codexModel))),
            h('div', { class: 'kv' }, h('span', {}, '思考程度'), h('span', {}, effortLabel(agent === 'claude' ? c.claudeEffort : c.codexEffort) || '模型預設'))],
          h('div', { class: 'kv' }, h('span', {}, '專案資料夾'), h('span', { 'data-tip': c.cwd }, c.cwd || '未選擇'))),
        h('div', { class: 'card' }, h('div', { class: 'card-title' }, '用量', h('button', { class: 'act', onclick: () => send({ type: agent === 'gemini' ? 'auth.check' : 'usage.refresh', agent }) }, '更新')), usageBlock(agent)),
        h('div', { class: 'card' }, h('div', { class: 'card-title' }, '權限模式'), permList(c)),
        agent === 'gemini' ? null : h('div', { class: 'card' }, h('div', { class: 'card-title' }, '插件', h('button', { class: 'act', onclick: () => { S.pluginTab = agent; openView('plugins'); } }, '管理')),
          h('div', { class: 'muted', style: { fontSize: '13px' } }, pluginSummary(agent))),
      ),
    );
  }
  function pluginSummary(agent) {
    const pl = S.plugins[agent];
    const mcp = S.mcp[agent];
    const parts = [];
    if (pl && pl.installed) parts.push(`已安裝 ${pl.installed.length} 個插件`);
    if (mcp && mcp.items) parts.push(`${mcp.items.length} 個 MCP 伺服器`);
    return parts.length ? parts.join('，') : '在插件商店可以替這個 AI 安裝插件和 MCP 伺服器。';
  }

  // The shared task record (src/task_ledger.cpp): every handoff with its revisions, files and tests.
  const TASK_STATUS = { running: '進行中', paused: '暫停', interrupted: '上次中斷', done: '已完成', stopped: '已停止', failed: '沒有完成' };
  function taskCard(c, t) {
    if (!t || !t.task_id) return null;
    const vtext = { write: '', approve: '同意', changed: '已修改', issues: '有疑慮' };  // a write turn has no verdict
    const box = h('div', { class: 'card task-card' },
      h('div', { class: 'card-title' }, '任務紀錄', h('span', { class: `tag${t.status === 'interrupted' ? ' warn' : ''}` }, TASK_STATUS[t.status] || t.status)));
    if (t.status === 'interrupted') {
      const q = t.quotaStop;
      box.append(h('div', { class: 'muted', style: { fontSize: '12px', margin: '2px 0 8px' } },
        q ? `所有 AI 額度用完而強制停止（${new Date(q.at).toLocaleString('zh-TW')}）。下一步記錄為 ${q.next}；額度恢復後接續，從這一步開始。`
          : 'CCA 在這個任務進行中關閉了。接續時，下一位會拿到結構化的交接紀錄，先檢查實際檔案再繼續。'),
        q && q.resumeAt > 0 ? h('div', { class: 'resume-plan' }, icon('clock', 14), `將於 ${new Date(q.resumeAt).toLocaleString('zh-TW', { month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit' })} 自動接續`,
          h('button', { class: 'link-btn', onclick: () => send({ type: 'task.autoResume', id: c.id, enabled: false }) }, '取消自動接續'))
          : q && S.settings.autoResume?.enabled ? h('button', { class: 'link-btn', onclick: () => send({ type: 'task.autoResume', id: c.id, enabled: true }) }, '排定額度恢復後自動接續') : '',
        h('button', { class: 'btn sm primary', onclick: () => send({ type: 'task.resume', id: c.id }) }, icon('refresh', 14), '接續任務'));
    }
    if (t.earlier) box.append(h('div', { class: 'muted', style: { fontSize: '12px' } }, `較早的 ${t.earlier} 次交接只保留在紀錄檔`));
    for (const x of t.handoffs || []) {
      const rev = x.base ? x.base.slice(0, 7) + (x.head ? '→' + x.head.slice(0, 7) : '') : '';
      box.append(h('div', { class: 'task-row', 'data-tip': x.next ? '下一步：' + x.next : null },
        avatar((t.pipeline?.nodes || c.pipeline?.nodes || []).find(n => n.id === x.agent)?.backend || x.agent, 18),
        h('span', {}, '#' + x.seq + ' ' + (PHASES[x.role] || x.role)),
        h('span', { class: 'muted' }, [x.verdict ? (x.verdict in vtext ? vtext[x.verdict] : x.verdict) : '', x.files ? x.files + ' 個檔案' : '',
          x.tests ? `測試 ${x.tests - x.failed}/${x.tests}` : ''].filter(Boolean).join(' · ')),
        rev ? h('span', { class: 'mono task-rev' }, rev) : null,
        x.failed ? h('span', { class: 'minus' }, '測試失敗') : null));
    }
    return box;
  }

  function pipelineName(node) {
    const profile = AGENTS[node.backend] ? displayName(node.backend) : '';
    return node.name && node.name !== profile ? node.name + (profile ? ' · ' + profile : '') : profile || node.id;
  }

  function renderGroupPanel(p, c) {
    if (c.pipeline?.nodes?.length) {
      const st = stateOf(c.id), task = st.task || {};
      const nodes = task.pipeline?.nodes || c.pipeline.nodes;
      p.append(h('div', { class: 'member-head' }, '參與代理 — ' + nodes.length));
      nodes.forEach((node, index) => {
        const known = !!AGENTS[node.backend];
        const title = pipelineName(node);
        const details = (PHASES[node.role] || node.role) + ' · ' +
          (node.model || (node.backend === 'gemini' ? 'Gemini CLI 預設模型' : node.backend)) +
          (node.backend === 'api' ? ' · API 計費' : node.backend === 'local' ? ' · 唯讀' : '') +
          (st.running && task.cursor === index ? ' · 目前代理' : '');
        p.append(h(known ? 'button' : 'div', { class: 'member' + (known ? ' member-link' : ''),
          type: known ? 'button' : null, 'data-node-id': node.id,
          'aria-label': known ? '查看 ' + title + ' 的資料卡' : null,
          onclick: known ? e => profileDialog(node.backend, c.id, e.currentTarget) : null },
        avatar(node.backend, 32, known ? agentStatus(node.backend, c.id) : null),
        h('span', { class: 'tag' }, String(index + 1)),
        h('div', { class: 'member-info' }, h('div', { class: 'mn ' + node.backend }, title), h('div', { class: 'ms' }, details))));
      });
      p.append(...[window.CCAPlatform.controls(c, task), timelineCard(c, st), taskCard(c, task)].filter(Boolean));
      const quota = [...new Set(nodes.map(n => n.backend))].filter(a => ['claude', 'codex', 'gemini'].includes(a));
      if (quota.length) p.append(h('div', { class: 'card' }, h('div', { class: 'card-title' }, '帳號額度'),
        quota.map(a => h('div', { class: 'pipeline-quota' }, h('div', { class: 'embed-field-name' }, displayName(a)), usageBlock(a)))));
      return;
    }
    const st = stateOf(c.id);
    const g = st.group || {};
    const typing = st.typing || [];
    const member = (kind, name, sub) => h('button', { class: 'member member-link', type: 'button', 'aria-label': '查看 ' + name + ' 的資料卡', onclick: (e) => profileDialog(kind, c.id, e.currentTarget) },
      kind === 'user' ? avatar('user', 32, 'ok') : avatar(kind, 32, agentStatus(kind, c.id)),
      h('div', {}, h('div', { class: `mn ${kind}` }, name), h('div', { class: 'ms' }, sub)));
    // Members grouped by role, the way Discord groups a member list.
    const lead = c.lead || 'claude', second = lead === 'claude' ? 'codex' : 'claude';
    const doing = (a) => typing.includes(a) ? (PHASES[g.phase] || '處理') + '中…'
      : c.exhausted?.[a] || (a === 'claude' && c.claudeLimited) ? '額度不足 · 輪到時跳過'
        : a === 'gemini' ? (S.auth.gemini?.state === 'ok' ? 'Gemini CLI 預設模型' : stateText(S.auth.gemini?.state) + ' · 輪到時跳過')
          : modelLabel(a, a === 'claude' ? c.claudeModel : c.codexModel);
    const group = (title, items) => (items.length ? [h('div', { class: 'member-head' }, `${title} — ${items.length}`), ...items] : []);
    p.append(...group('指揮官', [member('user', displayName('user'), '下指令')]),
      ...group('主刀手', [member(lead, displayName(lead), doing(lead))]),
      ...group('審查者', [member(second, displayName(second), doing(second)),
        c.geminiMember ? member('gemini', displayName('gemini'), doing('gemini')) : null].filter(Boolean)),
      ...group('唯讀審查員', (c.localMembers || []).map((l) => member(l, displayName(l),
        typing.includes(l) ? '唯讀審查中…' : (((S.settings.locals || {})[l] || {}).model || '尚未設定模型')))));
    // append() would print a null child as the text "null".
    if (window.CCAPlatform) p.append(...[window.CCAPlatform.controls(c, st.task)].filter(Boolean));
    // Settings (lead, models, permissions, local reviewers, round limit) live in the channel settings panel.
    const sec = h('div', { class: 'panel-section' });
    sec.append(...[timelineCard(c, st), taskCard(c, st.task)].filter(Boolean));
    sec.append(h('div', { class: 'card' }, h('div', { class: 'card-title' }, '額度'),
      h('div', { class: 'embed-field-name' }, 'Claude'), usageBlock('claude'), h('div', { class: 'embed-field-name', style: { marginTop: '8px' } }, 'GPT'), usageBlock('codex')));
    p.append(sec);
  }

  // The cross-review timeline of the latest run: who did each turn, the verdict, and how much each
  // turn changed (bar length is relative to the largest turn).
  function timelineCard(c, st) {
    const g = st.group || {};
    const msgs = S.msgs[c.id] || [];
    let start = 0;
    for (let i = msgs.length - 1; i >= 0; i--) if (msgs[i].author === 'user') { start = i; break; }
    const turns = msgs.slice(start).filter((m) => m.phase);
    const vtext = { approve: '同意', changed: '已修改', issues: '有疑慮' };
    const count = (m) => (m.blocks || []).filter((b) => b.type === 'file')
      .reduce((n, f) => ({ add: n.add + (f.added || 0), rem: n.rem + (f.removed || 0), files: n.files + 1 }), { add: 0, rem: 0, files: 0 });
    const most = Math.max(1, ...turns.map((m) => { const n = count(m); return n.add + n.rem; }));
    const changed = turns.some((m) => count(m).files);
    const box = h('div', { class: 'card timeline' }, h('div', { class: 'card-title' }, g.active ? `互審中 · 第 ${g.round} 輪` : '互審時間軸',
      changed ? h('button', { class: 'act', onclick: () => openReview(c, 'task') }, '審查變更') : null));
    if (!turns.length) {
      box.append(h('div', { class: 'muted', style: { fontSize: '13px' } }, '還沒有開始。下一個指令會先交給 ' + (c.pipeline?.nodes?.length ? pipelineName(c.pipeline.nodes[0]) : displayName(c.lead || 'claude')) + '。'));
      return box;
    }
    for (const m of turns) {
      const n = count(m);
      const state = m.streaming ? '進行中' : m.status === 'failed' ? '失敗' : m.verdict ? vtext[m.verdict] : n.files ? `${n.files} 個檔案` : '完成';
      box.append(h('button', { class: `tl-row${m.verdict ? ' v-' + m.verdict : ''}${m.streaming ? ' live' : ''}`, type: 'button',
        'aria-label': `${displayName(m.author)} · ${PHASES[m.phase] || m.phase} · ${state} · 新增 ${n.add} 行、刪除 ${n.rem} 行`,
        'data-tip': `${displayName(m.author)} · ${PHASES[m.phase] || m.phase}${m.durationMs ? ' · ' + dur(m.durationMs) : ''}`, onclick: () => jumpToMsg(m.id) },
      avatar(m.author, 18),
      h('span', { class: 'tl-phase' }, (PHASE_SHORT[m.phase] || m.phase) + (m.round ? ' ' + m.round : '')),
      h('span', { class: 'tl-bar' }, h('i', { class: m.author, style: { width: Math.max(n.add + n.rem ? 6 : 0, Math.round((n.add + n.rem) / most * 100)) + '%' } })),
      h('span', { class: 'tl-state' }, state),
      n.add + n.rem ? h('span', { class: 'tl-lines' }, h('span', { class: 'plus' }, '+' + n.add), h('span', { class: 'minus' }, '−' + n.rem)) : null));
    }
    return box;
  }

  function jumpToMsg(id) {
    const el = chat.list && chat.list.querySelector(`[data-id="${CSS.escape(id)}"]`);
    if (!el) return;
    el.scrollIntoView({ block: 'center', behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth' });
    el.classList.remove('flash');
    void el.offsetWidth;
    el.classList.add('flash');
  }

  // ================================================================ review pane
  // What changed since a checkpoint (src/review.cpp), file by file. The commander can put back a
  // file or one hunk, and leave comments on lines that go to the agent as one message.
  function loadReviewDraft(id) {
    if (S.reviewNotes[id]) return;
    const saved = store.get('review.' + id, {});
    S.reviewNotes[id] = (Array.isArray(saved?.notes) ? saved.notes : []).filter(n =>
      n && typeof n.path === 'string' && typeof n.body === 'string' && typeof n.quote === 'string' &&
      ['old', 'new'].includes(n.side) && Number.isInteger(n.line));
    S.reviewDrafts[id] = saved?.drafts && typeof saved.drafts === 'object' && !Array.isArray(saved.drafts) ? saved.drafts : {};
  }
  function saveReviewDraft(id) {
    S.reviewSaveFailed[id] = !store.set('review.' + id, { notes: S.reviewNotes[id] || [], drafts: S.reviewDrafts[id] || {} });
  }
  function openReview(c, scope = 'task') {
    if (!c) return;
    if (S.review?.reverting) { toast('info', '正在還原，完成後再切換範圍'); return; }
    loadReviewDraft(c.id);
    const previous = S.review?.convId === c.id ? S.review : {};
    const requestToken = String(++S.reviewRequest);
    if (S.git) git.close();
    S.review = { convId: c.id, scope, requestToken, loading: true, files: [], sel: previous.sel, query: previous.query || '', editing: null };
    send({ type: 'review.open', id: c.id, scope, requestToken });
    renderReview();
    renderPanel();
  }
  // The review and version-control panes share the chat area; opening one closes the other.
  function openGit(c) {
    if (S.review) { S.review = null; renderReview(); }
    git.open(c); renderPanel();
  }
  function closeReview() {
    S.review = null; renderReview(); renderPanel();
    chat.head?.querySelector('[aria-label="審查變更"]')?.focus();
  }

  function hunkStart(header) {
    const m = /@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@/.exec(header || '');
    return m ? { old: +m[1], neu: +m[2] } : { old: 0, neu: 0 };
  }

  function reviewRevert(c, r, file, hunk) {
    if (running(c.id)) { toast('info', 'AI 還在工作，這一輪結束後再還原'); return; }
    r.reverting = true;
    r.revertToken = String(++S.reviewRequest);
    renderReview();
    send({ type: 'review.revert', id: c.id, requestToken: r.revertToken, base: r.base, head: r.head,
      path: file.path, hunk, header: hunk >= 0 ? file.hunks[hunk].header : '' });
  }

  function sendReviewNotes(c) {
    const notes = S.reviewNotes[c.id] || [];
    if (!notes.length) return;
    if (S.reviewSending[c.id]) return;
    if (running(c.id)) { toast('info', '等這一輪結束再交給 AI'); return; }
    const lines = notes.map((n, i) => `${i + 1}. ${n.path} ${n.side === 'old' ? '原版本' : '修改後'}第 ${n.line} 行${n.quote ? `（\`${n.quote.trim().slice(0, 80)}\`）` : ''}：${n.body}`);
    const text = `【審查意見｜${displayName('user')} 在審查窗格留下 ${notes.length} 則】\n${lines.join('\n')}\n\n` +
      '先核對目前檔案與引用是否仍一致，再逐條處理；不同意的條目說明理由。';
    const pending = { text, notes: notes.slice() };
    S.reviewSending[c.id] = pending;
    renderReview();
    send({ type: 'conv.send', id: c.id, text });
    setTimeout(() => {
      if (S.reviewSending[c.id] !== pending) return;
      delete S.reviewSending[c.id];
      toast('info', '尚未確認意見送出，草稿已保留。可確認聊天紀錄後重試。');
      renderReview();
    }, 10000);
  }

  function fileDiffView(c, r, file, notes) {
    const box = h('div', { class: 'rv-file-view' },
      h('div', { class: 'rv-fhead' }, h('span', { class: `kind ${file.change}` }, CHANGE[file.change] || (file.change === 'binary' ? '二進位' : '修改')),
        h('span', { class: 'mono' }, file.path),
        h('button', { class: 'btn sm secondary', disabled: r.reverting, 'data-tip': '把整個檔案放回檢查點的版本（之後可以復原）',
          onclick: () => reviewRevert(c, r, file, -1) }, icon('refresh', 14), file.change === 'add' ? '移除這個新檔案' : '還原整個檔案')));
    if (file.change === 'binary') { box.append(h('div', { class: 'rv-empty' }, '二進位檔，無法顯示差異。')); return box; }
    file.hunks.forEach((hk, index) => {
      const start = hunkStart(hk.header);
      let oldNo = start.old, newNo = start.neu;
      const hunk = h('div', { class: 'rv-hunk' }, h('div', { class: 'rv-hh' }, h('span', {}, hk.header),
        h('button', { class: 'btn sm secondary', disabled: r.reverting, onclick: () => reviewRevert(c, r, file, index) }, '還原這段')));
      for (const raw of hk.lines) {
        const t = raw[0], text = raw.slice(1);
        if (t === '\\') { hunk.append(h('div', { class: 'rv-line meta' }, h('span'), h('span'), h('span'), h('span', { class: 't' }, text.trim()), h('span'))); continue; }
        const oldN = t === '+' ? '' : oldNo++, newN = t === '-' ? '' : newNo++;
        const side = t === '-' ? 'old' : 'new', line = t === '-' ? oldN : newN;
        const key = `${file.path}|${side}|${line}`;
        hunk.append(h('div', { class: `rv-line${t === '+' ? ' a' : t === '-' ? ' r' : ''}` },
          h('span', { class: 'n' }, oldN), h('span', { class: 'n' }, newN), h('span', { class: 's' }, t === ' ' ? '' : t),
          h('span', { class: 't' }, text),
          h('button', { class: 'add-note', type: 'button', 'aria-label': '在這一行留言', 'data-tip': '在這一行留言',
            onclick: () => { r.editing = key; renderReview(); } }, icon('plus', 14))));
        for (const [i, n] of notes.entries()) {
          if (`${n.path}|${n.side}|${n.line}` !== key) continue;
          hunk.append(h('div', { class: 'rv-note' }, icon('pencil', 14), h('span', {}, n.body),
            h('button', { class: 'icon-btn', 'aria-label': '刪除這則意見', onclick: () => { notes.splice(i, 1); saveReviewDraft(c.id); renderReview(); } }, icon('x', 14))));
        }
        if (r.editing === key) {
          const input = h('textarea', { class: 'input', rows: 2, value: S.reviewDrafts[c.id][key] || '',
            'aria-label': '逐行審查意見', placeholder: '這一行要怎麼改？Enter 加入，Shift + Enter 換行',
            oninput: (e) => { S.reviewDrafts[c.id][key] = e.target.value; saveReviewDraft(c.id); } });
          const add = () => {
            if (!input.value.trim()) return;
            notes.push({ path: file.path, side, line, quote: text, body: input.value.trim() });
            delete S.reviewDrafts[c.id][key];
            saveReviewDraft(c.id);
            r.editing = null;
            renderReview();
          };
          input.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) { e.preventDefault(); add(); }
            if (e.key === 'Escape') { e.stopPropagation(); r.editing = null; renderReview(); }
          });
          hunk.append(h('div', { class: 'rv-note-edit' }, input,
            h('div', { class: 'row' }, h('button', { class: 'btn sm primary', onclick: add }, '加入意見'),
              h('button', { class: 'btn sm ghost', onclick: () => { r.editing = null; renderReview(); } }, '取消'))));
          setTimeout(() => input.isConnected && input.focus(), 0);
        }
      }
      box.append(hunk);
    });
    if (file.cut) box.append(h('div', { class: 'rv-empty' }, '這個檔案的差異太長，只顯示前段；完整內容請直接開檔案。'));
    return box;
  }

  function renderReview() {
    const main = $('#main');
    let pane = main.querySelector('.review-pane');
    const r = S.review;
    if (!r || r.convId !== S.current || S.view !== 'chat') { if (pane) pane.remove(); return; }
    if (!pane) { pane = h('section', { class: 'review-pane', 'aria-label': '審查變更' }); main.append(pane); }
    const c = conv(r.convId);
    if (!c) { closeReview(); return; }
    const notes = S.reviewNotes[c.id] || (S.reviewNotes[c.id] = []);
    const files = r.files || [];
    const active = pane.contains(document.activeElement) ? document.activeElement : null;
    const focus = active?.getAttribute('aria-label');
    const selection = active && typeof active.selectionStart === 'number' ? [active.selectionStart, active.selectionEnd] : null;
    const filtered = files.filter(f => !r.query || f.path.toLocaleLowerCase().includes(r.query.toLocaleLowerCase()));
    // Keep the selection while a refresh is loading; pick the first file only once the list is known.
    if (!r.loading && !files.some((f) => f.path === r.sel)) r.sel = files.length ? files[0].path : null;
    const file = filtered.find((f) => f.path === r.sel) || filtered[0];
    if (!r.loading && file) r.sel = file.path;
    const add = files.reduce((n, f) => n + (f.added || 0), 0), rem = files.reduce((n, f) => n + (f.removed || 0), 0);
    const scopes = [['turn', '這一輪'], ['task', '這次任務'], ['all', '整個對話']];
    const top = h('div', { class: 'rv-top' }, icon('diff', 18), h('span', { class: 'rv-title' }, '審查變更'),
      h('div', { class: 'seg', role: 'group', 'aria-label': '審查範圍' }, scopes.map(([v, label]) => h('button', {
        class: v === r.scope ? 'sel' : '', type: 'button', disabled: r.reverting, 'aria-label': label,
        'aria-pressed': String(v === r.scope), onclick: () => openReview(c, v) }, label))),
      h('span', { class: 'rv-sum' }, r.loading ? '正在比較檢查點…' : r.error ? '' : [`${files.length} 個檔案 `, h('span', { class: 'plus' }, '+' + add), ' ', h('span', { class: 'minus' }, '−' + rem)]),
      h('button', { class: 'icon-btn', disabled: r.reverting, 'aria-label': '重新整理', 'data-tip': '重新整理', onclick: () => openReview(c, r.scope) }, icon('refresh', 18)),
      h('button', { class: 'icon-btn', 'aria-label': '關閉審查窗格', 'data-tip': '關閉（Esc）', onclick: closeReview }, icon('x', 18)));
    const list = h('div', { class: 'rv-files', role: 'group', 'aria-label': '變更檔案' }, filtered.map((f) => {
      const count = notes.filter((n) => n.path === f.path).length;
      return h('button', { class: `rv-file${f.path === r.sel ? ' sel' : ''}`, type: 'button',
        'aria-label': f.path, 'aria-pressed': String(f.path === r.sel),
        onclick: () => { r.sel = f.path; r.editing = null; renderReview(); },
        onkeydown: (e) => {
          if (!['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(e.key)) return;
          e.preventDefault();
          const at = filtered.indexOf(f), index = e.key === 'Home' ? 0 : e.key === 'End' ? filtered.length - 1 :
            (at + (e.key === 'ArrowDown' ? 1 : -1) + filtered.length) % filtered.length;
          r.sel = filtered[index].path; r.editing = null; renderReview();
          [...pane.querySelectorAll('.rv-file')].find(el => el.getAttribute('aria-label') === r.sel)?.focus();
        } },
        h('span', { class: `kind ${f.change}` }, CHANGE[f.change] || (f.change === 'binary' ? '二進位' : '修改')),
        h('span', { class: 'p', 'data-tip': f.path }, '‎' + f.path),
        count ? h('span', { class: 'rv-count', 'data-tip': count + ' 則意見' }, count) : null,
        h('span', { class: 'plus' }, '+' + (f.added || 0)), h('span', { class: 'minus' }, '−' + (f.removed || 0)));
    }));
    const browser = h('div', { class: 'rv-browser' }, h('label', { class: 'rv-search' }, icon('search', 14),
      h('input', { class: 'input', type: 'search', value: r.query || '', 'aria-label': '篩選變更檔案', placeholder: '篩選檔案路徑',
        oninput: (e) => { r.query = e.target.value; renderReview(); } })), list);
    const body = h('div', { class: 'rv-diff' });
    if (r.error) body.append(h('div', { class: 'rv-empty' }, icon('alert', 18), r.error));
    else if (r.loading) body.append(h('div', { class: 'rv-empty' }, h('span', { class: 'spinner' }), '正在比較檢查點…'));
    else if (!files.length) body.append(h('div', { class: 'rv-empty' }, icon('check', 18), '這個範圍沒有檔案變更。'));
    else if (!filtered.length) body.append(h('div', { class: 'rv-empty' }, icon('search', 18), '沒有符合的檔案，請更換搜尋文字。'));
    else if (file) body.append(fileDiffView(c, r, file, notes));
    if (r.truncated) body.append(h('div', { class: 'rv-empty' }, '變更太多，只列出前面的檔案。'));
    const target = c.kind === 'group' ? displayName(c.lead || 'claude') : convTitle(c);
    const overview = notes.length ? h('details', { class: 'rv-notes' }, h('summary', {}, `${notes.length} 則已保存的審查意見`),
      notes.map((n, i) => h('div', { class: 'rv-note-summary' },
        h('strong', { class: 'mono' }, `${n.path} · ${n.side === 'old' ? '原版本' : '修改後'} ${n.line}`), h('span', {}, n.body),
        h('button', { class: 'icon-btn', 'aria-label': '刪除意見 ' + (i + 1),
          onclick: () => { notes.splice(i, 1); saveReviewDraft(c.id); renderReview(); } }, icon('x', 14))))) : null;
    const foot = h('div', { class: 'rv-foot' }, overview,
      h('span', { class: S.reviewSaveFailed[c.id] ? 'muted warn-text' : 'muted', role: 'status' },
        S.reviewSaveFailed[c.id] ? '本機儲存不可用，意見暫存在記憶體；離開程式前請先送出或複製。' :
          notes.length ? '草稿保存在本機。送出時會附上版本與行號。' : '在任一行按「＋」留言；還原前會先存檢查點，可從聊天復原。'),
      window.CCAQuality ? h('button', { class: 'btn sm secondary', onclick: () => window.CCAQuality.evidenceDialog(c,
        notes.map(n => n.path + ':' + n.line + ' ' + n.body).join('\n') || r.sel || '審查疑慮', true) }, '重現這項疑慮') : null,
      h('button', { class: 'btn sm primary', disabled: !notes.length || !!S.reviewSending[c.id] || running(c.id), onclick: () => sendReviewNotes(c) },
        S.reviewSending[c.id] ? '正在送出…' : `交給 ${target} 處理`));
    const scroll = pane.querySelector('.rv-diff')?.scrollTop || 0;
    put(pane, top, browser, body, foot);
    if (r.sel === pane.dataset.sel) body.scrollTop = scroll;
    pane.dataset.sel = r.sel || '';
    if (focus) {
      const replacement = [...pane.querySelectorAll('[aria-label]')].find(el => el.getAttribute('aria-label') === focus);
      replacement?.focus();
      if (selection && replacement?.tagName === 'TEXTAREA') replacement.setSelectionRange(...selection);
    }
  }

  // ================================================================ pickers & dialogs
  let popEl = null, popAnchor = null;
  function closePop(restoreFocus = false) {
    if (popEl) { popEl.remove(); popEl = null; }
    if (restoreFocus && popAnchor?.isConnected) popAnchor.focus();
    popAnchor = null;
  }
  function popover(anchor, content, cls) {
    // Measure first: the anchor may sit inside the popover that closePop() is about to remove.
    const r = anchor.getBoundingClientRect();
    closePop();
    popAnchor = anchor;
    popEl = h('div', { class: 'popover' + (cls ? ' ' + cls : ''), role: 'dialog',
      'aria-label': cls?.includes('ops') ? '頻道設定' : '選項' }, content);
    $('#layers').append(popEl);
    const w = popEl.offsetWidth, hh = popEl.offsetHeight;
    popEl.style.left = Math.max(8, Math.min(window.innerWidth - w - 8, r.left)) + 'px';
    popEl.style.top = (r.bottom + 6 + hh > window.innerHeight ? Math.max(8, r.top - hh - 6) : r.bottom + 6) + 'px';
    const current = popEl;
    setTimeout(() => {
      if (popEl === current) current.querySelector('input:not([disabled]), button:not([disabled]), select:not([disabled])')?.focus();
    }, 0);
  }
  document.addEventListener('mousedown', (e) => {
    if (popEl && !popEl.contains(e.target)) closePop();
  }, true);

  function modelPicker(anchor, c, agent) {
    const list = S.models[agent] && S.models[agent].length ? S.models[agent] : (agent === 'claude' ? FALLBACK_CLAUDE_MODELS : []);
    const cur = (agent === 'claude' ? c.claudeModel : c.codexModel) || '';
    const curEffort = (agent === 'claude' ? c.claudeEffort : c.codexEffort) || '';
    const key = agent === 'claude' ? 'claudeModel' : 'codexModel';
    const ekey = agent === 'claude' ? 'claudeEffort' : 'codexEffort';
    const info = modelInfo(agent, cur);
    const efforts = effortsFor(agent, info);
    const set = (settings) => { send({ type: 'routing.manual', id: c.id, agent, settings }); closePop(); };
    const items = list.map((m) => {
      const v = m.value === 'default' ? '' : (m.value || '');
      const sel = !c.adaptive?.[agent]?.model && (v === cur || (!cur && (m.value === 'default' || m.isDefault)));
      // Keep the thinking level when the new model supports it.
      const keep = effortsFor(agent, m).includes(curEffort) ? curEffort : '';
      return h('button', { class: `pop-item${sel ? ' sel' : ''}`, onclick: () => set({ [key]: v, ...(c.adaptive?.[agent]?.effort ? {} : { [ekey]: keep }) }) },
        h('div', {}, h('div', {}, m.displayName || m.value || '預設'), m.description ? h('div', { class: 'pd' }, m.description) : null),
        sel ? h('span', { class: 'tick' }, icon('check', 16)) : null);
    });
    if (!list.length) items.push(h('div', { class: 'pop-item' }, h('div', { class: 'pd' }, '還沒拿到模型清單，請稍候或重新檢查登入。')));
    const effortItem = (e) => {
      const sel = !c.adaptive?.[agent]?.effort && curEffort === e;
      const [name, desc] = e ? (EFFORTS[e] || [e, '']) : ['模型預設', '由模型自己決定思考程度'];
      return h('button', { class: `pop-item${sel ? ' sel' : ''}`, onclick: () => set({ [ekey]: e }) },
        h('div', {}, h('div', { style: e === 'ultracode' || e === 'ultra' ? { color: 'var(--local2-name)', fontWeight: 600 } : null }, name),
          h('div', { class: 'pd' }, desc)),
        sel ? h('span', { class: 'tick' }, icon('check', 16)) : null);
    };
    popover(anchor, [
      h('div', { class: 'pop-title' }, `${displayName(agent)}：${selectionLabel(c, agent)}`),
      h('div', { class: 'pop-sep' }),
      h('div', { class: 'pop-cols' },
        h('div', {}, h('div', { class: 'pop-title' }, '模型'),
          h('button', { class: 'pop-item' + (c.adaptive?.[agent]?.model ? ' sel' : ''), onclick: () => { send({ type: 'routing.set', id: c.id, agent, model: true }); closePop(); } }, '依需求自動選擇'), items),
        h('div', {}, h('div', { class: 'pop-title' }, '思考程度'),
          h('button', { class: 'pop-item' + (c.adaptive?.[agent]?.effort ? ' sel' : ''), onclick: () => { send({ type: 'routing.set', id: c.id, agent, effort: true }); closePop(); } }, '依需求自動調整'),
          efforts.length ? [effortItem(''), efforts.map(effortItem)]
            : h('div', { class: 'pd', style: { padding: '4px 8px 6px', fontSize: '12px', color: 'var(--text-faint)' } }, '這個模型沒有思考程度可以調整。'))),
      agent === 'claude' && efforts.length ? h('div', { class: 'pd', style: { padding: '6px 8px 2px', fontSize: '12px', color: 'var(--text-faint)' } },
        '換思考程度會在下一則訊息時重新啟動 Claude，對話內容會接續。') : null], 'wide');
  }

  function permPicker(anchor, c) {
    popover(anchor, [h('div', { class: 'pop-title' }, '權限模式'),
      Object.entries(PERMS).map(([k, p]) => h('button', { class: `pop-item${c.permission === k ? ' sel' : ''}`, onclick: () => { closePop(); setPermission(c, k); } },
        icon(p.icon, 16), h('div', {}, h('div', { style: p.danger ? { color: 'var(--danger-text)' } : null }, p.label), h('div', { class: 'pd' }, p.desc)),
        c.permission === k ? h('span', { class: 'tick' }, icon('check', 16)) : null))]);
  }

  function modePicker(anchor, c, modes) {
    const pick = (mode) => { send({ type: 'extensions.mode', id: c.id, mode }); closePop(); };
    popover(anchor, [h('div', { class: 'pop-title' }, '插件模式'),
      h('div', { class: 'pop-col' }, h('button', { class: c.mode ? '' : 'sel', onclick: () => pick('') }, '無（保留目前設定）'),
        ...modes.map(m => h('button', { class: c.mode === m.id ? 'sel' : '', 'data-tip': m.description || '', onclick: () => pick(m.id) }, m.name))),
      h('div', { class: 'pd', style: { padding: '2px 8px 6px', fontSize: '12px', color: 'var(--text-muted)', maxWidth: '280px' } },
        '套用模式會照插件的設定修改這個對話，並把模式指示附給 AI。取消模式不會把設定改回去。')]);
  }

  function renderEditionRouting(box) {
    // A special edition keeps the ordinary routing controls.
    appearance.routingPage(box, conv());
  }

  function roundsPicker(anchor, c) {
    popover(anchor, [h('div', { class: 'pop-title' }, '最多來回幾輪後先問你'),
      h('div', { class: 'pop-row' }, [2, 4, 6, 8, 10, 12, 16, 20].map((n) => h('button', { class: c.maxRounds === n ? 'sel' : '', onclick: () => { send({ type: 'conv.settings', id: c.id, settings: { maxRounds: n } }); closePop(); } }, String(n)))),
      h('div', { class: 'pd', style: { padding: '2px 8px 6px', fontSize: '12px', color: 'var(--text-muted)', maxWidth: '260px' } }, '每一次審查（GPT 審查或 Claude 複審）算一輪。到達上限時會停下來，讓你決定要不要繼續。')]);
  }

  // The command is the user's own; CCA runs it through cmd.exe in the project folder, never one a model suggested.
  function testGuardDialog(c) {
    const input = h('input', { class: 'input mono', type: 'text', value: c.testCommand || '', maxlength: 500, placeholder: '例如 build.bat test', 'aria-label': '測試指令' });
    modal({ title: '測試守門的測試指令', body: [
      h('p', { class: 'hint' }, '審查者（GPT 的審查、Claude 的複審）修改檔案後，CCA 會在專案資料夾執行這行指令。結束碼 0 才算通過。'),
      h('p', { class: 'hint' }, '如果轉紅，CCA 會再對審查前的檢查點跑一次：那次是綠的，才還原審查者的修改並把測試輸出交給下一位；那次也是紅的，就保留修改，不算審查者的錯。'),
      h('p', { class: 'hint' }, '指令由你寫、在你的電腦上執行，單行、500 字以內，逾時 10 分鐘。需要專案資料夾與 git（檢查點）。這道守門只擋測試變紅，不證明審查者的修改正確。'),
      input],
    foot: (close) => [h('button', { class: 'btn sm ghost', onclick: close }, '取消'),
      h('button', { class: 'btn sm primary', onclick: () => { send({ type: 'conv.settings', id: c.id, settings: { testCommand: input.value.trim() } }); close(); } }, '儲存')] });
  }

  function setPermission(c, k) {
    if (k === c.permission) return;
    if (k === 'bypass') { bypassDialog(c); return; }
    send({ type: 'conv.settings', id: c.id, settings: { permission: k } });
  }

  async function pickFolder(c) {
    if (running(c.id)) { toast('error', 'AI 正在工作，停止後才能更換資料夾'); return; }
    const p = await request({ type: 'folder.pick', initial: c.cwd });
    if (p) send({ type: 'conv.settings', id: c.id, settings: { cwd: p } });
  }

  // bare: no title bar or padding, for dialogs that draw their own header (the full profile).
  function modal({ title, desc, body, foot, wide, onClose, canClose, bare, cls, label }) {
    const previousFocus = document.activeElement;
    const close = () => {
      if (!back.isConnected || (canClose && !canClose())) return;
      back.remove(); document.removeEventListener('keydown', onKey);
      if (onClose) onClose();
      if (previousFocus?.isConnected) previousFocus.focus?.();
    };
    const onKey = e => {
      if ([...document.querySelectorAll('.backdrop')].at(-1) !== back) return;
      if (e.key === 'Escape') { e.preventDefault(); close(); }
      if (e.key === 'Tab') {
        const focusable = [...back.querySelectorAll('a[href], button, input, textarea, select, [tabindex]')]
          .filter(el => !el.disabled && el.tabIndex >= 0 && el.getClientRects().length);
        const first = focusable[0], last = focusable.at(-1), active = document.activeElement;
        if (!first) { e.preventDefault(); return; }
        if (!back.contains(active) || (e.shiftKey ? active === first : active === last)) {
          e.preventDefault(); (e.shiftKey ? last : first).focus();
        }
      }
    };
    const back = h('div', { class: 'backdrop', onmousedown: (e) => { if (e.target === back) close(); } },
      h('div', { class: `modal${wide ? ' wide' : ''}${cls ? ' ' + cls : ''}`, role: 'dialog', 'aria-modal': 'true', 'aria-label': label || title },
        bare ? null : h('div', { class: 'modal-head' }, h('h2', {}, title), desc ? h('p', {}, desc) : null),
        body ? h('div', { class: bare ? 'modal-bare' : 'modal-body' }, body) : null,
        foot ? h('div', { class: 'modal-foot' }, typeof foot === 'function' ? foot(close) : foot) : null));
    document.addEventListener('keydown', onKey);
    $('#layers').append(back);
    const first = back.querySelector('input:not([disabled]), textarea:not([disabled])') || back.querySelector('button:not([disabled])');
    if (first) setTimeout(() => { if (back.isConnected) first.focus(); }, 0);
    return close;
  }

  function bypassDialog(c) {
    const ok = h('input', { type: 'checkbox' });
    const confirmBtn = h('button', { class: 'btn danger', disabled: true }, '跳過所有確認');
    ok.addEventListener('change', () => { confirmBtn.disabled = !ok.checked; });
    modal({
      title: '要跳過所有權限確認嗎？',
      desc: `開啟後，${c.kind === 'group' ? 'Claude 和 GPT' : convTitle(c)} 在這個對話裡做任何事都不會先問你。`,
      body: [
        h('ul', { class: 'risk-list' },
          h('li', {}, '可以修改、覆寫或刪除專案資料夾內外的任何檔案。'),
          h('li', {}, '可以執行任何指令，包括安裝套件、刪除檔案、推送 git。'),
          h('li', {}, '可以連上網路，下載或上傳資料。'),
          h('li', {}, '如果讀到含有惡意指示的檔案或網頁（提示注入），AI 可能照著做，而且不會有人攔下。')),
        h('p', { class: 'muted', style: { fontSize: '13px', lineHeight: 1.5 } },
          'Claude Code 和 Codex 的官方文件都建議只在沒有重要資料、最好沒有網路的隔離環境（容器或虛擬機）裡使用這個模式。',
          c.isGit ? '' : '這個資料夾沒有 git，改壞了也無法用 git 還原。'),
        h('label', { class: 'check', style: { marginTop: '8px' } }, ok, '我了解風險，並自行負責'),
      ],
      foot: (close) => [h('button', { class: 'btn ghost', onclick: close }, '取消'),
        (confirmBtn.onclick = () => { send({ type: 'conv.settings', id: c.id, settings: { permission: 'bypass', confirmBypass: true } }); close(); }, confirmBtn)],
    });
  }

  async function createGroupDialog() {
    const name = h('input', { class: 'input', placeholder: '協作群組', maxlength: 80 });
    let folder = '';
    const folderLabel = h('span', { class: 'mono muted', style: { fontSize: '13px', wordBreak: 'break-all' } }, '尚未選擇');
    const pick = h('button', { class: 'btn secondary', onclick: async () => {
      const p = await request({ type: 'folder.pick', initial: '' });
      if (p) { folder = p; folderLabel.textContent = p; if (!name.value) name.value = baseName(p); }
    } }, icon('folder', 16), '選擇資料夾');
    modal({
      title: '建立協作群組',
      desc: '可選 Claude 或 GPT 主刀；額度耗盡時可由 GPT 接續，包含目前進度和未完成工作。',
      body: [h('div', { class: 'field' }, h('label', {}, '群組名稱'), name),
        h('div', { class: 'field' }, h('label', {}, '專案資料夾'), h('div', { style: { display: 'flex', gap: '10px', alignItems: 'center', flexWrap: 'wrap' } }, pick, folderLabel),
          h('div', { class: 'hint' }, '兩個 AI 會在同一個資料夾裡讀寫檔案。建議使用有 git 的資料夾，方便檢查和還原修改。'))],
      foot: (close) => [h('button', { class: 'btn ghost', onclick: close }, '取消'),
        h('button', { class: 'btn primary', onclick: () => { send({ type: 'conv.create', name: name.value.trim(), cwd: folder }); close(); } }, '建立')],
    });
  }

  function renameDialog(c) {
    const name = h('input', { class: 'input', value: convTitle(c), maxlength: 80 });
    const picture = h('div', { class: 'group-photo-preview' }, groupAvatar(c, 80));
    const remove = h('button', { class: 'btn ghost sm', onclick: () => send({ type: 'conv.avatar.remove', id: c.id }) }, '移除群組頭像');
    const refresh = () => { const current = conv(c.id) || c; put(picture, groupAvatar(current, 80)); remove.hidden = !current.avatar; };
    S.groupEditor = { id: c.id, refresh };
    modal({
      title: '群組資料', onClose: () => { S.groupEditor = null; },
      desc: '頭像儲存或移除後立即生效；名稱需另按「儲存名稱」。',
      body: [h('div', { class: 'group-photo-edit' }, picture,
        h('div', {}, h('button', { class: 'btn secondary sm', onclick: () => send({ type: 'conv.image', id: c.id }) }, '更換群組頭像'), remove)),
        h('div', { class: 'field' }, h('label', {}, '群組名稱'), name)],
      foot: (close) => [h('button', { class: 'btn ghost', onclick: close }, '關閉'),
        h('button', { class: 'btn primary', onclick: () => { if (name.value.trim()) send({ type: 'conv.settings', id: c.id, settings: { name: name.value.trim() } }); close(); } }, '儲存名稱')],
    });
    refresh();
  }

  function clearDialog(c) {
    modal({
      title: c.kind === 'group' ? '清除或刪除群組' : `清除和 ${convTitle(c)} 的對話`,
      desc: c.kind === 'group'
        ? '清除會刪掉這個群組的訊息，兩個 AI 也會開始全新的工作階段（不記得之前的內容）。刪除會連同群組一起移除。專案資料夾裡的檔案都不會被動到。'
        : `清除會刪掉訊息，${convTitle(c)} 會開始全新的工作階段。專案資料夾裡的檔案不會被動到。`,
      foot: (close) => [h('button', { class: 'btn ghost', onclick: close }, '取消'),
        c.kind === 'group' ? h('button', { class: 'btn secondary', onclick: () => { send({ type: 'conv.delete', id: c.id }); close(); } }, '刪除群組') : null,
        h('button', { class: 'btn danger', onclick: () => { send({ type: 'conv.reset', id: c.id }); close(); } }, '清除對話')],
    });
  }

  function settingsDialog() {
    const cp = h('input', { class: 'input mono', value: S.settings.claudePath || '', placeholder: (S.auth.claude && S.auth.claude.exe) || '自動偵測' });
    const xp = h('input', { class: 'input mono', value: S.settings.codexPath || '', placeholder: (S.auth.codex && S.auth.codex.exe) || '自動偵測' });
    const gp = h('input', { class: 'input mono', value: S.settings.geminiPath || '', placeholder: S.auth.gemini?.exe || '自動偵測' });
    modal({
      title: '設定',
      wide: true,
      body: [
        h('div', { class: 'field' }, h('label', {}, 'Claude Code 執行檔'), cp, h('div', { class: 'hint' }, '留空會自動尋找：PATH、%USERPROFILE%\\.local\\bin、npm 全域安裝。')),
        h('div', { class: 'field' }, h('label', {}, 'Codex 執行檔'), xp, h('div', { class: 'hint' }, '留空會自動尋找 PATH 和 npm 全域安裝。')),
        h('div', { class: 'field' }, h('label', {}, 'Gemini CLI 執行檔'), gp, h('div', { class: 'hint' }, '使用官方 npm 安裝的 gemini.cmd；帳號方案與額度查詢需要 Node.js。')),
        h('div', { class: 'field' }, h('label', {}, '關於'),
          h('div', { class: 'kv' }, h('span', {}, 'ChatClaudeAgent 版本'), h('span', {}, S.version || '-')),
          h('div', { class: 'kv' }, h('span', {}, 'Claude Code'), h('span', {}, (S.auth.claude && S.auth.claude.version) || '-')),
          h('div', { class: 'kv' }, h('span', {}, 'Codex'), h('span', {}, (S.auth.codex && S.auth.codex.version) || '-')),
          h('div', { class: 'hint' }, '對話紀錄存在 %APPDATA%\\CCA。任務會傳送至啟用的 Anthropic、OpenAI、Google 或自訂供應商。')),
      ],
      foot: (close) => [h('button', { class: 'btn ghost', onclick: close }, '取消'),
        h('button', { class: 'btn primary', onclick: () => { send({ type: 'settings.set', settings: { claudePath: cp.value.trim(), codexPath: xp.value.trim(), geminiPath: gp.value.trim() } }); close(); } }, '儲存')],
    });
  }

  function showHelp() {
    modal({
      title: 'ChatClaudeAgent 怎麼用',
      wide: true,
      body: [
        h('div', { class: 'field' }, h('label', {}, '私人訊息'), h('div', { class: 'muted', style: { lineHeight: 1.5 } }, '直接和 Claude 或 GPT 一對一對話，各自在你選的專案資料夾裡工作。')),
        h('div', { class: 'field' }, h('label', {}, '協作群組'), h('div', { class: 'muted', style: { lineHeight: 1.5 } }, '你下指令 → Claude 撰寫 → GPT 審查並直接修改 → Claude 複審 GPT 的修改 → 來回直到其中一方審查後完全同意、沒有再改任何檔案。到達輪數上限會先停下來問你。')),
        h('div', { class: 'field' }, h('label', {}, '權限模式'), Object.values(PERMS).map((p) => h('div', { class: 'kv' }, h('span', {}, p.label), h('span', { style: { whiteSpace: 'normal' } }, p.desc)))),
        h('div', { class: 'field' }, h('label', {}, '你會看到的資訊'), h('div', { class: 'muted', style: { lineHeight: 1.6 } },
          '每個檔案的新增/刪除行數和 diff、執行的指令和結果、權限請求、待辦清單、token 用量和上下文使用率、5 小時與每週額度、API 重試和錯誤。')),
      ],
      foot: (close) => [h('button', { class: 'btn primary', onclick: close }, '知道了')],
    });
  }

  function quickSwitcher() {
    const input = h('input', { class: 'input', placeholder: '要去哪裡？' });
    const list = h('div', { style: { marginTop: '10px', maxHeight: '320px', overflowY: 'auto' } });
    const paint = () => {
      const q = input.value.trim().toLowerCase();
      clear(list);
      const items = S.convs.filter((c) => !q || convTitle(c).toLowerCase().includes(q) || (c.cwd || '').toLowerCase().includes(q));
      for (const c of items) {
        list.append(h('button', { class: 'dm', onclick: () => { close(); openConv(c.id); } },
          c.kind === 'group' ? groupAvatar(c) : avatar(c.kind, 32),
          h('div', { class: 'dm-text' }, h('div', { class: 'dm-name' }, convTitle(c)), h('div', { class: 'dm-sub' }, c.cwd || ''))));
      }
    };
    input.addEventListener('input', paint);
    input.addEventListener('keydown', (e) => { if (e.key === 'Enter') { const first = list.querySelector('button'); if (first) first.click(); } });
    const close = modal({ title: '尋找對話', body: [input, list] });
    paint();
  }
  document.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') { e.preventDefault(); quickSwitcher(); }
    if (e.key === 'Escape') {
      // Escape closes the innermost thing: a popover first, then the review pane.
      if (!popEl && S.review && !document.querySelector('.backdrop')) closeReview();
      closePop(true); hideTip();
    }
  });

  // ================================================================ user settings
  // A full-screen page like Discord's User Settings: profiles, personas, daily mode.
  const settingsUi = { open: false, section: 'me', el: null, renameOnPick: true };
  const SWATCHES = ['#5865f2', '#3ba55c', '#faa61a', '#ed4245', '#eb459e', '#d97757', '#10a37f', '#4f7bd8', '#9b84ec', '#1e1f22'];

  function openSettings(section) {
    if (!settingsUi.open || section === 'local') settingsUi.localAsked = false;
    if (!settingsUi.open) settingsUi.humanizeDraft = null;
    settingsUi.open = true;
    if (section) settingsUi.section = section;
    renderSettings();
  }
  function closeSettings() {
    appearance.cancelPreview();
    settingsUi.open = false;
    if (settingsUi.el) { settingsUi.el.remove(); settingsUi.el = null; }
  }
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && settingsUi.open && !document.querySelector('.backdrop')) closeSettings();
  });

  function renderSettings() {
    if (!settingsUi.open) return;
    // Keep focus and caret in a text field that is being typed into.
    const active = document.activeElement;
    const keepId = active && settingsUi.el && settingsUi.el.contains(active) ? active.dataset.keep : null;
    const caret = keepId && 'selectionStart' in active ? active.selectionStart : null;
    const scrollTop = settingsUi.el && settingsUi.el.querySelector('.settings-main') ? settingsUi.el.querySelector('.settings-main').scrollTop : 0;
    if (!settingsUi.el) {
      settingsUi.el = h('div', { class: 'settings-layer' });
      document.body.append(settingsUi.el);
    }
    const item = (id, label, lead) => h('button', { class: 'ss-item' + (settingsUi.section === id ? ' sel' : ''),
      onclick: () => { settingsUi.section = id; renderSettings(); if (id === 'updates') send({ type: 'update.get' }); } }, lead || null, label);
    const nav = h('nav', {},
      h('div', { class: 'ss-group' }, '使用者設定'),
      item('me', '我的個人資料', avatar('user', 20)),
      h('div', { class: 'ss-sep' }),
      h('div', { class: 'ss-group' }, 'AI 設定'),
      item('claude', displayName('claude'), avatar('claude', 20)),
      item('codex', displayName('codex'), avatar('codex', 20)),
      item('gemini', displayName('gemini'), avatar('gemini', 20)),
      special() ? null : item('local', '本地模型'),
      special() ? null : item('local1', displayName('local1'), avatar('local1', 20)),
      special() ? null : item('local2', displayName('local2'), avatar('local2', 20)),
      item('routing', '自動模型與思考程度'),
      item('platform', '多代理與 API 工具'),
      special() ? null : item('personas', '性格庫'),
      special() ? null : item('daily', '日常模式'),
      item('humanize', '去 AI 化'),
      item('resume', '額度與自動接續'),
      h('div', { class: 'ss-sep' }),
      h('div', { class: 'ss-group' }, 'App 設定'),
      item('themes', '介面主題'),
      item('extensions', 'CCA 插件'),
      item('layout', '版面與閱讀'),
      item('updates', '軟體更新'),
      item('git', '版本控制'),
      item('advanced', '執行檔與資料'),
      item('about', '關於 ChatClaudeAgent'));
    const content = h('div', { class: 'settings-content' });
    const sec = settingsUi.section;
    if (sec === 'me' || AGENTS[sec]) renderProfileSection(content, sec === 'me' ? 'user' : sec);
    else if (sec === 'local') renderLocalSection(content);
    else if (sec === 'platform' && window.CCAPlatform) window.CCAPlatform.providersPage(content);
    else if (sec === 'personas') renderPersonaLibrary(content);
    else if (sec === 'daily') renderDailySection(content);
    else if (sec === 'humanize') renderHumanizeSection(content);
    else if (sec === 'themes') appearance.themePage(content);
    else if (sec === 'extensions') extensions.page(content);
    else if (sec === 'routing' && special()) renderEditionRouting(content);
    else if (sec === 'routing') appearance.routingPage(content, conv());
    else if (sec === 'layout') desk.settings(content);
    else if (sec === 'updates') desk.updateSettings(content);
    else if (sec === 'git') git.rulesPage(content);
    else if (sec === 'resume') renderResumeSection(content);
    else if (sec === 'advanced') renderAdvancedSection(content);
    else renderAboutSection(content);
    const main = h('div', { class: 'settings-main' }, content,
      h('div', { class: 'settings-close' }, h('button', { 'aria-label': '關閉設定', onclick: closeSettings }, icon('x', 18)), 'ESC'));
    put(settingsUi.el, h('div', { class: 'settings-side scroll-thin' }, nav), main);
    main.scrollTop = scrollTop;
    if (keepId) {
      const again = settingsUi.el.querySelector('[data-keep="' + keepId + '"]');
      if (again) { again.focus(); if (caret != null && again.setSelectionRange) again.setSelectionRange(caret, caret); }
    }
  }

  function previewCard(who) {
    const persona = who === 'user' ? null : personaOf(who);
    const sub = who === 'user' ? '你'
      : AGENTS[who].vendor + ' · ' + AGENTS[who].cli + (persona ? ' · 性格：' + persona.name : '');
    let about = '';
    if (who === 'user') {
      about = userAbout();
    } else {
      about = profileFacts(who).about;
    }
    return h('div', { class: 'preview-card' },
      h('div', { class: 'pc-banner', style: bannerStyle(who) }),
      h('div', { class: 'pc-avatar' }, avatar(who, 80, who === 'user' ? 'ok' : agentStatus(who))),
      h('div', { class: 'pc-body' },
        h('div', { class: 'pc-name' }, displayName(who)),
        h('div', { class: 'pc-sub' }, sub),
        window.CCABadges?.row(who),
        about ? [h('div', { class: 'pc-sep' }), h('div', { class: 'pc-label' }, who === 'user' ? 'AI 怎麼稱呼你' : '自我介紹'),
          h('div', { class: 'pc-text' }, about)] : null));
  }

  function renderProfileSection(box, who) {
    const p = prof(who);
    const isAi = who !== 'user';
    const def = who === 'user' ? S.user.name : AGENTS[who].name;
    const nameInput = h('input', { class: 'input', value: p.name || '', placeholder: def, maxlength: 32, 'data-keep': 'name-' + who });
    let timer = null;
    nameInput.addEventListener('input', () => {
      clearTimeout(timer);
      timer = setTimeout(() => send({ type: 'profile.set', who, name: nameInput.value }), 350);
    });
    const curColor = p.bannerColor || defaultBanner(who);
    const joined = h('input', { type: 'date', class: 'input', value: p.joinedAt || '', 'aria-label': '自訂加入日期', 'data-keep': 'joined-' + who });
    joined.addEventListener('change', () => send({ type: 'profile.set', who, joinedAt: joined.value || null }));
    const defaultDate = isAi ? profileFacts(who).info?.date : '';
    const colorInput = h('input', { type: 'color', class: 'color-input', value: curColor, 'data-tip': '自訂顏色' });
    colorInput.addEventListener('change', () => send({ type: 'profile.set', who, bannerColor: colorInput.value }));
    box.append(
      h('h2', {}, who === 'user' ? '我的個人資料' : displayName(who) + ' 的個人資料'),
      isAi ? h('p', { class: 'lead' }, isLocalAgent(who) ? '這是第 ' + who.slice(-1) + ' 位本地唯讀審查員的名稱、頭像、橫幅和性格。模型在「本地模型」裡選。' : '這裡改的是 ' + AGENTS[who].name + ' 在 CCA 裡顯示的名稱、頭像和橫幅，以及它說話的性格。它背後仍然是 ' + AGENTS[who].vendor + ' 的 ' + AGENTS[who].cli + '。')
        : h('p', { class: 'lead' }, '你在對話裡顯示的名稱、頭像和橫幅。AI 在群組裡也會用這個名稱稱呼你。'),
      h('div', { class: 'profile-edit' },
        h('div', { class: 'pe-form' },
          h('div', { class: 'field' }, h('label', {}, '顯示名稱'), nameInput,
            h('div', { class: 'hint' }, '留空就使用「' + def + '」。')),
          isAi ? h('div', { class: 'field' }, h('label', {}, '加入日期'), joined,
            h('div', { class: 'hint' }, '留空依目前模型的發布日' + (defaultDate ? '（' + defaultDate + '）' : '，未知日期不會自動填入') + '。')) : null,
          h('div', { class: 'field' }, h('label', {}, '頭像'),
            h('div', { class: 'pe-row' },
              h('button', { class: 'btn primary sm', onclick: () => send({ type: 'profile.image', who, kind: 'avatar' }) }, '更換頭像'),
              p.avatar ? h('button', { class: 'btn ghost sm', onclick: () => send({ type: 'profile.set', who, avatar: null }) }, '移除頭像') : null),
            h('div', { class: 'hint' }, 'PNG、JPG、GIF 或 WebP，10 MB 以內；裁切後儲存為 512 × 512 靜態 PNG。')),
          h('div', { class: 'field' }, h('label', {}, '橫幅'),
            h('div', { class: 'pe-row' },
              h('button', { class: 'btn secondary sm', onclick: () => send({ type: 'profile.image', who, kind: 'banner' }) }, '更換橫幅圖片'),
              p.banner ? h('button', { class: 'btn ghost sm', onclick: () => send({ type: 'profile.set', who, banner: null }) }, '移除圖片') : null),
            h('div', { class: 'swatches' },
              SWATCHES.map((col) => h('button', { class: 'swatch' + (!p.banner && curColor.toLowerCase() === col ? ' sel' : ''), style: { background: col },
                'aria-label': col, onclick: () => send({ type: 'profile.set', who, bannerColor: col }) })),
              colorInput),
            h('div', { class: 'hint' }, p.banner ? '目前使用圖片；移除圖片後會顯示這個顏色。沒有頭像時，字母頭像也用這個顏色。' : '沒有頭像時，字母頭像也用這個顏色。'))),
        previewCard(who)));
    if (isAi) renderPersonaPicker(box, who);
    else box.append(h('div', { class: 'field' }, h('h3', {}, '個人徽章'),
      h('p', { class: 'lead' }, '所有安裝內建成員徽章；其他徽章依本機授予資料顯示。'),
      h('button', { class: 'btn secondary', type: 'button', 'data-manage-badges': true, onclick: () => window.CCABadges?.open() }, '管理徽章')));
  }

  function renderPersonaPicker(box, agent) {
    const cur = ((S.settings.persona || {})[agent]) || 'none';
    const rename = h('input', { type: 'checkbox', checked: settingsUi.renameOnPick });
    rename.addEventListener('change', () => { settingsUi.renameOnPick = rename.checked; });
    const pick = (id) => send({ type: 'persona.assign', agent, id, rename: id !== 'none' && rename.checked });
    const card = (p) => {
      const id = p ? p.id : 'none';
      const sel = cur === id;
      return h('div', { class: 'persona-card' + (sel ? ' sel' : ''), role: 'button', tabIndex: 0, onclick: () => { if (!sel) pick(id); } },
        h('span', { class: 'radio' }),
        h('div', { class: 'pp-main' },
          h('div', { class: 'pp-name' }, p ? p.name : '無性格',
            p ? h('span', { class: 'tag' }, p.builtin ? '本機預設' : '自訂') : null,
            p && p.agent !== 'any' && p.agent !== agent && !(p.agent === 'local' && isLocalAgent(agent)) ? h('span', { class: 'tag' }, '原本是給 ' + (p.agent === 'local' ? '本地審查員' : AGENTS[p.agent].name) + ' 的') : null),
          h('div', { class: 'pp-sum' }, p ? (p.summary || '（沒有簡介）') : AGENTS[agent].name + ' 原本的說話方式。'),
          p && sel ? h('details', { class: 'prompt-view', onclick: (e) => e.stopPropagation() }, h('summary', {}, '查看完整的性格指示'), h('pre', {}, p.prompt)) : null),
        p ? h('div', { class: 'pp-actions' },
          h('button', { class: 'btn sm outline', onclick: (e) => { e.stopPropagation(); personaEditor(p.builtin ? { ...p, id: '', builtin: false, name: p.name + '（自訂）' } : p, agent); } },
            p.builtin ? '複製並修改' : '編輯')) : null);
    };
    const fits = (p) => p.agent === agent || p.agent === 'any' || (isLocalAgent(agent) && p.agent === 'local');
    const mine = S.personas.filter(fits);
    const others = S.personas.filter((p) => !fits(p));
    box.append(
      h('h3', {}, '性格'),
      h('p', { class: 'lead', style: { marginTop: 0 } }, '性格只改變說話的語氣和態度，不影響程式能力；權限規則和協作流程的格式永遠優先。換性格後，下一則訊息開始生效。'),
      h('div', { class: 'persona-list' }, card(null), mine.map(card), others.map(card)),
      h('div', { class: 'pe-row', style: { marginTop: '12px' } },
        h('button', { class: 'btn primary sm', onclick: () => personaEditor({ agent }, agent) }, icon('plus', 14), '建立自訂性格'),
        h('label', { class: 'check', style: { marginLeft: '8px', fontSize: '13px' } }, rename, '選擇角色時，同時把名稱改成角色名')));
  }

  function personaEditor(p, assignTo) {
    const name = h('input', { class: 'input', value: p.name || '', maxlength: 32, placeholder: '例如：嚴格的資深工程師' });
    const agent = h('select', { class: 'select' },
      h('option', { value: 'claude' }, '給 ' + displayName('claude') + '（Claude）'),
      h('option', { value: 'codex' }, '給 ' + displayName('codex') + '（GPT）'),
      h('option', { value: 'gemini' }, '給 ' + displayName('gemini') + '（Gemini）'),
      h('option', { value: 'local' }, '給本地審查員'),
      h('option', { value: 'any' }, '都可以'));
    agent.value = p.agent || assignTo || 'any';
    const summary = h('input', { class: 'input', value: p.summary || '', maxlength: 120, placeholder: '一句話介紹，會顯示在個人資料卡上' });
    const address = h('input', { class: 'input', value: p.address || '', maxlength: 20, placeholder: '例如：指揮官、老闆、學長（選填）' });
    const prompt = h('textarea', { class: 'input', rows: 12, style: { fontFamily: 'var(--font)', fontSize: '14px', minHeight: '220px' },
      placeholder: '用自己的話描述這個角色：身分、性格、說話方式、口頭禪……\n\n例如：\n你是一位冷靜的資深工程師，說話簡短直接，遇到風險會先提醒，喜歡用比喻解釋概念。' });
    prompt.value = p.prompt || '';
    const useNow = h('input', { type: 'checkbox', checked: !!assignTo });
    modal({
      title: p.id ? '編輯性格' : '建立性格',
      desc: '寫下角色的身分、性格和說話方式。CCA 會自動加上規則：角色只影響語氣，不能降低程式品質，也不能蓋過權限和協作流程。',
      wide: true,
      body: [
        h('div', { class: 'field' }, h('label', {}, '名稱'), name),
        h('div', { class: 'field' }, h('label', {}, '適合'), agent),
        h('div', { class: 'field' }, h('label', {}, '簡介'), summary),
        h('div', { class: 'field' }, h('label', {}, '稱呼你為'), address),
        h('div', { class: 'field' }, h('label', {}, '性格與說話方式'), prompt),
        assignTo ? h('label', { class: 'check' }, useNow, '儲存後讓 ' + displayName(assignTo) + ' 使用這個性格') : null,
      ],
      foot: (close) => [
        p.id && !p.builtin ? h('button', { class: 'btn ghost', style: { marginRight: 'auto', color: 'var(--danger-text)' }, onclick: () => {
          send({ type: 'persona.delete', id: p.id });
          close();
        } }, '刪除這個性格') : null,
        h('button', { class: 'btn ghost', onclick: close }, '取消'),
        h('button', { class: 'btn primary', onclick: () => {
          if (!name.value.trim() || !prompt.value.trim()) { toast('error', '名稱和性格與說話方式都要填'); return; }
          send({ type: 'persona.save', persona: { id: p.builtin ? '' : (p.id || ''), name: name.value.trim(), agent: agent.value, summary: summary.value.trim(),
            address: address.value.trim(), prompt: prompt.value }, assignTo: assignTo && useNow.checked ? assignTo : '' });
          close();
        } }, '儲存')],
    });
  }

  function renderPersonaLibrary(box) {
    const used = (id) => PROFILE_AGENTS.filter((a) => ((S.settings.persona || {})[a]) === id).map((a) => displayName(a));
    box.append(h('h2', {}, '性格庫'),
      h('p', { class: 'lead' }, '性格由本機資料管理，程式不綁定角色或預設指派。可建立自訂性格，或在資料目錄加入 persona-presets.json，重新啟動後載入。表達風格不會改變技術規範、工具權限與協作格式。'),
      !S.personas.length ? h('p', { class: 'muted' }, '尚未建立性格。代理將沿用原本的表達方式。') : null,
      h('div', { class: 'persona-list' }, S.personas.map((p) => h('div', { class: 'persona-card' },
        h('div', { class: 'pp-main' },
          h('div', { class: 'pp-name' }, p.name, h('span', { class: 'tag' }, p.builtin ? '本機預設' : '自訂'),
            p.agent === 'local' ? h('span', { class: 'tag' }, '給本地審查員') : p.agent !== 'any' && AGENTS[p.agent] ? h('span', { class: 'tag' }, '給 ' + AGENTS[p.agent].name) : h('span', { class: 'tag' }, '都可以'),
            used(p.id).length ? h('span', { class: 'inst-tag' }, used(p.id).join('、') + ' 使用中') : null),
          h('div', { class: 'pp-sum' }, p.summary || '（沒有簡介）'),
          h('details', { class: 'prompt-view' }, h('summary', {}, '查看完整的性格指示'), h('pre', {}, p.prompt))),
        h('div', { class: 'pp-actions' },
          h('button', { class: 'btn sm outline', onclick: () => personaEditor(p.builtin ? { ...p, id: '', builtin: false, name: p.name + '（自訂）' } : p, null) }, p.builtin ? '複製並修改' : '編輯'))))),
      h('div', { class: 'pe-row', style: { marginTop: '12px' } },
        h('button', { class: 'btn primary sm', onclick: () => personaEditor({ agent: 'any' }, null) }, icon('plus', 14), '建立性格')));
  }

  function autoDailyModel(agent) {
    const saved = S.settings.daily || {};
    S.settings.daily = { ...saved, [agent + 'Model']: '' };
    const m = dailyPick(agent).model;
    S.settings.daily = saved;
    return m;
  }

  function renderDailySection(box) {
    const d = S.settings.daily || {};
    const block = (agent) => {
      const list = S.models[agent] && S.models[agent].length ? S.models[agent] : (agent === 'claude' ? FALLBACK_CLAUDE_MODELS : []);
      const now = dailyPick(agent);
      const modelSel = h('select', { class: 'select' },
        h('option', { value: '' }, '自動：最省的模型（' + modelLabel(agent, autoDailyModel(agent)) + '）'),
        list.filter((m) => m.value && m.value !== 'default').map((m) => h('option', { value: m.value }, m.displayName || m.value)));
      modelSel.value = d[agent + 'Model'] || '';
      modelSel.addEventListener('change', () => send({ type: 'daily.set', [agent + 'Model']: modelSel.value, [agent + 'Effort']: '' }));
      const efforts = effortsFor(agent, modelInfo(agent, now.model)).filter((e) => e !== 'ultracode' && e !== 'ultra');
      const effSel = h('select', { class: 'select' }, h('option', { value: '' }, '自動：最低'),
        efforts.map((e) => h('option', { value: e }, effortLabel(e) + '（' + EFFORTS[e][1] + '）')));
      effSel.value = d[agent + 'Effort'] || '';
      effSel.addEventListener('change', () => send({ type: 'daily.set', [agent + 'Effort']: effSel.value }));
      return h('div', { class: 'card', style: { marginTop: 0 } },
        h('div', { class: 'card-title' }, avatar(agent, 20), displayName(agent)),
        h('div', { class: 'field' }, h('label', {}, '模型'), modelSel),
        h('div', { class: 'field', style: { marginBottom: 0 } }, h('label', {}, '思考程度'),
          efforts.length ? effSel : h('div', { class: 'muted', style: { fontSize: '13px' } }, '這個模型沒有思考程度可以調整')),
        h('div', { class: 'hint', style: { marginTop: '10px', fontSize: '12px', color: 'var(--text-muted)' } }, '目前會用：' + modelWithEffort(agent, now.model, now.effort)));
    };
    box.append(h('h2', {}, '日常模式'),
      h('p', { class: 'lead' }, '在私訊的標題列打開「日常模式」，就會改用最省額度的模型和最低的思考程度跟你聊天，不需要選專案資料夾；AI 要改檔案或執行指令前都會先問你。性格設定一樣有效。'),
      h('div', { class: 'daily-grid' }, block('claude'), block('codex')));
  }

  // Quota-aware resume: when every member is out of quota the task waits for the earliest reported reset.
  function renderResumeSection(body) {
    const r = S.settings.autoResume || {}, fb = r.fallback || {}, win = r.window || {};
    const st = { enabled: !!r.enabled, delayMinutes: r.delayMinutes ?? 3, maxAttempts: r.maxAttempts ?? 3,
      fallback: { claude: fb.claude ?? 300, codex: fb.codex ?? 300, gemini: fb.gemini ?? 60 },
      window: { enabled: !!win.enabled, from: win.from || '00:00', to: win.to || '08:00' } };
    const save = () => send({ type: 'autoResume.set', settings: st });
    const number = (key, label, value, min, max, set, hint) => h('div', { class: 'field' }, h('label', { for: 'resume-' + key }, label),
      h('input', { class: 'input resume-number', id: 'resume-' + key, type: 'number', min, max, value, 'data-keep': 'resume-' + key,
        onchange: e => { set(Math.max(min, Math.min(max, Math.round(+e.target.value) || min))); save(); } }),
      hint ? h('div', { class: 'hint' }, hint) : null);
    body.append(h('h2', {}, '額度與自動接續'),
      h('p', { class: 'hint' }, '群組或多代理流程的所有 AI 額度都用完時，CCA 會強制停止並記錄下一步。開啟自動接續後，CCA 依各 AI 回報的重置時間（Claude 的額度事件、GPT 的用量視窗）排定接續；沒有回報時用下面的預設等待。只在 CCA 開著、任務沒有被手動接續時執行。'),
      h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: st.enabled, onchange: e => { st.enabled = e.target.checked; save(); } }), '額度恢復後自動接續任務'),
      h('div', { class: 'resume-grid' + (st.enabled ? '' : ' off') },
        number('delay', '重置後再等幾分鐘', st.delayMinutes, 0, 1440, v => { st.delayMinutes = v; }, '避免剛重置時立刻用掉第一批額度'),
        number('attempts', '最多自動接續幾次', st.maxAttempts, 1, 20, v => { st.maxAttempts = v; }, '每次接續若仍沒有額度會再排下一次'),
        number('claude', displayName('claude') + ' 沒回報重置時間時等待（分鐘）', st.fallback.claude, 1, 10080, v => { st.fallback.claude = v; }),
        number('codex', displayName('codex') + ' 沒回報重置時間時等待（分鐘）', st.fallback.codex, 1, 10080, v => { st.fallback.codex = v; }),
        number('gemini', displayName('gemini') + ' 沒回報重置時間時等待（分鐘）', st.fallback.gemini, 1, 10080, v => { st.fallback.gemini = v; })),
      h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: st.window.enabled, disabled: !st.enabled,
        onchange: e => { st.window.enabled = e.target.checked; save(); } }), '只在指定時段接續'),
      h('div', { class: 'resume-window' + (st.enabled && st.window.enabled ? '' : ' off') },
        h('input', { class: 'input', type: 'time', value: st.window.from, 'aria-label': '開始時間', onchange: e => { st.window.from = e.target.value; save(); } }),
        h('span', {}, '到'),
        h('input', { class: 'input', type: 'time', value: st.window.to, 'aria-label': '結束時間', onchange: e => { st.window.to = e.target.value; save(); } }),
        h('span', { class: 'hint' }, '例如晚上 23:00 到早上 07:00，跨午夜也可以')),
      h('p', { class: 'hint' }, '這些等待時間也決定「跳過」的成員多久後再試一次。帳號已停用的 Gemini 不算可恢復的成員。'));
  }

  function renderHumanizeSection(box) {
    const hz = S.settings.humanize || {};
    const on = hz.enabled !== false;
    const sw = h('button', { class: 'chip keep toggle' + (on ? ' on' : ''), onclick: () => send({ type: 'humanize.set', enabled: !on }) },
      h('span', { class: 'switch' }, h('i')), h('span', {}, on ? '開啟' : '關閉'));
    const extra = h('textarea', { class: 'input', rows: 5, 'data-keep': 'humanize-extra', maxlength: 2000,
      style: { fontFamily: 'var(--font)', fontSize: '14px' },
      placeholder: '例如：\n主色用專案 logo 的墨綠色\n文案用口語，不要出現「解決方案」' });
    // An unsaved draft survives the redraw that a settings update causes.
    extra.value = settingsUi.humanizeDraft != null ? settingsUi.humanizeDraft : (hz.extra || '');
    extra.addEventListener('input', () => { settingsUi.humanizeDraft = extra.value; });
    const save = h('button', { class: 'btn primary sm', onclick: () => { settingsUi.humanizeDraft = null; send({ type: 'humanize.set', extra: extra.value }); } }, '儲存');
    box.append(h('h2', {}, '去 AI 化'),
      h('p', { class: 'lead' }, '開啟後，Claude 和 GPT 做網頁、介面、美術素材和寫文字時，都要避開一眼就看得出是 AI 做的預設風格，例如沒來由的紫色漸層、公版的三張卡片、套話和過度排比。群組審查時，GPT、Claude 和本地審查員也會一起檢查這些地方。'),
      h('div', { class: 'card' },
        h('div', { class: 'card-title' }, '強制去 AI 化', h('span', { style: { marginLeft: 'auto' } }, sw)),
        h('div', { class: 'muted', style: { fontSize: '13px' } }, '對所有私訊和群組都有效，包含日常模式。改了之後，下一則訊息就會套用。')),
      h('div', { class: 'card' },
        h('div', { class: 'card-title' }, '你自己的規則'),
        h('div', { class: 'muted', style: { fontSize: '13px', marginBottom: '8px' } }, '會接在內建規則後面，一起交給 AI。可以寫品牌色、字體、文案語氣，或你特別討厭的寫法。'),
        extra,
        h('div', { class: 'pe-row', style: { marginTop: '8px' } }, save)),
      S.humanizeRules ? h('details', { class: 'prompt-view' }, h('summary', {}, '查看內建規則'), h('pre', {}, S.humanizeRules)) : null);
  }

  function renderAdvancedSection(box) {
    box.append(h('h2', {}, '執行檔與資料'),
      h('div', { class: 'card' }, h('div', { class: 'card-title' }, 'Claude Code'),
        h('div', { class: 'kv' }, h('span', {}, '執行檔'), h('span', { class: 'mono' }, (S.auth.claude && S.auth.claude.exe) || '找不到')),
        h('div', { class: 'kv' }, h('span', {}, '版本'), h('span', {}, (S.auth.claude && S.auth.claude.version) || '-'))),
      h('div', { class: 'card' }, h('div', { class: 'card-title' }, 'Codex'),
        h('div', { class: 'kv' }, h('span', {}, '執行檔'), h('span', { class: 'mono' }, (S.auth.codex && S.auth.codex.exe) || '找不到')),
        h('div', { class: 'kv' }, h('span', {}, '版本'), h('span', {}, (S.auth.codex && S.auth.codex.version) || '-'))),
      h('div', { class: 'pe-row', style: { marginTop: '12px' } }, h('button', { class: 'btn secondary sm', onclick: settingsDialog }, '指定執行檔位置')),
      h('h3', {}, '資料'),
      h('div', { class: 'muted', style: { fontSize: '14px', lineHeight: 1.6 } },
        '對話紀錄、設定和自訂頭像存在 %APPDATA%\\CCA。CCA 不會把你的程式碼或對話傳到 Anthropic 和 OpenAI 以外的地方。'));
  }

  function renderAboutSection(box) {
    box.classList.add('about-section');
    box.append(h('h2', {}, '關於 ChatClaudeAgent'),
      h('dl', { class: 'about-versions' },
        h('dt', {}, 'ChatClaudeAgent 版本'), h('dd', {}, S.version || '-'),
        h('dt', {}, 'Claude Code'), h('dd', {}, (S.auth.claude && S.auth.claude.version) || '-'),
        h('dt', {}, 'Codex'), h('dd', {}, (S.auth.codex && S.auth.codex.version) || '-')),
      h('p', { class: 'about-intro' },
        'CCA（ChatClaudeAgent）是以工作交接、交叉審查與主機驗證為核心的 Windows 開發工作區，提供桌面介面與原生 CLI。'),
      h('h3', {}, '為什麼開發 CCA'),
      h('p', { class: 'about-copy' },
        'CCA 起於作者使用 OpenAI 為 Claude Code 開發的 Codex 插件時，對雙模型協作流程仍有未被滿足的需求：修改、審查、權限與驗收散在不同工具中，切換代理後需要重新整理進度，也難以核對完成聲明是否符合實際結果。'),
      h('h3', {}, '開發目的'),
      h('p', { class: 'about-copy' },
        '讓長時間、多階段的開發工作可以持續交接、獨立審查、追查來源並由使用者掌握。需求與計畫保留版本，交接附上實際檔案變更、指令狀態與待辦；測試結果由主機取得，並與對應來源連結，避免只以模型的口頭結論判定完成。'),
      h('h3', {}, '設計原則與界線'),
      h('p', { class: 'about-copy' },
        '模型與表達風格可以替換，技術規範、工具權限和驗收依據保持一致。沿用使用者已有的 CLI 登入與本機專案，API 服務可另行設定。角色資料不編入程式；不同模型同意或測試通過，仍不能保證軟體不存在錯誤。'));
  }

  // ================================================================ local models (Ollama)
  // Two optional local reviewers. They never edit anything: CCA hands them the diff
  // and files, and their suggestions go to Claude/GPT, who must verify them first.
  const FIT = { gpu: '整個放進顯示卡，速度最快', tight: '會用到部分系統記憶體，比較慢', cpu: '只用 CPU，可以跑但比較慢' };
  const gbText = (n) => (Math.round((Number(n) || 0) * 10) / 10) + ' GB';

  function renderLocalSection(box) {
    if (!settingsUi.localAsked) { settingsUi.localAsked = true; send({ type: 'local.status' }); }
    const L = S.local;
    box.append(h('h2', {}, '本地模型'),
      h('p', { class: 'lead' }, '最多兩個本地模型，用 Ollama 在你的電腦上執行，不花訂閱額度。它們只做唯讀審查：CCA 把變更和檔案內容交給它們看，它們提出的建議會轉給 ' +
        displayName('claude') + ' 和 ' + displayName('codex') + '，兩位要先查證才能採納。在群組右側的「本地審查員」把它們邀請進來。'));
    if (!L) { box.append(h('div', { class: 'empty' }, h('div', { class: 'spinner', style: { margin: '0 auto 10px' } }), '正在檢查 Ollama 和這台電腦的硬體…')); return; }

    const status = h('div', { class: 'card' }, h('div', { class: 'card-title' }, 'Ollama',
      h('button', { class: 'act', onclick: () => { S.local = null; settingsUi.localAsked = false; renderSettings(); } }, '重新整理')));
    if (!L.installedOllama) {
      status.append(h('div', { class: 'notice-line warn' }, icon('alert', 15), '這台電腦還沒有安裝 Ollama。安裝後回來按「重新整理」。'),
        h('div', { class: 'pe-row', style: { marginTop: '10px' } }, h('button', { class: 'btn primary sm', onclick: () => send({ type: 'open', target: 'https://ollama.com/download' }) }, icon('external', 14), '前往 Ollama 官網下載')));
    } else if (!L.running) {
      status.append(h('div', { class: 'notice-line warn' }, icon('alert', 15), 'Ollama 沒有回應' + (L.error ? '：' + L.error : '')));
    } else {
      status.append(h('div', { class: 'notice-line' }, h('span', { style: { color: 'var(--green)' } }, icon('check', 15)), 'Ollama 正在執行，CCA 需要時會自動啟動它。'));
    }
    const hw = L.hardware || {};
    const gpu = (hw.gpus || []).filter((g) => !g.integrated);
    const machine = h('div', { class: 'card' }, h('div', { class: 'card-title' }, '這台電腦'),
      h('div', { class: 'kv' }, h('span', {}, 'CPU'), h('span', {}, hw.cpu || '-')),
      h('div', { class: 'kv' }, h('span', {}, '記憶體'), h('span', {}, gbText(hw.ramGb) + (hw.freeRamGb != null ? '（目前可用 ' + gbText(hw.freeRamGb) + '）' : ''))),
      h('div', { class: 'kv' }, h('span', {}, '顯示卡'), h('span', {}, gpu.length ? gpu.map((g) => g.name + '（' + gbText(g.vramGb) + '）').join('、') : '沒有獨立顯示卡')));
    box.append(h('div', { class: 'daily-grid' }, status, machine));

    // The two reviewer slots.
    const models = L.models || [];
    const slots = (S.settings.locals) || {};
    const slotCard = (slot) => {
      const cur = (slots[slot] || {}).model || '';
      const enabled = (slots[slot] || {}).enabled !== false;
      // A reviewer reads the files itself through tool calls; models without them cannot review.
      const sel = h('select', { class: 'select', style: { width: '100%' } }, h('option', { value: '' }, '未設定（不參與審查）'),
        models.map((m) => h('option', { value: m.name, disabled: m.tools === false && m.name !== cur },
          m.name + '（' + gbText(m.sizeGb) + (m.params ? '，' + m.params : '') + (m.tools === false ? '，不支援讀檔' : '') + '）')));
      if (cur && !models.some((m) => m.name === cur)) sel.append(h('option', { value: cur }, cur + '（尚未下載）'));
      sel.value = cur;
      sel.addEventListener('change', () => send({ type: 'local.set', slot, model: sel.value }));
      const curModel = models.find((m) => m.name === cur);
      const noTools = curModel && curModel.tools === false;
      const sw = h('button', { class: 'chip keep toggle' + (enabled ? ' on' : ''), onclick: () => send({ type: 'local.set', slot, enabled: !enabled }) },
        h('span', { class: 'switch' }, h('i')), h('span', {}, enabled ? '啟用' : '停用'));
      const persona = personaOf(slot);
      return h('div', { class: 'card', style: { marginTop: 0 } },
        h('div', { class: 'card-title' }, avatar(slot, 20), displayName(slot), h('span', { style: { marginLeft: 'auto' } }, sw)),
        h('div', { class: 'muted', style: { fontSize: '12px', marginBottom: '8px' } }, persona ? '性格：' + persona.name + '　' + (persona.summary || '') : '沒有性格'),
        h('div', { class: 'field', style: { marginBottom: '10px' } }, h('label', {}, '模型'), sel),
        noTools ? h('div', { class: 'notice-line warn', style: { marginBottom: '10px' } }, icon('alert', 15),
          cur + ' 不支援工具呼叫，沒辦法自己讀檔，群組裡會略過這位審查員。請換成支援工具的模型。') : null,
        h('button', { class: 'btn sm secondary', onclick: () => openSettings(slot) }, icon('pencil', 14), '編輯個人資料與性格'));
    };
    box.append(h('h3', {}, '兩位審查員'),
      h('p', { class: 'lead', style: { marginTop: 0 } }, '兩位可以共用同一個模型，只差在性格，最省硬碟和顯示卡記憶體。'),
      h('div', { class: 'daily-grid' }, slotCard('local1'), slotCard('local2')));

    // Recommendations for this hardware, with download progress.
    const installed = new Set(models.map((m) => m.name));
    const rec = L.recommended || [];
    const pullRow = (model) => {
      const p = S.pulls[model];
      if (!p) return null;
      const pct = p.total ? Math.round((p.completed || 0) / p.total * 100) : 0;
      return h('div', { style: { marginTop: '6px' } }, meter(p.status || '下載中', pct, p.total ? `${gbText((p.completed || 0) / 1073741824)} / ${gbText(p.total / 1073741824)}` : (p.status || '')));
    };
    box.append(h('h3', {}, '推薦的模型（依照這台電腦的硬體）'),
      rec.length ? h('div', {}, rec.map((r) => {
        const has = installed.has(r.model) || installed.has(r.model.replace(/:latest$/, ''));
        const busy = !!S.pulls[r.model];
        return h('div', { class: 'prow' },
          h('div', { class: 'p-icon', style: { background: r.recommended ? 'var(--brand)' : 'var(--bg-side-hover)' } }, icon('code', 20)),
          h('div', { class: 'p-main' },
            h('div', { class: 'p-name' }, r.label, h('span', { class: 'tag mono' }, r.model), r.recommended ? h('span', { class: 'inst-tag' }, '推薦') : null, has ? h('span', { class: 'tag' }, '已下載') : null),
            h('div', { class: 'p-desc' }, r.why),
            h('div', { class: 'p-meta' }, gbText(r.sizeGb) + ' · ' + (FIT[r.fit] || '')), pullRow(r.model)),
          busy ? h('button', { class: 'btn sm secondary', onclick: () => send({ type: 'local.cancel', model: r.model }) }, '取消')
            : has ? h('button', { class: 'btn sm outline', onclick: () => confirmDeleteModel(r.model) }, '刪除')
              : h('button', { class: 'btn sm success', disabled: !L.running, onclick: () => send({ type: 'local.pull', model: r.model }) }, icon('download', 15), '下載'));
      })) : h('div', { class: 'muted', style: { fontSize: '13px' } }, '這台電腦的記憶體不太夠跑本地模型。'));
    const others = models.filter((m) => !rec.some((r) => r.model === m.name));
    if (others.length) {
      box.append(h('h3', {}, '其他已下載的模型'), h('div', {}, others.map((m) => h('div', { class: 'prow' },
        h('div', { class: 'p-icon', style: { background: 'var(--bg-side-hover)' } }, icon('code', 20)),
        h('div', { class: 'p-main' }, h('div', { class: 'p-name' }, m.name), h('div', { class: 'p-meta' }, [gbText(m.sizeGb), m.params, m.quant].filter(Boolean).join(' · '))),
        h('button', { class: 'btn sm outline', onclick: () => confirmDeleteModel(m.name) }, '刪除')))));
    }
  }

  function confirmDeleteModel(model) {
    modal({ title: '刪除 ' + model + '？', desc: '會從 Ollama 刪除這個模型的檔案。之後要用需要重新下載。',
      foot: (close) => [h('button', { class: 'btn ghost', onclick: close }, '取消'),
        h('button', { class: 'btn danger', onclick: () => { send({ type: 'local.delete', model }); close(); } }, '刪除')] });
  }

  // ================================================================ rollback
  function msgIndex(id) { return (S.msgs[S.current] || []).findIndex((x) => x.id === id); }
  function rollbackDialog(m) {
    const c = conv();
    const list = S.msgs[c.id] || [];
    const idx = msgIndex(m.id);
    if (idx < 0) return;
    const later = list.slice(idx);
    const hasSnap = later.some((x) => x.snapshot);
    const files = {};
    for (const x of later) for (const b of x.blocks || []) if (b.type === 'file') files[b.path] = true;
    const fileList = Object.keys(files);
    const fBox = h('input', { type: 'checkbox', checked: hasSnap, disabled: !hasSnap });
    const cBox = h('input', { type: 'checkbox', checked: true });
    const go = h('button', { class: 'btn danger' }, '回滾');
    const sync = () => { go.disabled = !fBox.checked && !cBox.checked; };
    fBox.addEventListener('change', sync);
    cBox.addEventListener('change', sync);
    modal({
      title: '回滾到這則訊息之前',
      desc: '回到「' + (m.author === 'user' ? displayName('user') : displayName(m.author)) + '・' + stamp(m.ts) + '」這則訊息之前的狀態。',
      body: [
        h('label', { class: 'check', style: { alignItems: 'flex-start' } }, fBox, h('div', {}, h('div', {}, '回滾工作：還原專案檔案'),
          h('div', { class: 'muted', style: { fontSize: '13px', marginTop: '2px' } }, hasSnap
            ? (fileList.length ? '之後改過的檔案：' + fileList.slice(0, 8).join('、') + (fileList.length > 8 ? ` 等 ${fileList.length} 個` : '') : '之後 AI 沒有回報改過檔案，但仍會依檢查點還原。') + ' 之後新增的檔案會被刪除。'
            : '這則訊息之後沒有檢查點（例如日常模式或沒有選資料夾），無法還原檔案。'))),
        h('label', { class: 'check', style: { alignItems: 'flex-start', marginTop: '12px' } }, cBox, h('div', {}, h('div', {}, '回滾聊天：刪除這則和之後的訊息'),
          h('div', { class: 'muted', style: { fontSize: '13px', marginTop: '2px' } }, displayName('claude') + ' 和 ' + displayName('codex') + ' 也會忘記這之後的內容，從這裡重新接續。'))),
        h('p', { class: 'muted', style: { fontSize: '13px', marginTop: '14px' } }, '還原檔案前，CCA 會先把目前的狀態存成檢查點，回滾後可以按「復原檔案」回來。'),
      ],
      foot: (close) => [h('button', { class: 'btn ghost', onclick: close }, '取消'),
        (go.onclick = () => { send({ type: 'rollback', convId: c.id, msgId: m.id, files: fBox.checked, chat: cBox.checked }); close(); }, go)],
    });
  }

  // ================================================================ daily usage
  // Stacked bars of tokens per day; colors validated with the dataviz validator on #232428.
  const USAGE_SERIES = [
    { key: 'gemini', label: () => displayName('gemini'), color: 'var(--gemini)', agents: ['gemini'] },
    { key: 'claude', label: () => displayName('claude'), color: '#d4794f', agents: ['claude'] },
    { key: 'local', label: () => '本地模型', color: 'var(--local2)', agents: ['local1', 'local2', 'local'] },
    { key: 'api', label: () => 'API 模型', color: 'var(--signal)', agents: ['api'] },
    { key: 'codex', label: () => displayName('codex'), color: '#22a07b', agents: ['codex'] },
  ];
  function dayKey(d) { return d.getFullYear() + '-' + String(d.getMonth() + 1).padStart(2, '0') + '-' + String(d.getDate()).padStart(2, '0'); }
  function usageDays(n) {
    const daily = (S.usage && S.usage.daily) || {};
    const out = [];
    for (let i = n - 1; i >= 0; i--) {
      const d = new Date(); d.setDate(d.getDate() - i);
      const rec = daily[dayKey(d)] || {};
      const row = { date: d, key: dayKey(d), total: 0, cost: 0, turns: 0 };
      for (const s of USAGE_SERIES) {
        row[s.key] = s.agents.reduce((a, ag) => a + ((rec[ag] || {}).input || 0) + ((rec[ag] || {}).output || 0), 0);
        row.total += row[s.key];
      }
      row.cost = (rec.claude || {}).cost || 0;
      for (const ag of ['claude', 'codex', 'gemini', 'local1', 'local2', 'api', 'local']) row.turns += (rec[ag] || {}).turns || 0;
      out.push(row);
    }
    return out;
  }

  function usageCard() {
    const days = usageDays(14);
    const today = days[days.length - 1];
    const card = h('div', { class: 'card usage-card', style: { marginTop: 0, marginBottom: '16px' } }, h('div', { class: 'card-title' }, '每日用量（最近 14 天）',
      h('button', { class: 'act', onclick: () => { S.usageTable = !S.usageTable; renderAll(); } }, S.usageTable ? '顯示圖表' : '顯示表格')));
    const tile = (label, value, sub) => h('div', { class: 'stat-tile' }, h('div', { class: 'st-label' }, label), h('div', { class: 'st-value' }, value), sub ? h('div', { class: 'st-sub' }, sub) : null);
    card.append(h('div', { class: 'stat-row' },
      tile('今天的 token', tok(today.total), today.turns + ' 次回覆'),
      USAGE_SERIES.map((s) => tile(s.label(), tok(today[s.key]), null)),
      tile('Claude 等值 API 估算', 'US$' + today.cost.toFixed(2), '訂閱不另外收費')));
    if (S.usageTable) {
      const table = h('table', { class: 'md-table', style: { width: '100%', marginTop: '10px' } },
        h('thead', {}, h('tr', {}, h('th', {}, '日期'), USAGE_SERIES.map((s) => h('th', {}, s.label())), h('th', {}, '合計'))),
        h('tbody', {}, days.slice().reverse().map((d) => h('tr', {}, h('td', {}, d.key), USAGE_SERIES.map((s) => h('td', {}, tok(d[s.key]))), h('td', {}, tok(d.total))))));
      card.append(h('div', { class: 'md-table-wrap' }, table));
      return card;
    }
    const W = 640, H = 170, padL = 40, padB = 22, padT = 8;
    const max = Math.max(1, ...days.map((d) => d.total));
    const nice = Math.pow(10, Math.floor(Math.log10(max)));
    const top = Math.ceil(max / nice) * nice;
    const bw = (W - padL) / days.length;
    const svgNS = 'http://www.w3.org/2000/svg';
    const el = (tag, attrs) => { const e = document.createElementNS(svgNS, tag); for (const k in attrs) e.setAttribute(k, attrs[k]); return e; };
    const svg = el('svg', { viewBox: `0 0 ${W} ${H}`, class: 'usage-chart', role: 'img', 'aria-label': '最近 14 天每日 token 用量' });
    for (let i = 0; i <= 2; i++) {
      const v = top * i / 2, y = H - padB - (H - padB - padT) * i / 2;
      svg.append(el('line', { x1: padL, x2: W, y1: y, y2: y, class: 'grid' }));
      const t = el('text', { x: padL - 6, y: y + 4, class: 'axis', 'text-anchor': 'end' }); t.textContent = tok(v); svg.append(t);
    }
    const tip = h('div', { class: 'tooltip chart-tip hidden' });
    days.forEach((d, i) => {
      const x = padL + i * bw + bw * 0.2, w = bw * 0.6;
      let y = H - padB;
      USAGE_SERIES.forEach((s) => {
        const hgt = (H - padB - padT) * d[s.key] / top;
        if (hgt <= 0) return;
        // 2px surface gap between stacked segments.
        svg.append(el('rect', { x, y: y - hgt + 1, width: w, height: Math.max(0, hgt - 2), rx: 2, fill: s.color }));
        y -= hgt;
      });
      if (i % 2 === (days.length - 1) % 2) {
        const t = el('text', { x: x + w / 2, y: H - 6, class: 'axis', 'text-anchor': 'middle' });
        t.textContent = (d.date.getMonth() + 1) + '/' + d.date.getDate(); svg.append(t);
      }
      const hit = el('rect', { x: padL + i * bw, y: 0, width: bw, height: H - padB, fill: 'transparent' });
      hit.addEventListener('mouseenter', (ev) => {
        put(tip, h('div', { style: { fontWeight: 600, marginBottom: '4px' } }, d.key),
          USAGE_SERIES.map((s) => h('div', { class: 'tip-row' }, h('span', { class: 'dotc', style: { background: s.color } }), s.label(), h('span', { style: { marginLeft: 'auto' } }, tok(d[s.key])))),
          h('div', { class: 'tip-row', style: { borderTop: '1px solid var(--divider)', marginTop: '4px', paddingTop: '4px' } }, '合計', h('span', { style: { marginLeft: 'auto' } }, tok(d.total))));
        tip.classList.remove('hidden');
        const r = ev.target.getBoundingClientRect();
        tip.style.left = Math.min(window.innerWidth - 220, r.right + 6) + 'px';
        tip.style.top = Math.max(8, r.top + 20) + 'px';
      });
      hit.addEventListener('mouseleave', () => tip.classList.add('hidden'));
      svg.append(hit);
    });
    card.append(h('div', { class: 'chart-legend' }, USAGE_SERIES.map((s) => h('span', {}, h('span', { class: 'dotc', style: { background: s.color } }), s.label()))), svg, tip);
    if (!days.some((d) => d.total)) card.append(h('div', { class: 'muted', style: { fontSize: '13px', marginTop: '4px' } }, '還沒有用量紀錄。開始對話後，每天的 token 用量會記在這裡。'));
    return card;
  }

  // ================================================================ plugins page
  function renderPluginsPage(main, light) {
    // Keep the search box (and its focus) when only the list changes.
    if (!light || !main.querySelector('.plugins-page')) {
      const search = h('input', { placeholder: '搜尋插件', value: S.pluginQuery });
      search.addEventListener('input', () => { S.pluginQuery = search.value; S.pluginLimit = 60; renderPluginList(); });
      put(main,
        h('div', { class: 'chat-head' }, h('div', { class: 'title' }, icon('puzzle', 22), h('span', {}, '插件商店')), h('div', { class: 'head-sep' }),
          h('div', { class: 'tabs', id: 'plugin-tabs' })),
        h('div', { class: 'page plugins-page' }, h('div', { class: 'page-inner' },
          h('div', { class: 'muted', style: { fontSize: '14px', lineHeight: 1.5, marginBottom: '12px' } },
            'Claude 和 GPT 的插件格式不同，每個插件只能給其中一個 AI 使用，清單上會標示是給誰的。MCP 伺服器兩邊都能用，可以一次裝給兩個。安裝後，下一則訊息開始生效。'),
          h('div', { class: 'searchbox', id: 'plugin-search' }, search, icon('search', 20)),
          h('div', { id: 'plugin-list' }))));
    }
    const tabs = $('#plugin-tabs');
    const count = (a) => (S.plugins[a] && S.plugins[a].installed ? S.plugins[a].installed.length : null);
    put(tabs,
      h('button', { class: `tab${S.pluginTab === 'claude' ? ' sel' : ''}`, onclick: () => { S.pluginTab = 'claude'; S.pluginLimit = 60; renderAll(); } }, 'Claude 插件', count('claude') != null ? h('span', { class: 'badge' }, count('claude')) : null),
      h('button', { class: `tab${S.pluginTab === 'codex' ? ' sel' : ''}`, onclick: () => { S.pluginTab = 'codex'; S.pluginLimit = 60; renderAll(); } }, 'GPT 插件', count('codex') != null ? h('span', { class: 'badge' }, count('codex')) : null),
      h('button', { class: `tab${S.pluginTab === 'mcp' ? ' sel' : ''}`, onclick: () => { S.pluginTab = 'mcp'; renderAll(); } }, 'MCP 伺服器'),
    );
    $('#plugin-search').classList.toggle('hidden', S.pluginTab === 'mcp');
    renderPluginList();
  }

  function renderPluginList() {
    const box = $('#plugin-list');
    if (!box) return;
    clear(box);
    if (S.pluginTab === 'mcp') { renderMcp(box); return; }
    const agent = S.pluginTab;
    const data = S.plugins[agent];
    const busyList = S.busy['plugins.' + agent];
    if (!data || busyList) {
      box.append(h('div', { class: 'empty' }, h('div', { class: 'spinner', style: { margin: '0 auto 10px' } }), `正在讀取 ${displayName(agent)} 的插件清單…`));
      return;
    }
    if (data.error) {
      box.append(h('div', { class: 'empty' }, icon('alert', 40), h('div', {}, data.error),
        h('button', { class: 'btn secondary', style: { marginTop: '12px' }, onclick: () => send({ type: 'plugins.list', agent, refresh: true }) }, '重新讀取')));
      return;
    }
    const installedIds = new Set((data.installed || []).map((p) => p.id));
    const q = S.pluginQuery.trim().toLowerCase();
    let items = (data.available || []).map((p) => ({ ...p, installed: p.installed || installedIds.has(p.id) }));
    // Installed plugins that are no longer listed by any marketplace still show up.
    for (const p of data.installed || []) {
      if (!items.some((x) => x.id === p.id)) items.push({ id: p.id, name: p.id.split('@')[0], description: '', marketplace: p.id.split('@')[1] || '', installed: true });
    }
    if (q) items = items.filter((p) => [p.name, p.displayName, p.description, p.marketplace, p.id].some((s) => (s || '').toLowerCase().includes(q)));
    items.sort((a, b) => (b.installed - a.installed) || ((b.installs || 0) - (a.installs || 0)) || (a.displayName || a.name || '').localeCompare(b.displayName || b.name || ''));
    box.append(h('div', { class: 'list-head' }, `${q ? '搜尋結果' : '全部'} — ${items.length}`,
      h('button', { class: 'btn sm outline act', onclick: () => send({ type: 'plugins.list', agent, refresh: true }) }, icon('refresh', 14), '重新整理')));
    if (!items.length) { box.append(h('div', { class: 'empty' }, icon('search', 40), h('div', {}, '找不到符合的插件'))); return; }
    for (const p of items.slice(0, S.pluginLimit)) box.append(pluginRow(agent, p));
    if (items.length > S.pluginLimit) {
      box.append(h('div', { style: { textAlign: 'center', marginTop: '12px' } },
        h('button', { class: 'btn secondary', onclick: () => { S.pluginLimit += 60; renderPluginList(); } }, `顯示更多（還有 ${items.length - S.pluginLimit} 個）`)));
    }
  }

  function pluginRow(agent, p) {
    const name = p.displayName || p.name || p.id;
    const busyKey = `install.${agent}.${p.id}`;
    const busy = S.busy[busyKey];
    const action = busy ? h('button', { class: 'btn sm secondary', disabled: true }, h('span', { class: 'spinner' }), '處理中')
      : p.installed ? h('button', { class: 'btn sm outline', onclick: () => send({ type: 'plugins.uninstall', agent, id: p.id }) }, '移除')
        : h('button', { class: 'btn sm success', onclick: () => send({ type: 'plugins.install', agent, id: p.id, name: p.name, displayName: name, marketplaceName: p.marketplaceName, marketplacePath: p.marketplacePath }) }, icon('download', 15), '安裝');
    return h('div', { class: 'prow' },
      h('div', { class: 'p-icon', style: { background: hashColor(p.id) } }, (name || '?').replace(/[^\p{L}\p{N}]/gu, '').slice(0, 1).toUpperCase() || '?'),
      h('div', { class: 'p-main' },
        h('div', { class: 'p-name' }, name, h('span', { class: `for-tag ${agent}` }, '給 ' + displayName(agent)), p.installed ? h('span', { class: 'inst-tag' }, '已安裝') : null),
        p.description ? h('div', { class: 'p-desc' }, p.description) : null,
        h('div', { class: 'p-meta' }, [p.marketplace ? '來源：' + p.marketplace : '', p.developer ? '開發者：' + p.developer : '', p.installs ? `${tok(p.installs)} 次安裝` : ''].filter(Boolean).join(' · '))),
      action);
  }

  function renderMcp(box) {
    box.append(h('div', { class: 'list-head' }, 'MCP 伺服器讓 AI 使用外部工具（資料庫、瀏覽器、API…）',
      h('button', { class: 'btn sm primary act', onclick: mcpDialog }, icon('plus', 14), '新增 MCP 伺服器')));
    for (const agent of ['claude', 'codex']) {
      const d = S.mcp[agent];
      box.append(h('div', { class: 'list-head' }, h('span', { class: `for-tag ${agent}` }, '給 ' + displayName(agent)), d && d.items ? `— ${d.items.length}` : ''));
      if (!d) { box.append(h('div', { class: 'muted', style: { fontSize: '13px', padding: '8px 10px' } }, '讀取中…')); continue; }
      if (d.error) box.append(h('div', { class: 'notice-line warn', style: { padding: '0 10px' } }, icon('alert', 15), d.error));
      if (!d.items || !d.items.length) { box.append(h('div', { class: 'muted', style: { fontSize: '13px', padding: '8px 10px' } }, '還沒有 MCP 伺服器')); continue; }
      for (const s of d.items) {
        const target = s.transport === 'stdio' ? [s.command, ...(s.args || [])].filter(Boolean).join(' ') : s.url;
        box.append(h('div', { class: 'prow' },
          h('div', { class: 'p-icon', style: { background: hashColor(s.name) } }, icon(s.transport === 'stdio' ? 'terminal' : 'globe', 20)),
          h('div', { class: 'p-main' }, h('div', { class: 'p-name' }, s.name, h('span', { class: 'plan-badge' }, s.transport === 'stdio' ? '本機指令' : 'HTTP'),
            s.enabled === false ? h('span', { class: 'plan-badge' }, '已停用') : null),
            h('div', { class: 'p-desc mono', style: { fontSize: '12.5px' } }, target || '')),
          h('button', { class: 'btn sm outline', onclick: () => send({ type: 'mcp.remove', agent, name: s.name }) }, '移除')));
      }
    }
  }

  function mcpDialog() {
    const name = h('input', { class: 'input', placeholder: '例如 filesystem' });
    let transport = 'stdio';
    const cmd = h('input', { class: 'input mono', placeholder: '例如 npx' });
    const args = h('textarea', { class: 'input', placeholder: '每行一個參數，例如：\n-y\n@modelcontextprotocol/server-filesystem\nC:\\Projects' });
    const env = h('textarea', { class: 'input', placeholder: '每行一個，例如 API_KEY=xxxx（選填）', rows: 2 });
    const url = h('input', { class: 'input mono', placeholder: 'https://example.com/mcp' });
    const forClaude = h('input', { type: 'checkbox', checked: true });
    const forCodex = h('input', { type: 'checkbox', checked: true });
    const stdioBox = h('div', {}, h('div', { class: 'field' }, h('label', {}, '啟動指令'), cmd), h('div', { class: 'field' }, h('label', {}, '參數'), args),
      h('div', { class: 'field' }, h('label', {}, '環境變數'), env));
    const httpBox = h('div', { class: 'hidden' }, h('div', { class: 'field' }, h('label', {}, '網址'), url));
    const seg = h('div', { class: 'seg' });
    const paintSeg = () => {
      put(seg,
        h('button', { class: transport === 'stdio' ? 'sel' : '', onclick: () => { transport = 'stdio'; paintSeg(); } }, '本機指令（stdio）'),
        h('button', { class: transport === 'http' ? 'sel' : '', onclick: () => { transport = 'http'; paintSeg(); } }, '遠端（HTTP）'));
      stdioBox.classList.toggle('hidden', transport !== 'stdio');
      httpBox.classList.toggle('hidden', transport !== 'http');
    };
    paintSeg();
    modal({
      title: '新增 MCP 伺服器',
      desc: 'MCP 伺服器會以你的身分在這台電腦上執行。只加入你信任的來源。',
      wide: true,
      body: [h('div', { class: 'field' }, h('label', {}, '名稱'), name, h('div', { class: 'hint' }, '英文字母、數字、- 和 _')),
        h('div', { class: 'field' }, h('label', {}, '類型'), seg), stdioBox, httpBox,
        h('div', { class: 'field' }, h('label', {}, '安裝給'),
          h('div', { style: { display: 'flex', gap: '20px' } }, h('label', { class: 'check' }, forClaude, 'Claude'), h('label', { class: 'check' }, forCodex, 'GPT')))],
      foot: (close) => [h('button', { class: 'btn ghost', onclick: close }, '取消'),
        h('button', { class: 'btn primary', onclick: () => {
          const targets = [forClaude.checked && 'claude', forCodex.checked && 'codex'].filter(Boolean);
          if (!targets.length) { toast('error', '至少選一個 AI'); return; }
          const envObj = {};
          for (const line of env.value.split('\n')) {
            const k = line.indexOf('=');
            if (k > 0) envObj[line.slice(0, k).trim()] = line.slice(k + 1).trim();
          }
          send({ type: 'mcp.add', targets, name: name.value.trim(), transport, command: cmd.value.trim(), url: url.value.trim(),
            args: args.value.split('\n').map((s) => s.trim()).filter(Boolean), env: envObj });
          close();
        } }, '新增')],
    });
  }

  // ================================================================ account page
  // Claude Code only reports a usage figure near the limit; the live query is opt-in because it
  // reads Claude Code's stored sign-in (src/claude_usage.cpp).
  function claudeLiveToggle() {
    const on = !!(S.settings && S.settings.claudeUsageLive);
    const live = ((S.usage || {}).claude || {}).live || {};
    return h('div', { class: 'live-usage' },
      h('label', { class: 'check-row' },
        h('input', { type: 'checkbox', checked: on, onchange: (e) => send({ type: 'claudeUsage.set', enabled: e.target.checked }) }),
        h('span', {}, '即時查詢 Claude 額度')),
      h('div', { class: 'muted', style: { fontSize: '12px', lineHeight: 1.5 } },
        '每 60 秒查一次，Claude 工作時每 20 秒，每輪回覆後也會更新。開啟後，CCA 會讀取 Claude Code 在這台電腦保存的登入權杖，只用來向 Anthropic 查詢額度，不會保存、記錄或傳到其他地方。'),
      on && live.checkedAt ? h('div', { class: `muted${live.ok ? '' : ' warn-text'}`, style: { fontSize: '12px', marginTop: '4px' } },
        live.ok ? '上次更新：' + new Date(live.checkedAt).toLocaleTimeString('zh-TW') : live.error) : null);
  }

  function renderAccountPage(main) {
    const card = (agent) => {
      const a = AGENTS[agent];
      const au = S.auth[agent] || {};
      const ok = au.state === 'ok';
      return h('div', { class: 'acct-card' },
        h('div', { class: 'banner-strip', style: bannerStyle(agent) }),
        h('div', { class: 'acct-top' }, avatar(agent, 64, agentStatus(agent))),
        h('div', { class: 'acct-body' },
          h('div', { class: 'acct-name' }, displayName(agent), ' ', h('span', { class: `plan-badge ${ok ? 'ok' : 'bad'}` }, au.planLabel || stateText(au.state))),
          h('div', { class: 'kv' }, h('span', {}, '狀態'), h('span', {}, stateText(au.state))),
          au.email ? h('div', { class: 'kv' }, h('span', {}, '帳號'), h('span', {}, au.email)) : null,
          h('div', { class: 'kv' }, h('span', {}, a.cli + ' 版本'), h('span', {}, au.version || '-')),
          h('div', { class: 'kv' }, h('span', {}, '執行檔'), h('span', { class: 'mono', 'data-tip': au.exe }, au.exe || '找不到')),
          au.message && !ok ? h('div', { class: 'notice-line warn', style: { marginTop: '6px' } }, icon('alert', 15), au.message) : null,
          agent === 'gemini' && au.state === 'missing' ? h('div', { class: 'codeblock' }, h('pre', {}, h('code', {}, a.install))) : null,
          h('div', { class: 'card' }, h('div', { class: 'card-title' }, '額度'), usageBlock(agent), agent === 'claude' ? claudeLiveToggle() : null),
          h('div', { style: { display: 'flex', gap: '8px', marginTop: '12px', flexWrap: 'wrap' } },
            h('button', { class: 'btn sm secondary', onclick: () => send({ type: 'auth.check', agent }) }, icon('refresh', 14), '重新檢查'),
            !ok && au.state !== 'missing' ? h('button', { class: `btn sm ${au.state === 'retired' ? 'secondary' : 'primary'}`, onclick: () => send({ type: 'auth.login', agent }) },
              au.state === 'retired' ? '改用其他帳號登入' : '登入') : null,
            au.state === 'retired' ? h('button', { class: 'btn sm primary', onclick: () => send({ type: 'open', target: a.notice }) }, '查看官方說明') : null,
            au.state === 'noPlan' ? h('button', { class: 'btn sm success', onclick: () => send({ type: 'open', target: a.upgrade }) }, '升級方案') : null,
            ok && agent !== 'gemini' ? h('button', { class: 'btn sm outline', onclick: () => { S.pluginTab = agent; openView('plugins'); } }, '管理插件') : null)));
    };
    put(main,
      h('div', { class: 'chat-head' }, h('div', { class: 'title' }, icon('chart', 22), h('span', {}, '訂閱與用量'))),
      h('div', { class: 'page' }, h('div', { class: 'page-inner' },
        h('div', { class: 'muted', style: { fontSize: '14px', lineHeight: 1.5, marginBottom: '16px' } },
          'CCA 使用官方 Claude Code、Codex 與 Gemini CLI。Gemini 可開私訊、加入一般群組或多代理流程。Google 已於 2026-06-18 停止免費層與 Google One 個人帳號使用 Gemini CLI，需改用仍受支援的付費或工作區帳號；方案與額度依 Google 實際回報，每 60 秒更新。'),
        window.CCAActivity?.render(S.usage.activity, { h, mode: S.activityMode, onMode: mode => {
          S.activityMode = mode; renderAll();
          requestAnimationFrame(() => document.querySelector('.activity-tab.sel')?.focus());
        } }),
        usageCard(),
        h('div', { class: 'acct-grid' }, card('claude'), card('codex'), card('gemini')))));
  }
  function stateText(s) {
    return { checking: '檢查中…', ok: '已通過', missing: '找不到 CLI', loggedOut: '尚未登入', wrongMethod: '登入方式不對', noPlan: '方案不符', error: '檢查未完成', serviceError: '已登入，服務檢查未通過', retired: '個人帳號已停用', loggingIn: '等待登入…' }[s] || s || '-';
  }

  // ================================================================ sign-in gate
  let gateEl = null;
  function renderGate() {
    const ok = S.auth.ok || S.apiSetup || Object.keys(S.settings.providers || {}).length > 0;
    if (ok) { if (gateEl) { gateEl.remove(); gateEl = null; } return; }
    if (!gateEl) { gateEl = h('div', { class: 'gate' }); document.body.append(gateEl); }
    const row = (agent, need) => {
      const a = AGENTS[agent];
      const au = S.auth[agent] || {};
      const st = au.state || 'checking';
      const stateEl = st === 'ok' ? h('span', { class: 'ok' }, icon('check', 22))
        : st === 'checking' || st === 'loggingIn' ? h('span', { class: 'spinner', style: { width: '18px', height: '18px' } })
          : h('span', { class: 'bad' }, icon('x', 22));
      const actions = [];
      if (st === 'missing') {
        actions.push(h('div', { class: 'install-cmd' }, h('div', { class: 'muted', style: { fontSize: '13px', marginBottom: '4px' } }, a.installNote),
          h('div', { class: 'codeblock', style: { maxWidth: '100%' } }, h('pre', {}, h('code', {}, a.install)),
            h('button', { class: 'code-copy', style: { opacity: 1 } }, '複製'))));
      }
      if (st === 'loggedOut' || st === 'wrongMethod' || st === 'error') actions.push(h('button', { class: 'btn sm primary', onclick: () => send({ type: 'auth.login', agent }) }, agent === 'claude' ? '登入 Claude' : agent === 'gemini' ? '登入 Gemini' : '登入 ChatGPT'));
      if (st === 'noPlan') actions.push(h('button', { class: 'btn sm success', onclick: () => send({ type: 'open', target: a.upgrade }) }, '查看方案'));
      if (st !== 'checking' && st !== 'ok') actions.push(h('button', { class: 'btn sm secondary', onclick: () => send({ type: 'auth.check', agent }) }, '重新檢查'));
      if (st === 'missing') actions.push(h('button', { class: 'btn sm ghost', onclick: settingsDialog }, '指定執行檔位置'));
      return h('div', { class: 'gate-row' }, avatar(agent, 40),
        h('div', { class: 'gr-main' },
          h('div', { class: 'gr-name' }, displayName(agent), h('span', { class: 'muted', style: { fontWeight: 500, fontSize: '13px' } }, `需要 ${need}`)),
          h('div', { class: 'gr-msg' }, st === 'ok' ? `${au.planLabel || '已訂閱'}${au.email ? ' · ' + au.email : ''}` : st === 'checking' ? `正在檢查 ${a.cli}…` : au.message || stateText(st)),
          actions.length ? h('div', { class: 'gr-actions' }, actions) : null),
        h('div', { class: 'gr-state' }, stateEl));
    };
    put(gateEl, h('div', { class: 'gate-card' },
      h('img', { class: 'gate-logo', src: 'logo.svg', alt: '' }),
      h('h1', {}, '確認可用的訂閱'),
      h('p', {}, 'ChatClaudeAgent（CCA）讓 Claude 和 GPT 一起寫程式，用的是你自己的訂閱額度，登入可用的訂閱即可開始；可指定 GPT 主導工作。'),
      row('claude', 'Claude Pro 以上'),
      row('codex', 'ChatGPT Plus 以上'),
      row('gemini', '受支援的 Google 付費或工作區帳號'),
      h('button', { class: 'btn secondary', onclick: () => { S.apiSetup = true; renderGate(); openSettings('platform'); } }, '改用 API，設定獨立計費的供應商'),
      h('div', { class: 'gate-foot' }, 'CCA 只讀取官方 CLI 回報的登入狀態和方案名稱，不會保存你的密碼和權杖。只有在你開啟「即時查詢 Claude 額度」後，才會讀取 Claude Code 的登入權杖來查詢額度。')));
  }

  // ================================================================ toasts, tooltips, misc
  function toast(level, text) {
    let box = $('.toasts');
    if (!box) { box = h('div', { class: 'toasts' }); $('#layers').append(box); }
    const t = h('div', { class: `toast ${level}` }, icon(level === 'error' ? 'alert' : level === 'ok' ? 'check' : 'info', 18), h('div', {}, text));
    box.append(t);
    setTimeout(() => t.remove(), level === 'error' ? 7000 : 4000);
  }
  function copyText(text) {
    const done = () => toast('ok', '已複製');
    if (navigator.clipboard) navigator.clipboard.writeText(text).then(done, () => fallbackCopy(text, done));
    else fallbackCopy(text, done);
  }
  function fallbackCopy(text, done) {
    const ta = h('textarea', { style: { position: 'fixed', opacity: 0 } });
    ta.value = text;
    document.body.append(ta);
    ta.select();
    try { document.execCommand('copy'); done(); } catch { /* clipboard unavailable */ }
    ta.remove();
  }

  let tipEl = null, tipTimer = null;
  function hideTip() { clearTimeout(tipTimer); if (tipEl) { tipEl.remove(); tipEl = null; } }
  document.addEventListener('mouseover', (e) => {
    const t = e.target.closest('[data-tip]');
    if (!t || !t.getAttribute('data-tip')) { hideTip(); return; }
    if (tipEl && tipEl.dataset.for === t.dataset.tip) return;
    hideTip();
    tipTimer = setTimeout(() => {
      if (!document.body.contains(t)) return;
      tipEl = h('div', { class: 'tooltip', role: 'tooltip' }, t.getAttribute('data-tip'));
      tipEl.dataset.for = t.dataset.tip;
      $('#layers').append(tipEl);
      const r = t.getBoundingClientRect();
      const w = tipEl.offsetWidth, hh = tipEl.offsetHeight;
      let left = r.left + r.width / 2 - w / 2;
      let top = r.top - hh - 8 < 8 ? r.bottom + 8 : r.top - hh - 8;
      if (t.dataset.tipSide === 'right') {
        left = r.right + 12 + w > window.innerWidth - 8 ? r.left - w - 12 : r.right + 12;
        top = r.top + r.height / 2 - hh / 2;
      }
      tipEl.style.left = Math.max(8, Math.min(window.innerWidth - w - 8, left)) + 'px';
      tipEl.style.top = Math.max(8, Math.min(window.innerHeight - hh - 8, top)) + 'px';
    }, 350);
  });

  // Links in AI replies and code copy buttons.
  document.addEventListener('click', (e) => {
    const a = e.target.closest('a[href]');
    if (a) {
      e.preventDefault();
      const href = a.getAttribute('href');
      if (a.dataset.localPath) send({ type: 'file.reveal', target: a.dataset.localPath });
      else if (/^https?:\/\//i.test(href)) send({ type: 'open', target: href });
      return;
    }
    const cp = e.target.closest('.code-copy');
    if (cp) {
      const code = cp.parentElement.querySelector('code');
      if (code) copyText(code.innerText);
    }
  });
  let wasDocked = panelDocked();
  window.addEventListener('resize', () => {
    hideTip();
    closePop();
    if (panelDocked() !== wasDocked) { wasDocked = panelDocked(); S.panelDrawer = false; renderAll(); }
    autoGrow();  // Measure after the side panel has changed the input's available width.
  });
  document.addEventListener('scroll', () => { if (tipEl) hideTip(); }, true);
  document.addEventListener('pointerdown', hideTip, true);
  // Keep relative times ("今天 下午3:24", reset countdowns) fresh.
  setInterval(() => { if (S.view !== 'chat') schedule(); else renderPanel(); }, 60000);

  // ================================================================ start
  // Mock mode only: lets mock.js jump straight to a screen for visual checks.
  if (window.MockBackend) {
    window.CCA_DEV = { openGit: id => openGit(S.convs.find(x => x.id === id) || conv()), openView, openConv, settingsDialog, createGroupDialog, showHelp, openSettings,
      rollbackFirst: () => { const m = (S.msgs[S.current] || []).find((x) => x.snapshot); if (m) rollbackDialog(m); },
      openModelPicker: (agent) => { const el = chat.head.querySelector('.head-facts'); if (el) modelPicker(el, conv(), agent); },
      openOps: () => { const el = chat.head.querySelector('.head-facts'); if (el) opsPanel(el, conv()); },
      openReview: (scope) => openReview(conv(), scope || 'task'),
      bypassDialog: () => bypassDialog(conv()), mcpDialog };
  }
  window.CCABadges?.setup({ h, put, send, modal, toast });
  window.CCAPlatform?.setup({ h, send, modal, mediaUrl, toast, getModels: () => S.models, getLocal: () => S.local || {},
    getProfiles: () => S.settings.reviewProfiles || [] });
  window.CCACollaboration?.setup({ h, put, send, modal, icon, avatar, name: displayName, markdown: text => MD.render(text),
    settings: () => S.settings, state: stateOf, conv, openConv, jump: id => {
      if (chat.list?.querySelector(`[data-id="${CSS.escape(id)}"]`)) jumpToMsg(id);
      else S.inboxJump = { convId: S.current, id };
    } });
    window.CCAQuality?.setup({ h, put, send, modal, state: stateOf, conv, quality: id => S.quality[id],
      mediaUrl: file => 'https://media.cca.app/' + encodeURIComponent(file) });
    if (native) native.addEventListener('message', (e) => onMessage(e.data));
  else if (window.MockBackend) window.MockBackend.connect(onMessage);
  renderAll();
  renderGate();
  send({ type: 'ready' });
  send({ type: 'platform.get' });
  send({ type: 'inbox.get' });
})();
