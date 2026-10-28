// CCA plugins and edition choices in the update page, in a real Edge page.
// Needs Playwright, Edge and the UI server (PORT=5220 node tools/serve_ui.js, or CCA_UI_URL).
const assert = require('node:assert/strict');
const fs = require('node:fs');
const { chromium } = require('playwright');

(async () => {
  const checks = [], errors = [];
  const check = (value, label) => { assert.ok(value, label); checks.push(label); };
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const base = process.env.CCA_UI_URL || 'http://127.0.0.1:5220/';
  async function open(flags) {
    const page = await browser.newPage({ viewport: { width: 1380, height: 950 } });
    page.setDefaultTimeout(10000);
    page.on('pageerror', error => errors.push(error.message));
    await page.route('**/mock.js', route => route.fulfill({ contentType: 'text/javascript', body: fs.readFileSync('ui/mock.js', 'utf8') + `
      window.__sent = [];
      const receive = window.MockBackend.receive;
      window.MockBackend.receive = message => { window.__sent.push(message); receive(message); };` }));
    await page.goto(base + '#mock,' + flags);
    await page.waitForFunction(() => window.CCA_DEV);
    await page.waitForTimeout(1200);
    return page;
  }
  const sent = (page, type) => page.evaluate(type => window.__sent.filter(m => m.type === type).at(-1), type);
  try {
    // ---- plugins in the ordinary edition
    let page = await open('ccaplugins');
    await page.evaluate(() => window.CCA_DEV.openSettings('extensions'));
    const card = page.locator('[data-plugin="night-shift"]');
    check(await card.getByText('未啟用').isVisible(), 'a found plugin starts disabled');
    await card.getByRole('button', { name: '檢視並啟用' }).click();
    await page.getByText('檢視插件：夜班').waitFor();
    check(await page.getByText('每一項修改都要附上重現步驟與測試結果。').isVisible(), 'mode instructions are shown before enabling');
    check(await page.getByText('請以安全審查的角度檢查：{{input}}').isVisible(), 'command templates are shown before enabling');
    const enable = page.getByRole('button', { name: '啟用插件', exact: true });
    check(await enable.isDisabled(), 'enabling needs the review checkbox');
    await page.getByRole('checkbox', { name: '我已檢視以上內容，啟用這個插件' }).check();
    await enable.click();
    const enabled = await sent(page, 'extensions.enable');
    check(enabled.id === 'night-shift' && /^[0-9a-f]{64}$/.test(enabled.hash), 'the approval carries the reviewed file hash');
    await page.locator('[data-plugin="night-shift"]').getByText('已啟用').waitFor();
    check(await page.getByText('/security').isVisible(), 'enabled plugin commands are listed');
    await page.evaluate(() => window.CCA_DEV.openSettings('themes'));
    await page.getByRole('radio', { name: '餘燼', exact: true }).click();
    check(await page.evaluate(() => document.documentElement.dataset.theme) === 'hunt', 'a plugin theme borrows its base theme devices');
    check(await page.evaluate(() => getComputedStyle(document.documentElement).getPropertyValue('--signal').trim()) === '#ff8a3d',
      'a plugin theme applies its own colors');
    await page.getByRole('button', { name: '取消預覽', exact: true }).click();
    await page.keyboard.press('Escape');
    await page.evaluate(() => window.CCA_DEV.openConv('g1'));
    await page.getByRole('button', { name: '頻道設定', exact: true }).click();
    const modeRow = page.locator('.ops-row').filter({ hasText: '插件模式' });
    check(await modeRow.isVisible(), 'the channel settings offer plugin modes');
    await modeRow.getByRole('button').click();
    await page.getByRole('button', { name: '嚴格審查', exact: true }).click();
    const mode = await sent(page, 'extensions.mode');
    check(mode.id === 'g1' && mode.mode === 'night-shift/strict', 'picking a mode sends it to the host');
    await page.close();


    // ---- the phone simulator: the Android package can be looked at on the PC, with the recommendation beside the button
    page = await open('editions');
    await page.evaluate(() => window.CCA_DEV.openSettings('updates'));
    await page.getByRole('button', { name: '立即掃描', exact: true }).click();
    const android = page.locator('[data-update-android]');
    await android.waitFor();
    check(await android.getByText('你已經有桌面版了，建議使用桌面版').isVisible(), 'the Android package says the desktop version is recommended');
    await android.locator('[data-haiku-open]').click();
    const opened = await sent(page, 'update.haiku');
    check(opened && opened.path.endsWith('.apk'), 'the button asks the host to open that package in the phone simulator');
    await page.close();

    check(errors.length === 0, 'no browser script errors: ' + errors.join('; '));
    console.log(`${checks.length} plugin and edition browser checks passed`);
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
