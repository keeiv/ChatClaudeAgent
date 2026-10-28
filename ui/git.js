// Version-control pane: colored file tiles to choose what to commit, a commit graph, remote sync and the
// never-commit rule editor. Every Git command runs in the host (src/gitpanel.cpp); this file only renders.
'use strict';
window.CCAGit = {
  create({ S, h, put, icon, send, toast, modal, conv, onChange }) {
    S.git = null;
    const TONE = { new: '新增', changed: '修改', risk: '不建議提交' };
    const KIND = { modified: '修改', added: '新增', untracked: '新增', deleted: '刪除', renamed: '改名', conflict: '衝突' };
    const LANE_COLORS = ['var(--signal)', 'var(--claude)', 'var(--codex)', 'var(--gemini)', 'var(--local1)', 'var(--local2)', 'var(--yellow)', 'var(--green)'];
    const ROW = 30, LANE = 14;
    let sequence = 0;

    function open(c) {
      if (!c) return;
      if (!c.cwd) { toast('error', '請先選擇專案資料夾'); return; }
      S.git = { convId: c.id, data: null, sel: new Set(), allowRisk: new Set(), message: '', task: true, diff: null, busy: '', secrets: [], seeded: false };
      send({ type: 'git.open', id: c.id });
      render();
    }
    function close() { S.git = null; render(); onChange?.(); }
    const base = path => path.split('/').pop();
    const dir = path => path.includes('/') ? path.slice(0, path.lastIndexOf('/')) : '';

    // Lanes are assigned in display order; a merge opens a lane for each extra parent.
    function graph(commits) {
      const known = new Set(commits.map(c => c.hash));
      let lanes = [];
      return commits.map(c => {
        const fromAbove = lanes.includes(c.hash);
        let lane = lanes.indexOf(c.hash);
        if (lane < 0) { lane = lanes.indexOf(null); if (lane < 0) { lane = lanes.length; lanes.push(null); } }
        const before = lanes.slice(); before[lane] = c.hash;
        const merging = before.map((x, i) => (x === c.hash && i !== lane ? i : -1)).filter(i => i >= 0);
        const after = before.slice(); merging.forEach(i => { after[i] = null; });
        const parents = (c.parents || []).filter(p => known.has(p));
        after[lane] = parents[0] || null;
        const targets = parents[0] ? [lane] : [];
        for (const p of parents.slice(1)) {
          let i = after.indexOf(p);
          if (i < 0) { i = after.indexOf(null); if (i < 0) { i = after.length; after.push(null); } after[i] = p; }
          targets.push(i);
        }
        const row = { c, lane, before, merging, targets, fromAbove, width: Math.max(before.length, after.length) };
        lanes = after; while (lanes.length && lanes[lanes.length - 1] === null) lanes.pop();
        return row;
      });
    }
    function rowSvg(row, width) {
      const ns = 'http://www.w3.org/2000/svg', svg = document.createElementNS(ns, 'svg');
      svg.setAttribute('width', width); svg.setAttribute('height', ROW); svg.setAttribute('aria-hidden', 'true');
      const x = i => 8 + i * LANE, mid = ROW / 2, color = i => LANE_COLORS[i % LANE_COLORS.length];
      const line = (d, i) => { const p = document.createElementNS(ns, 'path'); p.setAttribute('d', d); p.setAttribute('stroke', color(i)); p.setAttribute('class', 'git-edge'); svg.append(p); };
      row.before.forEach((hash, i) => { if (hash && hash !== row.c.hash) line(`M${x(i)} 0V${ROW}`, i); });
      if (row.fromAbove) line(`M${x(row.lane)} 0V${mid}`, row.lane);
      row.merging.forEach(i => line(`M${x(i)} 0C${x(i)} ${mid} ${x(row.lane)} ${mid / 2} ${x(row.lane)} ${mid}`, i));
      row.targets.forEach(i => line(`M${x(row.lane)} ${mid}C${x(row.lane)} ${ROW} ${x(i)} ${mid} ${x(i)} ${ROW}`, i));
      const dot = document.createElementNS(ns, 'circle');
      dot.setAttribute('cx', x(row.lane)); dot.setAttribute('cy', mid); dot.setAttribute('r', row.c.task ? 5 : 4);
      dot.setAttribute('fill', row.c.task ? 'var(--bg-chat)' : color(row.lane)); dot.setAttribute('stroke', color(row.lane));
      dot.setAttribute('class', 'git-dot' + (row.c.task ? ' task' : ''));
      svg.append(dot);
      return svg;
    }

    function seedSelection(g) {
      g.sel = new Set((g.data.files || []).filter(f => f.selected).map(f => f.path));
      g.allowRisk = new Set();
      g.seeded = true;
    }
    function toggle(g, f) {
      if (f.locked) { toast('info', `${f.path} 符合永遠不提交規則「${f.rule}」，不會被提交`); return; }
      if (g.sel.has(f.path)) { g.sel.delete(f.path); g.allowRisk.delete(f.path); render(); return; }
      if (f.tone === 'risk') {
        modal({ title: '這個檔案不建議提交', desc: f.path, body: h('ul', { class: 'git-reasons' }, (f.reasons || []).map(r => h('li', {}, r))),
          foot: closeModal => [h('button', { class: 'btn secondary', onclick: closeModal }, '取消'),
            h('button', { class: 'btn primary', onclick: () => { closeModal(); g.sel.add(f.path); g.allowRisk.add(f.path); render(); } }, '仍要提交')] });
        return;
      }
      g.sel.add(f.path); render();
    }
    function addRule(pattern) {
      const rules = [...new Set([...(S.settings.git?.neverCommit || []), pattern])];
      send({ type: 'git.rules', id: S.git?.convId, rules });
    }
    function draft(g) {
      const files = (g.data.files || []).filter(f => g.sel.has(f.path));
      const request = (g.data.taskRequest || '').split('\n')[0].trim();
      const subject = request ? request.slice(0, 72) : `更新 ${files.length} 個檔案`;
      const lines = files.slice(0, 20).map(f => `- ${KIND[f.kind] || '修改'} ${f.path}`);
      return `${subject}\n\n${lines.join('\n')}${files.length > 20 ? `\n- 另有 ${files.length - 20} 個檔案` : ''}`;
    }
    function commit(g, push, allowSecrets = false) {
      const message = g.message.trim();
      if (!message) { toast('error', '請輸入提交訊息'); return; }
      if (!g.sel.size) { toast('error', '請至少選擇一個檔案'); return; }
      g.busy = push ? 'commit-push' : 'commit';
      g.pending = { push };
      send({ type: 'git.commit', id: g.convId, paths: [...g.sel], message, push, task: g.task && !!g.data.task,
        allowRisk: [...g.allowRisk], allowSecrets, requestToken: 'git-' + (++sequence) });
      render();
    }

    function tile(g, f) {
      const selected = g.sel.has(f.path);
      return h('div', { class: `git-tile tone-${f.tone}${selected ? ' selected' : ''}${f.locked ? ' locked' : ''}`, role: 'listitem' },
        h('button', { class: 'git-tile-main', type: 'button', 'aria-pressed': String(selected), 'data-path': f.path,
          'data-tip': (f.reasons || []).join('；') || TONE[f.tone], onclick: () => toggle(g, f) },
          h('span', { class: 'git-tile-icon' }, icon(f.locked ? 'shield' : 'file', 20), selected ? h('i', { class: 'git-check' }, icon('check', 12)) : null),
          h('span', { class: 'git-tile-name' }, base(f.path)),
          h('span', { class: 'git-tile-dir' }, dir(f.path) || '專案根目錄'),
          h('span', { class: 'git-tile-kind' }, f.locked ? '永遠不提交' : (KIND[f.kind] || '修改') + (f.tone === 'risk' ? ' · 不建議' : ''))),
        h('div', { class: 'git-tile-actions' },
          f.kind === 'deleted' ? null : h('button', { class: 'icon-btn', type: 'button', 'aria-label': '查看差異 ' + f.path, 'data-tip': '查看差異',
            onclick: () => { g.diff = { path: f.path, text: null }; send({ type: 'git.diff', id: g.convId, path: f.path, untracked: f.kind === 'untracked' }); render(); } }, icon('eye', 14)),
          f.locked ? null : h('button', { class: 'icon-btn', type: 'button', 'aria-label': '永遠不提交 ' + f.path, 'data-tip': '加入永遠不提交規則',
            onclick: () => addRule(f.path) }, icon('shield', 14))));
    }
    function diffView(g) {
      if (!g.diff) return null;
      const lines = g.diff.text == null ? [h('span', { class: 'muted' }, '讀取中…')]
        : g.diff.text ? g.diff.text.split('\n').slice(0, 2000).map(l => h('span', { class: l.startsWith('+') && !l.startsWith('+++') ? 'add' : l.startsWith('-') && !l.startsWith('---') ? 'del' : l.startsWith('@@') ? 'hunk' : '' }, l + '\n'))
          : [h('span', { class: 'muted' }, '沒有可顯示的文字差異（可能是二進位檔）')];
      return h('div', { class: 'git-diff' }, h('div', { class: 'git-diff-head' }, h('strong', {}, g.diff.path),
        h('button', { class: 'icon-btn', type: 'button', 'aria-label': '關閉差異', onclick: () => { g.diff = null; render(); } }, icon('x', 14))),
      h('pre', {}, lines));
    }

    const plainTop = () => h('div', { class: 'git-top' }, icon('branch', 18), h('strong', {}, '版本控制'), h('span', { class: 'git-grow' }),
      h('button', { class: 'icon-btn', type: 'button', 'aria-label': '關閉版本控制', onclick: close }, icon('x', 18)));
    const hostOf = d => d.auth?.host || d.remote?.host || d.remote?.name || '遠端倉庫';
    const needIdentity = d => !d.identity?.name || !d.identity?.email;

    // Shown before the file tiles whenever the remote needs a sign-in or Git has no author yet.
    // Credentials are typed only into Git Credential Manager or the browser; this page never has a password field.
    function loginPage(g, d) {
      const a = d.auth || {}, who = d.identity || {}, remote = d.remote || {}, host = hostOf(d);
      const needLogin = a.state === 'required', waiting = g.busy === 'login';
      g.who = g.who || { name: who.name || '', email: who.email || '', global: true };
      const step = (title, ...body) => h('section', { class: 'git-login-card' }, h('h3', {}, title), ...body);
      const done = text => h('section', { class: 'git-login-card done' }, h('h3', {}, h('span', { class: 'git-login-ok' }, icon('check', 14)), text));
      let remoteCard = null;
      if (needLogin && a.ssh) {
        remoteCard = step(`登入 ${host}（SSH）`,
          h('p', {}, 'SSH 遠端用金鑰登入。CCA 不會建立、讀取或上傳你的金鑰，請自己完成下面的步驟再重新檢查。'),
          h('ol', { class: 'git-login-steps' },
            h('li', {}, '建立金鑰：', h('code', {}, 'ssh-keygen -t ed25519')),
            h('li', {}, `把 %USERPROFILE%\\.ssh\\id_ed25519.pub 的內容加到 ${host} 的 SSH 金鑰設定`),
            h('li', {}, '測試連線：', h('code', {}, `ssh -T git@${host}`))),
          a.detail ? h('p', { class: 'hint' }, 'Git 回報：' + a.detail) : null);
      } else if (needLogin) {
        const noHelper = a.helper === '', setup = noHelper && !!a.gcm;
        remoteCard = step(`登入 ${host}`,
          h('p', {}, `${remote.name || '遠端'}（${remote.url || host}）需要登入，才能推送、拉取與擷取。`),
          noHelper && !a.gcm
            ? h('p', { class: 'git-login-warn' }, '這台電腦沒有 Git 認證管理員（Git Credential Manager）。請安裝或更新 Git for Windows，再按「重新檢查」。')
            : h('button', { class: 'btn primary', type: 'button', disabled: waiting,
              onclick: () => { g.busy = 'login'; send({ type: 'git.login', id: g.convId, setupHelper: setup }); render(); } },
            waiting ? '等待登入完成…' : `使用瀏覽器登入 ${host}`),
          waiting ? h('p', { class: 'hint', role: 'status' }, '請在跳出的 Git 認證管理員視窗或瀏覽器完成登入，最多等待 5 分鐘。') : null,
          h('p', { class: 'hint' }, '帳號密碼只在 Git 認證管理員的視窗或瀏覽器輸入，CCA 看不到也不保存；登入結果由 Git 認證管理員存進 Windows 認證管理員。'),
          setup ? h('p', { class: 'hint' }, '這台電腦尚未啟用 Git 認證管理員，登入時會一併設定 credential.helper=manager。') : null,
          a.detail ? h('p', { class: 'hint' }, 'Git 回報：' + a.detail) : null);
      } else if (remote.name) {
        remoteCard = done(a.state === 'ok' ? `${host} 已登入` : `${host}：${a.state === 'offline' ? '目前無法連線，稍後再試' : '尚未確認登入狀態'}`);
      }
      let identityCard;
      if (needIdentity(d)) {
        const field = (key, label, type) => h('div', { class: 'field' }, h('label', { for: 'git-id-' + key }, label),
          h('input', { class: 'input', id: 'git-id-' + key, type, value: g.who[key], autocomplete: key === 'email' ? 'email' : 'name',
            oninput: e => { g.who[key] = e.target.value; } }));
        identityCard = step('設定提交身分',
          h('p', {}, 'Git 會把名稱和 e-mail 寫進每個提交；沒有設定就無法提交。'),
          field('name', '名稱', 'text'), field('email', 'e-mail', 'email'),
          h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: g.who.global, onchange: e => { g.who.global = e.target.checked; } }),
            '套用到這台電腦的所有倉庫'),
          h('button', { class: 'btn secondary', type: 'button',
            onclick: () => send({ type: 'git.identity', id: g.convId, name: g.who.name.trim(), email: g.who.email.trim(), global: g.who.global }) }, '保存提交身分'));
      } else {
        identityCard = done(`提交身分：${who.name} <${who.email}>`);
      }
      return h('div', { class: 'git-login' },
        h('div', { class: 'git-login-head' }, icon('shield', 24), h('div', {},
          h('h2', {}, needLogin ? `需要登入 ${host}` : '提交前先設定身分'),
          h('p', { class: 'hint' }, needLogin ? '版本控制要連到遠端倉庫才能推送、拉取與擷取。完成後會回到檔案選擇。' : '完成後會回到檔案選擇。'))),
        remoteCard, identityCard,
        h('div', { class: 'git-login-foot' },
          h('button', { class: 'btn secondary', type: 'button', disabled: waiting, onclick: () => send({ type: 'git.refresh', id: g.convId, checkAuth: true }) }, '重新檢查'),
          h('button', { class: 'link-btn', type: 'button', onclick: () => { g.skipLogin = true; render(); } }, needLogin ? '先略過，只用本機提交' : '先略過')));
    }

    function render() {
      const main = document.querySelector('#main');
      if (!main) return;
      let pane = main.querySelector('.git-pane');
      const c = conv(), g = S.git;
      if (!g || !c || c.id !== g.convId) { pane?.remove(); if (g && (!c || c.id !== g.convId)) S.git = null; return; }
      if (!pane) { pane = h('section', { class: 'git-pane', 'aria-label': '版本控制' }); main.append(pane); }
      const d = g.data;
      if (!d) { put(pane, h('div', { class: 'git-loading' }, h('span', { class: 'spinner' }), '讀取 Git 狀態…')); return; }
      if (!d.ok) {
        put(pane, plainTop(), h('div', { class: 'git-empty' }, d.error || '無法讀取 Git 狀態'));
        return;
      }
      const auth = d.auth || {};
      if (!g.skipLogin && auth.state === 'checking') {
        put(pane, plainTop(), h('div', { class: 'git-loading', role: 'status' }, h('span', { class: 'spinner' }), `確認 ${hostOf(d)} 的登入狀態…`,
          h('button', { class: 'link-btn', type: 'button', onclick: () => { g.skipLogin = true; render(); } }, '略過')));
        return;
      }
      if (!g.skipLogin && (auth.state === 'required' || needIdentity(d))) {
        const keep = document.activeElement?.id;
        put(pane, plainTop(), loginPage(g, d));
        if (keep?.startsWith('git-id-')) pane.querySelector('#' + keep)?.focus();
        return;
      }
      if (!g.seeded) seedSelection(g);
      for (const p of [...g.sel]) if (!(d.files || []).some(f => f.path === p)) g.sel.delete(p);
      const files = d.files || [], auto = d.auto || {}, busy = !!g.busy || d.busy;
      const counts = { new: 0, changed: 0, risk: 0 };
      files.forEach(f => { counts[f.tone]++; });
      const rows = graph(d.commits || []);
      const width = 16 + Math.max(1, ...rows.map(r => r.width)) * LANE;
      const sync = h('div', { class: 'git-sync' },
        h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: !!auto.fetch, onchange: e => send({ type: 'git.auto', id: g.convId, fetch: e.target.checked, pull: e.target.checked && !!auto.pull, minutes: auto.minutes || 5 }) }), '自動擷取'),
        h('label', { class: 'check', 'data-tip': '只在 AI 沒有工作、可以快轉時拉取；有分歧時只提示' },
          h('input', { type: 'checkbox', checked: !!auto.pull, disabled: !auto.fetch, onchange: e => send({ type: 'git.auto', id: g.convId, fetch: true, pull: e.target.checked, minutes: auto.minutes || 5 }) }), '自動拉取'),
        h('select', { class: 'select', 'aria-label': '自動同步間隔', disabled: !auto.fetch, onchange: e => send({ type: 'git.auto', id: g.convId, fetch: !!auto.fetch, pull: !!auto.pull, minutes: +e.target.value }) },
          [1, 5, 15, 30, 60].map(n => h('option', { value: n, selected: (auto.minutes || 5) === n }, `每 ${n} 分鐘`))));
      const top = h('div', { class: 'git-top' }, icon('branch', 18),
        h('strong', { class: 'git-branch' }, d.branch || '尚無分支'),
        d.upstream ? h('span', { class: 'git-track', 'data-tip': '追蹤 ' + d.upstream }, `↑${d.ahead || 0} ↓${d.behind || 0}`) : h('span', { class: 'git-track muted' }, '沒有追蹤的遠端分支'),
        d.remote ? h('span', { class: 'git-remote', 'data-tip': d.remote.url }, d.remote.name) : null,
        auth.state === 'required' ? h('button', { class: 'git-chip tone-risk', type: 'button', onclick: () => { g.skipLogin = false; render(); } }, '需要登入')
          : auth.state === 'offline' ? h('span', { class: 'git-track', 'data-tip': auth.detail || '' }, '無法連線遠端') : null,
        needIdentity(d) ? h('button', { class: 'git-chip tone-changed', type: 'button', onclick: () => { g.skipLogin = false; render(); } }, '未設定提交身分') : null,
        h('span', { class: 'git-grow' }),
        h('button', { class: 'btn sm secondary', disabled: busy || !d.remote, onclick: () => { g.busy = 'fetch'; send({ type: 'git.fetch', id: g.convId }); render(); } }, '擷取'),
        h('button', { class: 'btn sm secondary', disabled: busy || !d.upstream, onclick: () => { g.busy = 'pull'; send({ type: 'git.pull', id: g.convId }); render(); } }, d.behind ? `拉取 ${d.behind}` : '拉取'),
        h('button', { class: 'btn sm secondary', disabled: busy || !d.remote, onclick: () => { g.busy = 'push'; send({ type: 'git.push', id: g.convId }); render(); } }, d.ahead ? `推送 ${d.ahead}` : '推送'),
        sync,
        h('button', { class: 'icon-btn', type: 'button', 'aria-label': '重新整理', 'data-tip': '重新整理', onclick: () => send({ type: 'git.refresh', id: g.convId }) }, icon('refresh', 16)),
        h('button', { class: 'icon-btn', type: 'button', 'aria-label': '關閉版本控制', onclick: close }, icon('x', 18)));
      const message = h('textarea', { class: 'input git-message', rows: 4, 'data-keep': 'git-message', placeholder: '提交訊息：第一行是摘要',
        value: g.message, oninput: e => { g.message = e.target.value; } });
      const secrets = g.secrets.length ? h('div', { class: 'git-secrets', role: 'alert' },
        h('strong', {}, `提交前掃描：${g.secrets.length} 處疑似金鑰，已取消暫存`),
        h('ul', {}, g.secrets.map(s => h('li', {}, `${s.path} 第 ${s.line} 行 · ${s.kind}`))),
        h('div', { class: 'desk-actions' },
          h('button', { class: 'btn sm secondary', onclick: () => { g.secrets = []; render(); } }, '回去修改'),
          h('button', { class: 'btn sm danger', onclick: () => { g.secrets = []; commit(g, !!g.pending?.push, true); } }, '確認不是金鑰，仍要提交'))) : null;
      const commitBox = h('section', { class: 'git-commit', 'aria-label': '選擇要提交的檔案' },
        h('div', { class: 'git-legend' },
          h('span', { class: 'git-chip tone-changed' }, `修改 ${counts.changed}`), h('span', { class: 'git-chip tone-new' }, `新增 ${counts.new}`),
          h('span', { class: 'git-chip tone-risk' }, `不建議提交 ${counts.risk}`), h('span', { class: 'git-grow' }),
          h('button', { class: 'link-btn', onclick: () => { seedSelection(g); render(); } }, '選擇建議項目'),
          h('button', { class: 'link-btn', onclick: () => { g.sel.clear(); g.allowRisk.clear(); render(); } }, '全部取消'),
          h('button', { class: 'link-btn', onclick: () => window.CCA_DEV?.openSettings?.('git') }, `永遠不提交規則 ${(d.rules || []).length}`)),
        files.length ? h('div', { class: 'git-tiles', role: 'list' }, files.map(f => tile(g, f))) : h('div', { class: 'git-empty' }, '工作區沒有變更'),
        diffView(g),
        files.length ? h('div', { class: 'git-compose' }, message,
          h('div', { class: 'git-compose-row' },
            d.task ? h('label', { class: 'check', 'data-tip': '在提交訊息加上 CCA-Task 編號，提交紀錄會標出來自 CCA 任務的提交' },
              h('input', { type: 'checkbox', checked: g.task, onchange: e => { g.task = e.target.checked; } }), '附上任務編號') : null,
            h('button', { class: 'link-btn', onclick: () => { g.message = draft(g); render(); } }, d.task ? '依任務紀錄草擬' : '依選擇草擬'),
            h('span', { class: 'git-grow' }),
            h('span', { class: 'muted' }, `已選 ${g.sel.size} 個`),
            h('button', { class: 'btn sm secondary', disabled: busy || !g.sel.size, onclick: () => commit(g, false) }, g.busy === 'commit' ? '提交中…' : '提交'),
            h('button', { class: 'btn sm primary', disabled: busy || !g.sel.size || !d.remote, onclick: () => commit(g, true) }, g.busy === 'commit-push' ? '提交並推送中…' : '提交並推送'))) : null,
        secrets);
      const history = h('section', { class: 'git-history', 'aria-label': '提交紀錄' },
        h('div', { class: 'card-title' }, `提交紀錄 · 最近 ${rows.length} 筆`),
        rows.length ? h('div', { class: 'git-rows' }, rows.map(r => h('div', { class: 'git-row' + (r.c.task ? ' task' : ''), title: r.c.hash },
          rowSvg(r, width),
          h('span', { class: 'git-subject' }, r.c.subject),
          (r.c.refs || []).slice(0, 3).map(ref => h('span', { class: 'git-ref' + (ref.startsWith('HEAD') ? ' head' : ref.includes('/') ? ' remote' : '') }, ref.replace('HEAD -> ', '● '))),
          r.c.task ? h('span', { class: 'git-ref task', 'data-tip': 'CCA 任務 ' + r.c.task }, 'CCA') : null,
          h('span', { class: 'git-meta' }, `${r.c.hash.slice(0, 7)} · ${r.c.author} · ${new Date(r.c.ts).toLocaleString('zh-TW', { month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit' })}`))))
          : h('div', { class: 'git-empty' }, '還沒有提交'));
      const keep = pane.querySelector('.git-message');
      const focused = keep && document.activeElement === keep;
      put(pane, top, h('div', { class: 'git-body' }, commitBox, history));
      if (focused) pane.querySelector('.git-message')?.focus();
    }

    function rulesPage(body) {
      const rules = S.settings.git?.neverCommit || [];
      const input = h('input', { class: 'input', placeholder: '例如 .env、*.pem、secrets/、docs/private', 'data-keep': 'git-rule' });
      const save = list => send({ type: 'git.rules', id: S.git?.convId || conv()?.id || '', rules: list });
      const add = () => { const v = input.value.trim(); if (!v) return; save([...new Set([...rules, v])]); input.value = ''; };
      const presets = ['.env', '*.pem', '*.key', 'id_rsa*', 'node_modules/', 'build/', 'dist/', '*.log', '.vs/', '*.pfx'];
      body.append(h('h2', {}, '版本控制'),
        h('p', { class: 'hint' }, '符合規則的檔案在提交面板顯示為紅色並鎖住，不論手動或「提交並推送」都不會被提交。規則寫法同 .gitignore：只寫名稱會比對任何位置，結尾 / 代表資料夾，含 / 則從專案根目錄比對，** 可跨資料夾。規則只存在這台電腦，不會寫進專案。'),
        h('div', { class: 'git-rule-list', role: 'list' }, rules.length ? rules.map(r => h('span', { class: 'git-rule', role: 'listitem' }, icon('shield', 13), r,
          h('button', { class: 'icon-btn', type: 'button', 'aria-label': '移除規則 ' + r, onclick: () => save(rules.filter(x => x !== r)) }, icon('x', 12))))
          : h('span', { class: 'muted' }, '目前沒有規則')),
        h('div', { class: 'field' }, h('label', {}, '新增規則'), h('div', { class: 'git-rule-add' }, input,
          h('button', { class: 'btn secondary', onclick: add }, '加入'))),
        h('div', { class: 'field' }, h('label', {}, '常用規則'), h('div', { class: 'git-rule-list' },
          presets.filter(p => !rules.includes(p)).map(p => h('button', { class: 'chip', type: 'button', onclick: () => save([...rules, p]) }, '+ ' + p)))),
        h('p', { class: 'hint' }, '紅色但沒有鎖頭的檔案是 CCA 判斷「不建議提交」：疑似金鑰或憑證、建置產物與相依套件、二進位檔、暫存檔、大於 5 MB 的檔案與未解決的衝突。它們預設不選，選取時會再確認一次。提交前還會掃描暫存的差異，發現疑似金鑰就先取消暫存。'));
    }

    function message(m) {
      const g = S.git;
      if (m.type === 'git.state') {
        if (!g || g.convId !== m.id) return true;
        const first = !g.data || !g.data.ok;
        g.data = m;
        if (first || m.last?.op === 'commit' && m.last?.ok) { g.seeded = false; if (m.last?.op === 'commit' && m.last?.ok) g.message = ''; }
        if (!needIdentity(m)) g.who = null;
        if (m.last && m.last.op !== 'auto') g.busy = '';
        render(); return true;
      }
      if (m.type === 'git.busy') { if (g && g.convId === m.id) { g.busy = m.op; render(); } return true; }
      if (m.type === 'git.result') {
        if (!g || g.convId !== m.id) return true;
        g.busy = '';
        g.secrets = m.result?.secrets || [];
        render(); return true;
      }
      if (m.type === 'git.diff') {
        if (g && g.convId === m.id && g.diff?.path === m.path) { g.diff.text = m.diff; render(); }
        return true;
      }
      return false;
    }
    return { open, close, render, message, rulesPage, isOpen: id => S.git?.convId === id };
  },
};
