// Browser UI wiring uses a labelled mock; actual Edge and native provenance are separate tests.
const assert = require('node:assert/strict');
const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const page = await browser.newPage({ viewport: { width: 1400, height: 980 } }), errors = [];
  page.on('pageerror', e => errors.push(e.message));
  let checks = 0;
  const check = (value, label) => { assert.ok(value, label); checks++; };
  try {
    await page.goto((process.env.CCA_UI_URL || 'http://127.0.0.1:5208/') + '#mock');
    await page.waitForFunction(() => window.CCA_DEV);
    await page.locator('.gate').waitFor({ state: 'hidden' });
    await page.evaluate(() => window.CCA_DEV.openConv('g1'));
    await page.locator('#panel').getByRole('button', { name: '需求與驗收證據', exact: true }).click();
    await page.getByRole('textbox', { name: '證據名稱', exact: true }).fill('邊界測試證據');
    await page.getByRole('button', { name: '核對並執行', exact: true }).click();
    check(await page.getByRole('dialog', { name: '確認執行驗證' }).isVisible(), 'test command requires an explicit reviewable confirmation');
    await page.getByRole('button', { name: '執行此命令', exact: true }).click();
    await page.getByText('模擬驗證', { exact: true }).waitFor();
    check(await page.getByText('邊界測試證據', { exact: true }).isVisible(), 'requirement evidence appears with source label');
    await page.getByRole('combobox', { name: '驗證種類' }).selectOption('repro');
    check(await page.getByRole('textbox', { name: '修正前 Git 版本' }).isVisible(), 'reproduction requests a real before revision');
    check(await page.getByRole('textbox', { name: '修正前預期失敗訊息' }).isVisible(), 'a specific old-version failure is required');
    await page.keyboard.press('Escape');
    await page.locator('#panel').getByRole('button', { name: '網站即時預覽', exact: true }).click();
    await page.getByRole('button', { name: '開啟網站', exact: true }).click();
    await page.locator('.quality-preview-image').evaluate(img => new Promise(resolve => { if (img.naturalWidth) resolve(); else img.onload = resolve; }));
    check(await page.locator('.quality-preview-image').evaluate(img => img.naturalWidth) === 640, 'preview displays a decoded browser frame');
    await page.getByRole('button', { name: '手機畫面', exact: true }).click();
    await page.getByRole('status').filter({ hasText: '390 px' }).waitFor(); checks++;
    await page.locator('.quality-preview-image').click({ position: { x: 30, y: 30 } });
    await page.getByText('BUTTON #fixture · 測試元素', { exact: true }).waitFor(); checks++;
    await page.getByRole('button', { name: '執行操作驗收並保存證據', exact: true }).click();
    await page.setViewportSize({ width: 390, height: 844 });
    const bounds = await page.getByRole('dialog', { name: '網站預覽與操作驗收' }).boundingBox();
    check(bounds.x >= 0 && bounds.x + bounds.width <= 390, 'preview panel fits a phone screen');
    await page.screenshot({ path: 'build/ui-shots/quality-mobile.png', animations: 'disabled' });
    check(errors.length === 0, errors.join('; '));
    console.log(checks + ' quality UI checks passed');
  } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
