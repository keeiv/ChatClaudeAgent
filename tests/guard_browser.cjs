// Reviewer test guard in a real Edge page: the channel setting, its command dialog and the "regression" message.
// Needs Playwright, Edge and the UI server (node tools/serve_ui.js with PORT=5220, or CCA_UI_URL).
const assert = require('node:assert/strict');
const fs = require('node:fs');
const { chromium } = require('playwright');

(async () => {
  const checks = [], errors = [];
  const check = (value, label) => { assert.ok(value, label); checks.push(label); };
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const page = await browser.newPage({ viewport: { width: 1380, height: 950 } });
  page.setDefaultTimeout(10000);
  page.on('pageerror', error => errors.push(error.message));
  try {
    await page.route('**/mock.js', route => route.fulfill({ contentType: 'text/javascript', body: fs.readFileSync('ui/mock.js', 'utf8') + `
      window.__settings = [];
      const receive = window.MockBackend.receive;
      window.MockBackend.receive = message => { if (message.type === 'conv.settings') window.__settings.push(message); receive(message); };` }));
    await page.goto((process.env.CCA_UI_URL || 'http://127.0.0.1:5220/') + '#mock,guard-demo');
    await page.waitForFunction(() => window.CCA_DEV);
    await page.waitForTimeout(1200);
    await page.evaluate(() => window.CCA_DEV.openConv('g1'));
    await page.getByRole('button', { name: '頻道設定', exact: true }).click();
    const row = page.locator('.ops-row').filter({ hasText: '測試守門' });
    check(await row.isVisible(), 'the channel settings have a test guard row');
    const toggle = row.locator('button.toggle');
    check(await toggle.isDisabled(), 'the guard cannot be turned on before a command exists');

    await row.getByRole('button', { name: '設定測試指令' }).click();
    await page.getByText('測試守門的測試指令').waitFor();
    check(await page.getByText('不證明審查者的修改正確', { exact: false }).isVisible(), 'the dialog states what the guard does not prove');
    await page.getByRole('textbox', { name: '測試指令' }).fill('npm test');
    await page.getByRole('button', { name: '儲存', exact: true }).click();
    const saved = await page.evaluate(() => window.__settings.at(-1));
    check(saved.settings.testCommand === 'npm test', 'the dialog sends the command as a conversation setting');

    await page.getByRole('button', { name: '頻道設定', exact: true }).click();
    const again = page.locator('.ops-row').filter({ hasText: '測試守門' });
    check(await again.locator('button.toggle').isEnabled(), 'a command unlocks the toggle');
    await again.locator('button.toggle').click();
    check((await page.evaluate(() => window.__settings.at(-1))).settings.regressionGuard === true, 'the toggle sends regressionGuard');
    await page.keyboard.press('Escape');

    await page.locator('.composer textarea').fill('修好購物車');
    await page.keyboard.press('Enter');
    const message = page.locator('.sys').filter({ hasText: '已還原' });
    await message.first().waitFor({ timeout: 20000 });
    check(await message.getByText('已還原', { exact: true }).isVisible(), 'a reverted reviewer edit is announced with its tag');
    await message.locator('summary').click();
    check(await message.locator('pre').innerText().then(t => t.includes('expected 1999')), 'the failing test output is readable');
    await page.waitForTimeout(400);
    await page.screenshot({ path: 'build/guard-message.png' });
    check(await page.locator('body').evaluate(el => el.scrollWidth <= window.innerWidth), 'no horizontal overflow');
    check(errors.length === 0, 'no browser script errors: ' + errors.join('; '));
    console.log(`${checks.length} test guard browser checks passed`);
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
