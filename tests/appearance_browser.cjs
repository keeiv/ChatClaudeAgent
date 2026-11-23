// Real Edge theme previews, persistence boundaries, keyboard navigation and routing controls.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const { chromium } = require('playwright');

(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
  const errors = [], results = [];
  let checks = 0;
  const check = (value, name) => { assert.ok(value, name); checks++; };
  page.on('pageerror', e => errors.push(e.message));
  await page.addInitScript(() => {
    let backend;
    window.appearanceRequests = [];
    Object.defineProperty(window, 'MockBackend', { configurable: true, get: () => backend, set(value) {
      backend = value;
      const connect = value.connect, receive = value.receive;
      value.connect = fn => { window.appearanceDeliver = fn; return connect(message => {
        if (message.type === 'boot') window.appearanceBoot = structuredClone(message);
        fn(message);
      }); };
      value.receive = message => {
        window.appearanceRequests.push(structuredClone(message));
        if (window.appearanceHoldPreview && message.type === 'routing.preview') return;
        return receive(message);
      };
    } });
  });
  fs.mkdirSync('build/oss-upgrade/screenshots', { recursive: true });
  await page.goto((process.env.CCA_UI_URL || 'http://127.0.0.1:5220/') + '#mock');
  await page.waitForTimeout(1200);
  const open = section => page.evaluate(section => window.CCA_DEV.openSettings(section), section);
  const theme = () => page.locator('html').getAttribute('data-theme');
  await open('themes');
  check(await page.getByRole('radio').count() === 2, 'the ordinary edition offers the dark and light looks');
  await page.getByRole('radio', { name: 'CCA 亮色', exact: true }).click();
  check(await theme() === 'cca-light', 'preview applies the whole interface');
  check(await page.evaluate(() => !window.appearanceRequests.some(m => m.type === 'theme.set')), 'preview does not write settings');
  await page.getByRole('button', { name: '取消預覽', exact: true }).click();
  check(await theme() === 'cca', 'cancel restores the saved look');
  await page.getByRole('radio', { name: 'CCA 暗色', exact: true }).focus();
  await page.keyboard.press('ArrowRight');
  await page.waitForFunction(() => document.activeElement?.dataset.themeOption === 'cca-light');
  check(await theme() === 'cca-light', 'arrow keys move the radio selection');
  await page.keyboard.press('Home');
  await page.waitForFunction(() => document.activeElement?.dataset.themeOption === 'cca');
  check(await theme() === 'cca', 'Home chooses the first look');
  await page.getByRole('radio', { name: 'CCA 亮色', exact: true }).click();
  await page.getByRole('button', { name: '套用', exact: true }).click();
  await page.keyboard.press('Escape');
  check(await theme() === 'cca-light', 'a saved look survives closing settings');
  await open('themes');
  await page.getByRole('radio', { name: 'CCA 暗色', exact: true }).click();
  await page.evaluate(() => { window.mockDeskSaveFailure = true; });
  await page.getByRole('button', { name: '套用', exact: true }).click();
  check(await theme() === 'cca-light', 'a failed save restores the previous look');
  await page.evaluate(() => { window.mockDeskSaveFailure = false; });
  await page.getByRole('radio', { name: 'CCA 暗色', exact: true }).click();
  await page.keyboard.press('Escape');
  check(await theme() === 'cca-light', 'closing an uncommitted preview restores the saved look');

  // A look reserved for a special edition is not offered in the ordinary edition.
  const themes = await page.evaluate(() => window.CCAThemeCatalog.filter(t => !t.edition).map(t => ({ id: t.id, name: t.name })));
  for (const item of themes) {
    await open('themes');
    await page.getByRole('radio', { name: item.name, exact: true }).click();
    const apply = page.getByRole('button', { name: '套用', exact: true });
    if (await apply.isEnabled()) await apply.click();
    await page.keyboard.press('Escape');
    const contrast = await page.evaluate(() => {
      const s = getComputedStyle(document.documentElement), get = k => s.getPropertyValue('--' + k).trim();
      const rgb = hex => hex.match(/[a-f\d]{2}/gi).map(v => parseInt(v, 16) / 255).map(v => v <= .04045 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4);
      const lum = hex => rgb(hex).reduce((sum, c, i) => sum + c * [.2126, .7152, .0722][i], 0);
      const ratio = (a, b) => { const x = lum(a), y = lum(b); return (Math.max(x, y) + .05) / (Math.min(x, y) + .05); };
      return { ...Object.fromEntries(['text', 'text-muted', 'claude-name', 'codex-name'].map(k => [k, ratio(get(k), get('bg-chat'))])),
        'brand-ink': ratio(get('brand-ink'), get('brand')) };
    });
    for (const [key, ratio] of Object.entries(contrast)) check(ratio >= 4.5, `${item.id} ${key} contrast ${ratio}`);
    check(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `${item.id} no horizontal overflow`);
    await page.screenshot({ path: `build/oss-upgrade/screenshots/${item.id}-chat.png` });
    await page.setViewportSize({ width: 1000, height: 600 });
    await open('themes');
    check(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `${item.id} narrow settings width`);
    await page.keyboard.press('Escape');
    await page.setViewportSize({ width: 1440, height: 1000 });
    results.push({ ...item, contrast });
  }
  await open('routing');
  check(await page.locator('.routing-provider').count() === 2, 'group exposes independent providers');
  const first = page.locator('.routing-provider').first();
  await first.getByRole('checkbox').first().check();
  check(await page.evaluate(() => window.appearanceRequests.some(m => m.type === 'routing.set' && m.agent === 'claude' && m.model === true)), 'model switch uses native persistence protocol');
  check(await first.getByRole('checkbox').nth(1).isChecked() === false, 'model selection does not change effort dimension');
  await page.getByRole('textbox', { name: '選擇預覽的需求' }).fill('檢查整個專案的並發安全');
  await page.getByRole('button', { name: '預覽選擇', exact: true }).click();
  check(await page.locator('.route-preview').innerText().then(t => t.includes('深入分析')), 'preview displays decision');
  check(await page.evaluate(() => !window.appearanceRequests.some(m => m.type === 'conv.send')), 'preview never sends a model prompt');
  await page.getByRole('textbox', { name: '選擇預覽的需求' }).fill('new request');
  check(await page.locator('.route-preview').count() === 0, 'editing clears obsolete preview');
  await page.evaluate(() => {
    window.appearanceHoldPreview = true;
    window.appearanceDeliver({ type: 'routing.preview', requestToken: 'obsolete', result: { category: 'stale reply' } });
  });
  check(await page.locator('.route-preview').count() === 0, 'stale asynchronous preview ignored');
  await page.evaluate(() => { window.mockDeskSaveFailure = true; });
  await first.getByRole('checkbox').nth(1).click();
  check(await first.getByRole('checkbox').nth(1).isChecked() === false, 'failed routing save reverts checkbox');
  await page.evaluate(() => { window.mockDeskSaveFailure = false; });
  await page.keyboard.press('Escape');
  await page.getByRole('button', { name: '頻道設定', exact: true }).click();
  await page.locator('.ops-row').filter({ hasText: '主刀' }).getByRole('button').last().click();
  check(await page.evaluate(() => window.appearanceRequests.some(m => m.type === 'conv.settings' && m.settings.lead === 'codex')), 'ordinary channel controls retain conversation settings protocol');
  await page.getByRole('button', { name: '頻道設定', exact: true }).click();
  check(await page.locator('.ops-row').filter({ hasText: 'Claude 模型' }).innerText().then(t => t.includes('自動模型')), 'model label shows automatic preference instead of a stale manual choice');
  await page.locator('.ops-row').filter({ hasText: 'Claude 模型' }).getByRole('button').click();
  await page.locator('.pop-cols > div').first().getByRole('button').last().click();
  check(await page.evaluate(() => window.appearanceRequests.some(m => m.type === 'routing.manual' && m.agent === 'claude' && 'claudeModel' in m.settings)), 'manual model picker uses atomic native selection protocol');
  check(errors.length === 0, errors.join('\n'));
  fs.writeFileSync('build/oss-upgrade/browser-results.json', JSON.stringify({ checks, errors, themes: results }, null, 2));
  console.log(`${checks} appearance and routing browser checks passed`);
  await browser.close();
})().catch(error => { console.error(error); process.exit(1); });
