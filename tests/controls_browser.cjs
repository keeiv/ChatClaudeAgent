// Real Edge checks for the workflow controls: protected paths, task limits, notifications, @ mentions, the Ctrl+K
// palette, the project rules editor, pipeline templates, the held group request and schedules. Mock host only.
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
      window.sent = [];
      const connect = window.MockBackend.connect;
      window.MockBackend.connect = fn => { window.deliver = fn; return connect(fn); };
      const receive = window.MockBackend.receive;
      window.MockBackend.receive = message => { window.sent.push(structuredClone(message)); receive(message); };
    ` }));
    await page.goto((process.env.CCA_UI_URL || 'http://127.0.0.1:5220/') + '#mock');
    await page.waitForFunction(() => window.CCA_DEV);
    fs.mkdirSync('build/controls', { recursive: true });
    const last = (type) => page.evaluate(t => window.sent.filter(m => m.type === t).pop(), type);
    const settingsRow = (label) => page.locator('.ops-row').filter({ hasText: label });

    // Protected paths and task limits from the channel settings panel.
    await page.evaluate(() => window.CCA_DEV.openConv('g1'));
    await page.getByRole('button', { name: '頻道設定', exact: true }).click();
    await settingsRow('受保護路徑').locator('button').click();
    await page.getByRole('textbox', { name: '受保護路徑' }).fill('migrations/\n*.lock\n');
    await page.getByRole('button', { name: '儲存', exact: true }).click();
    assert.deepEqual((await last('conv.settings')).settings, { protectedPaths: ['migrations/', '*.lock'] });
    await page.getByRole('button', { name: '頻道設定', exact: true }).click();
    assert.match(await settingsRow('受保護路徑').innerText(), /2 條規則/);
    await settingsRow('任務上限').locator('button').click();
    await page.getByRole('spinbutton', { name: '萬 tokens' }).fill('50');
    await page.getByRole('spinbutton', { name: '分鐘' }).fill('30');
    await page.getByRole('button', { name: '儲存', exact: true }).click();
    assert.deepEqual((await last('conv.settings')).settings, { limitTokens: 500000, limitMinutes: 30 });

    // A held group request shows the files and starts on request.
    await page.evaluate(() => {
      const all = ['dm-claude', 'dm-codex', 'dm-gemini', 'dm-cursor', 'g1'].map(id => window.CCA_DEV.conv(id)).filter(Boolean);
      window.deliver({ type: 'convs', items: all.map(c => c.id === 'g1' ? { ...c, startPending: true } : c) });
      window.deliver({ type: 'msg', convId: 'g1', msg: { id: 'dirty1', author: 'system', kind: 'dirty', ts: Date.now(),
        files: ['notes.txt', 'src/wip.ts'], count: 2, request: '加上折扣碼', text: '專案裡有 2 個還沒提交的修改。' } });
    });
    const held = page.locator('.sys').filter({ hasText: '專案裡有 2 個還沒提交的修改' });
    await held.waitFor();
    assert.match(await held.innerText(), /notes\.txt/);
    await held.getByRole('button', { name: '照常開始', exact: true }).click();
    assert.equal((await last('group.startAnyway')).id, 'g1');

    // @ mentions: the picker lists project files and inserts the chosen path.
    await page.evaluate(() => window.CCA_DEV.openConv('dm-claude'));
    const composer = page.locator('.composer textarea');
    await composer.fill('');
    await composer.type('看 @disc');
    const picker = page.locator('.mention-pop');
    await picker.getByText('src/discount.ts').waitFor();
    await page.keyboard.press('Enter');
    assert.equal(await composer.inputValue(), '看 @src/discount.ts ');
    assert.ok(await picker.isHidden(), 'the picker closes after a choice');
    await page.screenshot({ path: 'build/controls/mention.png' });

    // Ctrl+K: actions come first and run with Enter.
    await composer.fill('');
    await page.keyboard.press('Control+k');
    const palette = page.getByRole('textbox', { name: '指令面板' });
    await palette.waitFor();
    assert.match(await page.locator('.palette-list').innerText(), /權限改為/);
    await palette.fill('專案規則');
    await page.screenshot({ path: 'build/controls/palette.png' });
    await page.keyboard.press('Enter');

    // The rules editor opened from the palette: suggestions append, save carries the opened version.
    const rules = page.getByRole('textbox', { name: '規則內容' });
    await rules.waitFor();
    assert.equal(await rules.inputValue(), '# 規則\n- 用 TypeScript\n');
    await page.locator('.rule-suggestion').first().getByRole('button', { name: '加入' }).click();
    assert.match(await rules.inputValue(), /- 給 APPROVE 之前要先讀檔/);
    await page.getByRole('button', { name: '保存這個檔案', exact: true }).click();
    const save = await last('rules.save');
    assert.equal(save.name, 'AGENTS.md');
    assert.equal(save.base, '# 規則\n- 用 TypeScript\n', 'the save names the version that was opened');

    // Pipeline templates load into the editor without being applied.
    await page.evaluate(() => window.CCA_DEV.openConv('g1'));
    await page.getByRole('button', { name: '頻道設定', exact: true }).click();
    await settingsRow('多代理流程').locator('button').click();
    const template = page.getByRole('combobox', { name: '專案範本' });
    await template.locator('option', { hasText: '修 bug' }).waitFor({ state: 'attached' });
    const before = (await page.evaluate(() => window.sent.filter(m => m.type === 'pipeline.set').length));
    await template.selectOption({ label: '修 bug：重現、修正、驗證' });
    assert.equal(await page.locator('.pipeline-node').count(), 2);
    assert.equal(await page.evaluate(() => window.sent.filter(m => m.type === 'pipeline.set').length), before,
      'loading a template does not apply it');
    await page.keyboard.press('Escape');

    // Settings: notifications and schedules.
    await page.evaluate(() => window.CCA_DEV.openSettings('notify'));
    await page.getByLabel('開啟 Windows 通知').uncheck();
    assert.equal((await last('notifications.set')).enabled, false);
    await page.evaluate(() => window.CCA_DEV.openSettings('schedules'));
    await page.getByRole('textbox', { name: '需求' }).fill('每晚跑測試並修掉失敗的');
    await page.getByRole('button', { name: '新增', exact: true }).click();
    const schedule = (await last('schedule.save')).schedule;
    assert.equal(schedule.time, '02:00');
    assert.deepEqual(schedule.days, [1, 2, 3, 4, 5]);
    await page.locator('.schedule-row').first().waitFor();
    await page.screenshot({ path: 'build/controls/schedules.png' });
    assert.deepEqual(errors, []);
    console.log('workflow control browser checks passed');
  } finally {
    await browser.close();
  }
})().catch(error => {
  console.error(error);
  process.exit(1);
});
