// Real Edge checks for the Cursor direct message, its Grok-first model picker, the group member switch and the
// pipeline node: the page only offers ids the host reported and never sends a send while the model is unpicked.
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
      window.cursorRequests = [];
      const receive = window.MockBackend.receive;
      window.MockBackend.receive = message => { window.cursorRequests.push(structuredClone(message)); receive(message); };
    ` }));
    await page.goto((process.env.CCA_UI_URL || 'http://127.0.0.1:5220/') + '#mock,cursor-unpicked,cursor');
    await page.waitForFunction(() => window.CCA_DEV);
    fs.mkdirSync('build/cursor', { recursive: true });

    // Direct message without a model: the composer explains, and nothing reaches the host.
    await page.evaluate(() => window.CCA_DEV.openConv('dm-cursor'));
    const banner = page.locator('.banner').filter({ hasText: 'Cursor 模型' });
    await banner.waitFor();
    const composer = page.locator('textarea').first();
    assert.ok(await composer.isDisabled() || /模型/.test(await composer.getAttribute('placeholder') || ''),
      'the composer says a model is needed');
    await banner.getByRole('button', { name: '選擇模型', exact: true }).click();
    const picker = page.locator('.popover').filter({ has: page.locator('.cursor-models') });
    await picker.waitFor();
    const titles = await picker.locator('.cursor-models .pop-title').allInnerTexts();
    assert.equal(titles[0], 'Grok', 'Grok models are listed first');
    const ids = await picker.locator('.cursor-models .pd.mono').allInnerTexts();
    assert.deepEqual(ids.slice(0, 3), ['grok-4.7-high', 'grok-4.7-xhigh-fast', 'cursor-grok-4.6-medium']);
    await page.screenshot({ path: 'build/cursor/model-picker.png' });
    const panel = await page.locator('.agent-head').first().locator('xpath=..').innerText();
    assert.ok(!/\bnull\b/.test(panel), 'no stray null text in the status panel');
    assert.match(panel, /Cursor CLI 2026/);
    assert.ok(!/Cursor · Cursor CLI/.test(panel), 'the vendor is not repeated');
    await picker.getByRole('searchbox', { name: '篩選 Cursor 模型' }).fill('composer');
    assert.deepEqual(await picker.locator('.cursor-models .pd.mono').allInnerTexts(), ['composer-2'], 'the filter narrows the list');
    await picker.getByRole('searchbox', { name: '篩選 Cursor 模型' }).fill('no-such-model');
    await picker.getByText('沒有符合的模型。').waitFor();
    await picker.getByRole('searchbox', { name: '篩選 Cursor 模型' }).fill('4.7 high');
    await picker.getByRole('button', { name: /Grok 4\.7 High/ }).first().click();
    const pick = await page.evaluate(() => window.cursorRequests.findLast(m => m.type === 'conv.settings'));
    assert.deepEqual(pick.settings, { cursorModel: 'grok-4.7-high' }, 'the pick is sent as the reported id');
    await page.locator('.chat-head').getByText('Grok 4.7 High').first().waitFor();
    await banner.waitFor({ state: 'detached' });
    await page.screenshot({ path: 'build/cursor/direct-message.png' });

    // Group: the member switch and its own model row.
    await page.evaluate(() => window.CCA_DEV.openConv('g1'));
    await page.getByRole('button', { name: '頻道設定', exact: true }).click();
    const member = page.locator('.ops-row').filter({ hasText: 'Cursor 成員' });
    await member.waitFor();
    assert.match(await member.innerText(), /已加入：第(三|四)位審查者/);
    assert.match(await page.locator('.ops-row').filter({ hasText: 'Cursor 模型' }).innerText(), /Grok 4\.7 High/);
    await member.locator('button').click();
    const leave = await page.evaluate(() => window.cursorRequests.findLast(m => m.type === 'conv.settings'));
    assert.deepEqual(leave.settings, { cursorMember: false });

    // Pipeline: a Cursor node offers only catalogue models, Grok first, with no "CLI default" entry.
    await page.getByRole('button', { name: '頻道設定', exact: true }).click();
    await page.locator('.ops-row').filter({ hasText: '多代理流程' }).locator('button').click();
    await page.getByRole('button', { name: '新增 Cursor 審查者', exact: true }).click();
    const node = page.locator('.pipeline-node').last();
    assert.equal(await node.getByRole('combobox').first().inputValue(), 'cursor');
    const model = node.getByRole('combobox', { name: '模型', exact: true });
    const options = await model.locator('option').evaluateAll(list => list.map(o => o.value));
    assert.deepEqual(options.slice(0, 3), ['grok-4.7-high', 'grok-4.7-xhigh-fast', 'cursor-grok-4.6-medium']);
    assert.ok(!options.includes(''), 'no CLI default model is offered');
    assert.equal(await model.inputValue(), 'grok-4.7-high');
    await page.screenshot({ path: 'build/cursor/pipeline.png' });
    await page.getByRole('button', { name: '儲存流程', exact: true }).click();
    const config = await page.evaluate(() => window.cursorRequests.findLast(m => m.type === 'pipeline.set').pipeline);
    const saved = config.nodes.find(n => n.backend === 'cursor');
    assert.equal(saved.model, 'grok-4.7-high');
    assert.ok(!saved.effort, 'Cursor nodes carry no separate effort');

    // Account page and its install command.
    await page.locator('button[data-tip="訂閱與用量"]').first().click();
    const account = page.locator('.acct-card').filter({ hasText: 'Cursor' });
    await account.waitFor();
    assert.match(await account.innerText(), /Cursor Pro/);
    assert.match(await account.innerText(), /沒有提供剩餘額度/);
    assert.deepEqual(errors, []);
    console.log('Cursor direct message, picker, group, pipeline and account browser checks passed');
  } finally {
    await browser.close();
  }
})().catch(error => {
  console.error(error);
  process.exit(1);
});
