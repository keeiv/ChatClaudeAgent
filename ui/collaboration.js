// Commander-facing plans, independent opinions, review focus and isolated workspaces.
window.CCACollaboration = (() => {
  let api, refresh;
  const setup = hooks => { api = hooks; };
  const data = c => api.state(c.id).collaboration || c.collaboration || {};
  const busy = c => !!api.state(c.id).running;
  const button = (label, click, style = 'secondary', disabled = false) =>
    api.h('button', { class: 'btn sm ' + style, type: 'button', onclick: click, disabled }, label);
  const field = (label, input) => api.h('label', { class: 'field' }, api.h('span', {}, label), input);
  const input = (value, label, rows = 0) => api.h(rows ? 'textarea' : 'input', {
    class: 'input', value: value || '', 'aria-label': label, ...(rows ? { rows } : {}) });
  const command = (c, type, extra = {}) => api.send({ type, id: c.id, ...extra });
  const status = value => ({ draft: '待核准', approved: '已核准', running: '執行中', done: '完成', failed: '失敗',
    interrupted: '已中斷', accepted: '已驗收', awaiting_acceptance: '待驗收', summarizing: '整理共識中', queued: '等待中' }[value] || value || '尚未建立');
  const update = () => refresh?.();

  function planEditor(c) {
    const { h } = api;
    const old = data(c).plan || {};
    let revision = old.revision || 0;
    const title = input(old.title, '計畫標題');
    const rows = h('div', { class: 'plan-editor' });
    const fields = [];
    const add = step => {
      const text = input(step.text, '工作步驟', 2), acceptance = input(step.acceptance, '驗收條件', 2);
      const entry = { text, acceptance };
      const row = h('div', { class: 'card plan-edit-step' }, field('工作步驟', text), field('驗收條件', acceptance),
        button('移除步驟', () => { fields.splice(fields.indexOf(entry), 1); row.remove(); }, 'ghost'));
      fields.push(entry); rows.append(row);
    };
    (old.steps?.length ? old.steps : [{ text: '', acceptance: '' }]).forEach(add);
    const saved = h('p', { class: 'hint', role: 'status' }, '儲存後才會產生待核准版本；儲存不會執行工作。');
    let submitted;
    refresh = () => {
      const current = data(c).plan;
      if (!submitted || !current || current.revision <= revision) return;
      if (current.title === submitted.title && JSON.stringify(current.steps.map(s => [s.text, s.acceptance])) === JSON.stringify(submitted.steps.map(s => [s.text, s.acceptance]))) {
        revision = current.revision; submitted = null; saved.textContent = '已儲存版本 ' + revision + '。可關閉後檢視、留言與核准。';
      } else saved.textContent = '計畫已由其他操作更新，這份編輯內容已保留；請另行複製後重新開啟。';
    };
    api.modal({ title: '編輯工作計畫', wide: true, body: [field('計畫標題', title), rows,
      button('新增步驟', () => add({})), saved], onClose: () => { refresh = null; },
    foot: close => [button('關閉', close, 'ghost'), button('儲存草稿', () => {
      submitted = { title: title.value, steps: fields.map(e => ({ text: e.text.value, acceptance: e.acceptance.value })) };
      command(c, 'plan.save', { revision, plan: submitted });
    }, 'primary')] });
  }

  function planDialog(c) {
    const { h } = api;
    if (!data(c).plan?.steps?.length) { planEditor(c); return; }
    const body = h('div'), foot = h('div', { class: 'collab-actions' });
    let comment = '', step = '', fingerprint = '', submitted;
    const draw = () => {
      const plan = data(c).plan;
      const current = JSON.stringify([plan, busy(c)]);
      if (current === fingerprint) return;
      fingerprint = current;
      if (submitted && plan.revision > submitted.revision && plan.comments?.some(n => n.revision > submitted.revision && n.text === submitted.text && (n.step || '') === submitted.step)) {
        comment = ''; submitted = null;
      }
      const commentInput = input(comment, '計畫留言', 3);
      commentInput.oninput = () => { comment = commentInput.value; };
      const pick = h('select', { class: 'select', 'aria-label': '留言步驟' },
        h('option', { value: '' }, '整份計畫'), plan.steps.map((s, i) => h('option', { value: s.id }, '步驟 ' + (i + 1))));
      pick.value = step; pick.onchange = () => { step = pick.value; };
      api.put(body, h('div', { class: 'artifact-meta' }, h('strong', {}, plan.title),
        h('span', { class: 'tag' }, '版本 ' + plan.revision), h('span', {}, status(plan.status))),
      h('ol', { class: 'plan-steps' }, plan.steps.map(s => h('li', {}, h('strong', {}, s.text),
        h('p', { class: 'hint' }, '驗收：' + (s.acceptance || '尚未指定，核准前建議補齊')),
        h('p', { class: 'hint' }, window.CCAQuality?.stepLabel(c.id, s.evidenceId) || '人工核對'),
        h('label', { class: 'check-row' }, h('input', { type: 'checkbox', checked: s.accepted,
          disabled: busy(c) || plan.status === 'draft', onchange: e => command(c, 'plan.accept', {
            revision: plan.revision, step: s.id, accepted: e.target.checked }) }), '指揮官已驗收')))),
      h('div', { class: 'plan-comments' }, h('h3', {}, '留言與修訂紀錄'), (plan.comments || []).map(note =>
        h('article', { class: 'artifact-comment' }, h('div', { class: 'muted' }, `${note.author} · 版本 ${note.revision} · ${note.step || '整份計畫'}`),
          h('p', {}, note.text)))), field('留言對象', pick), field('留言內容', commentInput),
      button('加入留言並退回待核准', () => {
        submitted = { revision: plan.revision, step, text: comment.trim() };
        command(c, 'plan.comment', { revision: plan.revision, step, text: comment });
      }, 'secondary', busy(c)));
      api.put(foot, button('編輯計畫', () => { close(); planEditor(c); }, 'ghost', busy(c)),
        button('核准此版本', () => command(c, 'plan.approve', { revision: plan.revision }), 'secondary', busy(c) || plan.status !== 'draft'),
        button('執行已核准計畫', () => command(c, 'plan.execute', { revision: plan.revision }), 'primary', busy(c) || plan.status !== 'approved'));
    };
    let close;
    refresh = draw;
    close = api.modal({ title: '計畫與驗收', desc: '核准與執行是兩個動作。留言或編輯後，必須重新檢查並核准。',
      wide: true, body, foot, onClose: () => { refresh = null; } });
    draw();
  }

  function opinionDialog(c) {
    const { h } = api;
    const body = h('div'), footer = h('div', { class: 'collab-actions' });
    let question = data(c).opinion?.question || '', selected = ['claude', 'codex'], fingerprint = '';
    const draw = () => {
      const op = data(c).opinion || {};
      const current = JSON.stringify(op);
      if (fingerprint === current) return;
      fingerprint = current;
      const running = ['running', 'summarizing'].includes(op.status);
      const text = input(question, '意見模式問題', 4); text.oninput = () => { question = text.value; };
      const members = ['claude', 'codex', 'local1', 'local2'];
      api.put(body, field('共同問題', text), op.question ? h('p', { class: 'hint' }, '本次分析問題：' + op.question) : null,
      h('div', { class: 'opinion-members' }, members.map(agent =>
        h('label', { class: 'check-row' }, h('input', { type: 'checkbox', checked: selected.includes(agent), disabled: running,
          onchange: e => { selected = members.filter(a => a === agent ? e.target.checked : selected.includes(a)); } }),
        api.avatar(agent, 20), api.name(agent)))), h('p', { class: 'hint' },
        '2 至 4 位模型會同時獨立分析，維持唯讀；之後再用其中一位整理共識。會使用所選成員的訂閱或本機資源，整理共識另需一次回覆。'),
      op.status ? h('p', { class: 'artifact-meta', role: 'status' }, status(op.status)) : null,
      h('div', { class: 'opinion-results' }, Object.entries(op.results || {}).map(([agent, result]) => h('article', { class: 'card' },
        h('div', { class: 'card-title' }, agent === 'summary' ? '共識整理' : api.name(agent), h('span', { class: 'tag' }, status(result.status))),
        result.error ? h('p', { class: 'warn-text' }, result.error) : null,
        h('div', { class: 'artifact-text', html: api.markdown(result.text || '等待回覆') })))),
      op.summary ? h('section', { class: 'opinion-summary' }, h('h3', {}, '共識'),
        h('ul', {}, (Array.isArray(op.summary.consensus) ? op.summary.consensus : []).map(t => h('li', {}, t))), h('h3', {}, '分歧與待驗證項目'),
        h('ul', {}, (Array.isArray(op.summary.disagreements) ? op.summary.disagreements : []).map(t => h('li', {}, t))),
        op.summary.raw ? h('pre', { class: 'task-record' }, op.summary.raw) : null) : null,
      op.formatNotice ? h('p', { class: 'warn-text' }, op.formatNotice) : null);
      api.put(footer, button('停止蒐集', () => command(c, 'opinion.stop'), 'ghost', !running),
        button('將提案存成待核准計畫', () => { command(c, 'opinion.plan'); }, 'secondary', running || !op.proposedPlan),
        button('開始獨立蒐集', () => command(c, 'opinion.start', { question, agents: selected }), 'primary', running || busy(c)));
    };
    refresh = draw;
    api.modal({ title: '多模型意見', wide: true, body, foot: footer, onClose: () => { refresh = null; } });
    draw();
  }

  function workspaceDialog(c) {
    const { h } = api;
    const body = h('div'), footer = h('div', { class: 'collab-actions' });
    let fingerprint = '';
    const draw = () => {
      const w = data(c).workspace || {}, running = busy(c);
      const next = JSON.stringify([w, running]); if (next === fingerprint) return; fingerprint = next;
      const check = w.check;
      api.put(body, h('p', {}, '每個協作群組可使用自己的 worktree 與分支，同一項任務的代理在該工作區交接，其他任務可平行執行。'),
        h('p', { class: 'hint' }, '建立時從已提交的 HEAD 開始。來源有未提交變更時會阻擋；合併也只接受兩邊都已提交的內容，不會自動暫存檔案。'),
        h('dl', { class: 'workspace-facts' }, ['origin', 'path', 'branch', 'targetBranch'].flatMap((key, i) =>
          w[key] ? [h('dt', {}, ['來源倉庫', '隔離工作區', '任務分支', '合併目標'][i]), h('dd', { class: 'mono' }, w[key])] : [])),
        check ? h('div', { class: 'card' }, h('div', { class: 'card-title' }, check.merged ? '已合併' : check.clean ? '可無衝突合併' : '存在衝突'),
          h('pre', { class: 'task-record' }, check.summary || '沒有新的已提交差異'),
          check.conflicts ? h('pre', { class: 'task-record warn-text' }, check.conflicts) : null) : null);
      api.put(footer, w.ok ? button('檢查能否合併', () => command(c, 'worktree.check'), 'secondary', running) :
        button('建立隔離 worktree', () => command(c, 'worktree.create'), 'primary', running),
      w.ok ? button('合併已提交變更', () => api.modal({ title: '合併任務變更',
        desc: '目標：' + w.origin + ' · ' + w.targetBranch, body: h('pre', { class: 'task-record' }, check.summary || '沒有變更'),
        foot: close => [button('取消', close, 'ghost'), button('確認合併', () => {
          close(); command(c, 'worktree.merge', { target: check.target, source: check.source });
        }, 'primary')] }), 'primary', running || !check?.clean || check?.merged) : null);
    };
    refresh = draw;
    api.modal({ title: '隔離工作區', wide: true, body, foot: footer, onClose: () => { refresh = null; } }); draw();
  }

  function focusDialog(c) {
    const { h } = api;
    const profiles = api.settings().reviewProfiles || [];
    const fields = ['claude', 'codex', 'local1', 'local2'].map(agent => {
      const select = h('select', { class: 'select', 'aria-label': api.name(agent) + ' 審查關注點' },
        h('option', { value: '' }, '一般審查'), profiles.map(p => h('option', { value: p.id }, p.name)));
      select.value = data(c).focus?.[agent] || '';
      select.onchange = () => command(c, 'focus.assign', { agent, focus: select.value });
      return field(api.name(agent), select);
    });
    let selected = profiles[0]?.id || '';
    const name = input('', '關注點名稱'), markdown = input('', '關注點 Markdown', 7);
    const picker = h('select', { class: 'select', 'aria-label': '編輯關注點' }, profiles.map(p => h('option', { value: p.id }, p.name)),
      h('option', { value: '' }, '新增自訂關注點'));
    const pick = () => {
      selected = picker.value;
      const profile = (api.settings().reviewProfiles || []).find(p => p.id === selected);
      name.value = profile?.name || ''; markdown.value = profile?.text || '';
    };
    picker.onchange = pick; picker.value = selected; pick();
    api.modal({ title: '審查關注點', wide: true, body: [h('p', { class: 'hint' },
      '一般雙方互審與本地審查使用下列設定；多代理流程可替每個審查位置指定不同關注點。關注點不會提高執行權限。'), ...fields,
      h('h3', {}, '編輯 Markdown 規則'), picker, field('名稱', name), field('規則', markdown)],
    foot: close => [button('關閉', close, 'ghost'), button('儲存關注點', () => {
      const id = selected || 'focus-' + Date.now().toString(36);
      selected = id;
      const next = (api.settings().reviewProfiles || []).filter(p => p.id !== id).concat({ id, name: name.value, text: markdown.value });
      api.send({ type: 'focus.profiles', profiles: next });
    }, 'primary')] });
  }

  function controls(c) {
    const { h } = api, d = data(c);
    return h('section', { class: 'panel-section collab-card' }, h('div', { class: 'card' }, h('div', { class: 'card-title' }, '任務控制'),
      h('div', { class: 'collab-actions' }, button('計畫與驗收', () => planDialog(c)), button('多模型意見', () => opinionDialog(c)),
        button('審查關注點', () => focusDialog(c)), button('隔離工作區', () => workspaceDialog(c))),
      d.plan ? h('p', { class: 'hint' }, status(d.plan.status) + ' · ' + d.plan.title) : null,
      d.workspace?.ok ? h('p', { class: 'hint mono' }, d.workspace.branch) : null));
  }

  function inbox(container, state) {
    const { h } = api;
    const kind = { approval: '等待核准', question: '等待回覆', paused: '協作暫停', interrupted: '任務中斷', done: '工作完成', plan: '計畫待處理', error: '需要檢查' };
    api.put(container, h('header', { class: 'chat-head' }, api.icon('inbox', 20), h('strong', {}, '指揮官收件匣'),
      h('span', { class: 'muted' }, (state.total || 0) + ' 項'), button('重新整理', () => api.send({ type: 'inbox.get' }))),
    h('div', { class: 'page inbox-page' }, h('p', { class: 'hint' }, '集中顯示所有對話的核准、提問、暫停、計畫與完成通知。待處理事項需在原對話解決，標示已讀不會略過核准。'),
      (state.items || []).length ? state.items.map(item => h('article', { class: 'card inbox-item ' + (item.required ? 'needs-action' : '') },
        h('div', { class: 'artifact-meta' }, h('strong', {}, item.conversation), h('span', { class: 'tag' }, kind[item.kind] || item.kind),
          item.agent ? api.avatar(item.agent, 18) : null, h('time', {}, new Date(item.ts).toLocaleString('zh-TW'))), h('p', {}, item.text),
        h('div', { class: 'collab-actions' }, button('開啟對話', () => {
          api.openConv(item.convId); if (item.kind === 'plan') planDialog(api.conv(item.convId));
          else if (item.messageId) api.jump(item.messageId);
        }), item.required ? null : button('標示此對話已讀', () => api.send({ type: 'inbox.read', id: item.convId }), 'ghost')))) : h('div', { class: 'empty' }, '目前沒有待處理事項或未讀通知。'),
      state.truncated ? h('p', { class: 'hint' }, '顯示前 300 項；待處理事項優先。') : null));
  }
  return { setup, update, controls, inbox, planDialog, opinionDialog, workspaceDialog, focusDialog };
})();
