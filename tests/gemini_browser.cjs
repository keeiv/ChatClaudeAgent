// Real Edge checks for Gemini account metadata and a second editing reviewer in the workflow editor.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const { chromium } = require('playwright');

(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  try {
    await page.route('**/mock.js', route => route.fulfill({ contentType: 'text/javascript', body: fs.readFileSync('ui/mock.js', 'utf8') + `
      window.geminiRequests = [];
      const connect = window.MockBackend.connect;
      window.MockBackend.connect = fn => {
        window.geminiDeliver = fn;
        return connect(message => {
          if (message.type === 'boot') window.geminiAuth = message.auth;
          if (message.type === 'auth') window.geminiAuth = message;
          if (message.type === 'settings' || message.type === 'boot') window.geminiSettings = message.settings;
          if (message.type === 'convs') window.geminiConvs = message.items;
          fn(message);
        });
      };
      const receive = window.MockBackend.receive;
      window.MockBackend.receive = message => { window.geminiRequests.push(structuredClone(message)); receive(message); };
    ` }));
    await page.goto((process.env.CCA_UI_URL || 'http://127.0.0.1:5220/') + '#mock');
    await page.waitForFunction(() => window.CCA_DEV);
    await page.locator('button[data-tip="訂閱與用量"]').first().click();
    const account = page.locator('.acct-card').filter({ hasText: 'Gemini' });
    await account.waitFor();
    assert.match(await account.innerText(), /Google AI Pro/);
    assert.match(await account.innerText(), /剩餘 72%/);
    fs.mkdirSync('build/gemini', { recursive: true });
    await page.screenshot({ path: 'build/gemini/account.png' });
    await account.getByRole('button', { name: '重新檢查', exact: true }).click();
    assert.ok(await page.evaluate(() => window.geminiRequests.some(m => m.type === 'auth.check' && m.agent === 'gemini')));
    await page.waitForTimeout(150);
    await page.evaluate(() => window.geminiDeliver({ ...window.geminiAuth, type: 'auth',
      gemini: { state: 'serviceError', version: 'fixture', message: 'Google 登入已驗證，但服務檢查暫時失敗' } }));
    await account.getByText('已登入，服務檢查未通過', { exact: true }).first().waitFor();
    assert.match(await account.innerText(), /已登入，服務檢查未通過/);
    assert.match(await account.innerText(), /Google 登入已驗證/);
    await page.evaluate(() => window.geminiDeliver({ ...window.geminiAuth, type: 'auth' }));
    await page.evaluate(() => window.CCA_DEV.openConv('g1'));
    await page.getByRole('button', { name: '頻道設定', exact: true }).click();
    await page.locator('.ops-row').filter({ hasText: '多代理流程' }).locator('button').click();
    await page.getByRole('button', { name: '新增 Gemini 審查者', exact: true }).click();
    assert.equal(await page.locator('.pipeline-node').count(), 3);
    const node = page.locator('.pipeline-node').last();
    assert.equal(await node.getByRole('combobox').first().inputValue(), 'gemini');
    const model = node.getByRole('combobox', { name: '模型', exact: true });
    assert.equal(await model.locator('option').count(), 1);
    assert.equal(await model.inputValue(), '');
    await page.screenshot({ path: 'build/gemini/workflow-editor.png' });
    await page.getByRole('button', { name: '儲存流程', exact: true }).click();
    const config = await page.evaluate(() => window.geminiRequests.findLast(m => m.type === 'pipeline.set').pipeline);
    assert.equal(config.routing, 'full');
    assert.equal(config.nodes[2].backend, 'gemini');
    assert.equal(config.nodes[2].role, 'review');
    await page.keyboard.press('Escape');
    await page.evaluate(() => window.CCA_DEV.openSettings('gemini'));
    await page.getByRole('heading', { name: 'Gemini 的個人資料', exact: true }).waitFor();
    await page.locator('[data-keep="name-gemini"]').fill('Gemini examiner');
    await page.waitForFunction(() => window.geminiSettings.profiles.gemini.name === 'Gemini examiner');
    await page.getByRole('button', { name: '更換頭像', exact: true }).click();
    await page.waitForFunction(() => window.geminiRequests.some(m => m.type === 'profile.image' && m.who === 'gemini'));
    await page.keyboard.press('Escape');
    await page.evaluate(() => window.CCA_DEV.openSettings('gemini'));
    await page.getByRole('button', { name: '建立自訂性格', exact: true }).click();
    const editor = page.locator('.modal').last();
    await editor.locator('.field').filter({ has: page.locator('label', { hasText: /^名稱$/ }) }).locator('input').fill('Methodical reviewer');
    assert.equal(await editor.locator('select').inputValue(), 'gemini');
    assert.equal(await editor.locator('option[value="gemini"]').count(), 1);
    await editor.locator('textarea').fill('先核對證據，再提出可重現的問題。');
    await editor.getByRole('button', { name: '儲存', exact: true }).click();
    await page.waitForFunction(() => window.geminiSettings.persona.gemini !== 'none');
    assert.match(await page.locator('.settings-content').innerText(), /Methodical reviewer/);
    await page.evaluate(() => window.CCA_DEV.openSettings('personas'));
    assert.match(await page.locator('.persona-card').filter({ hasText: 'Methodical reviewer' }).innerText(), /Gemini examiner 使用中/);
    await page.keyboard.press('Escape');
    await page.evaluate(() => window.CCA_DEV.openSettings('routing'));
    assert.match(await page.locator('.settings-content').innerText(), /Gemini CLI 預設模型/);
    await page.keyboard.press('Escape');
    await page.evaluate(() => window.MockBackend.receive({ type: 'conv.create', name: 'Gemini workflow fixture' }));
    await page.waitForFunction(() => window.geminiConvs?.some(c => c.name === 'Gemini workflow fixture'));
    const groupId = await page.evaluate(() => window.geminiConvs.find(c => c.name === 'Gemini workflow fixture').id);
    const ordered = Array.from({ length: 8 }, (_, i) => ({ id: 'stage-' + i, name: 'Stage ' + (i + 1),
      backend: i === 0 ? 'claude' : i === 1 ? 'codex' : 'gemini', role: i ? 'review' : 'write' }));
    await page.evaluate(({ groupId, nodes }) => {
      window.MockBackend.receive({ type: 'pipeline.set', id: groupId, pipeline: { nodes, routing: 'full' } });
      window.CCA_DEV.openConv(groupId);
    }, { groupId, nodes: ordered });
    await page.getByRole('heading', { name: '歡迎來到 Gemini workflow fixture', exact: true }).waitFor();
    assert.match(await page.locator('.welcome').innerText(), /Stage 3 · Gemini examiner 審查/);
    const member = page.locator('.member-link[data-node-id="stage-2"]');
    await member.click();
    await page.locator('.user-popout').getByRole('button', { name: '編輯個人資料', exact: true }).click();
    await page.locator('[data-keep="name-gemini"]').waitFor();
    await page.keyboard.press('Escape');
    await page.evaluate(() => window.CCA_DEV.openConv(window.geminiConvs.find(c => c.name === 'Gemini workflow fixture').id));
    await page.locator('.wf-control').filter({ hasText: '展開' }).click();
    const graph = page.locator('.wf-graph');
    await graph.waitFor({ state: 'visible' });
    assert.equal(await graph.locator('.wf-node').count(), 8);
    assert.equal(await graph.locator('[data-agent="gemini"]').count(), 6);
    const order = await graph.locator('.wf-node').evaluateAll(nodes => nodes.map(n => n.dataset.nodeId));
    assert.deepEqual(order, ordered.map(n => 'pipeline:' + n.id));
    const rects = await graph.locator('.wf-node').evaluateAll(nodes => nodes.map(n => {
      const r = n.getBoundingClientRect(); return { left: r.left, right: r.right, top: r.top, bottom: r.bottom };
    }));
    for (let i = 0; i < rects.length; i++) for (let j = i + 1; j < rects.length; j++) {
      const a = rects[i], b = rects[j];
      assert.ok(a.right <= b.left || b.right <= a.left || a.bottom <= b.top || b.bottom <= a.top, 'workflow cards overlap');
    }
    fs.mkdirSync('build/gemini-integration', { recursive: true });
    await page.screenshot({ path: 'build/gemini-integration/workflow.png' });
    await page.evaluate(() => window.CCA_DEV.openSettings('gemini'));
    await page.locator('[data-keep="name-gemini"]').fill('Gemini renamed');
    await page.waitForFunction(() => window.geminiSettings.profiles.gemini.name === 'Gemini renamed');
    await page.keyboard.press('Escape');
    assert.match(await page.locator('.wf-node[data-node-id="pipeline:stage-2"]').innerText(), /Gemini renamed/);
    await page.evaluate(groupId => window.geminiDeliver({ type: 'history', id: groupId,
      messages: [{ id: 'gemini-profile-message', author: 'gemini', agent_label: 'Stage 3', phase: 'review',
        ts: Date.now(), status: 'completed', blocks: [{ type: 'text', text: '已核對檔案與證據。' }] }] }), groupId);
    assert.match(await page.locator('[data-id="gemini-profile-message"] .msg-author').textContent(), /Stage 3 · Gemini renamed/);
    await page.setViewportSize({ width: 1000, height: 800 });
    assert.match(await page.locator('.wf-compact').textContent(), /Gemini renamed/);
    await page.screenshot({ path: 'build/gemini-integration/narrow.png' });
    assert.deepEqual(errors, []);
    console.log('Gemini account, profile, persona and eight-stage workflow browser checks passed');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
