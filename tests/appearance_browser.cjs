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
  check(await page.getByRole('radio').count() === 16, 'provider light/dark pairs, source themes, the two halyosy themes, Halloween pair and Hunt');
  await page.getByRole('radio', { name: 'Anthropic Claude · 亮色', exact: true }).click();
  check(await theme() === 'claude', 'preview applies full interface');
  check(await page.evaluate(() => !window.appearanceRequests.some(m => m.type === 'theme.set')), 'preview does not write settings');
  await page.getByRole('button', { name: '取消預覽', exact: true }).click();
  check(await theme() === 'cca', 'cancel restores saved theme');
  await page.getByRole('radio', { name: 'OpenAI Codex · 亮色', exact: true }).click();
  await page.waitForFunction(() => document.activeElement?.dataset.themeOption === 'codex');
  await page.keyboard.press('ArrowRight');
  await page.waitForFunction(() => document.activeElement?.dataset.themeOption === 'codex-dark');
  check(await theme() === 'codex-dark', 'keyboard selects separate dark variant');
  await page.keyboard.press('ArrowRight');
  await page.waitForFunction(() => document.activeElement?.dataset.themeOption === 'claude');
  check(await theme() === 'claude' && await page.getByRole('radio', { name: 'Anthropic Claude · 亮色', exact: true }).evaluate(e => e === document.activeElement), 'keyboard radio selection and focus');
  await page.keyboard.press('End');
  await page.waitForFunction(() => document.activeElement?.dataset.themeOption === 'hunt');
  check(await theme() === 'hunt', 'End chooses final theme');
  await page.keyboard.press('Home');
  await page.waitForFunction(() => document.activeElement?.dataset.themeOption === 'cca');
  check(await theme() === 'cca', 'Home chooses original theme');
  await page.getByRole('radio', { name: 'Anthropic Claude · 亮色', exact: true }).click();
  await page.getByRole('button', { name: '套用主題', exact: true }).click();
  await page.keyboard.press('Escape');
  check(await theme() === 'claude', 'successful save survives closing settings');
  await open('themes');
  await page.getByRole('radio', { name: 'Google Gemini · 亮色', exact: true }).click();
  await page.evaluate(() => { window.mockDeskSaveFailure = true; });
  await page.getByRole('button', { name: '套用主題', exact: true }).click();
  check(await theme() === 'claude', 'failed save restores previous theme');
  await page.evaluate(() => { window.mockDeskSaveFailure = false; });
  await page.getByRole('radio', { name: 'The Sky', exact: true }).click();
  await page.keyboard.press('Escape');
  check(await theme() === 'claude', 'closing uncommitted preview restores saved theme');

  // A theme reserved for a special edition is not offered in the ordinary edition.
  const themes = await page.evaluate(() => window.CCAThemeCatalog.filter(t => !t.edition).map(t => ({ id: t.id, name: t.name })));
  for (const item of themes) {
    await open('themes');
    await page.getByRole('radio', { name: item.name, exact: true }).click();
    await page.screenshot({ path: `build/oss-upgrade/screenshots/${item.id}-settings.png` });
    const apply = page.getByRole('button', { name: '套用主題', exact: true });
    if (await apply.isEnabled()) await apply.click();
    await page.keyboard.press('Escape');
    const contrast = await page.evaluate(() => {
      const s = getComputedStyle(document.documentElement), get = k => s.getPropertyValue('--' + k).trim();
      const rgb = hex => hex.match(/[a-f\d]{2}/gi).map(v => parseInt(v, 16) / 255).map(v => v <= .04045 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4);
      const lum = hex => rgb(hex).reduce((sum, c, i) => sum + c * [.2126, .7152, .0722][i], 0);
      const ratio = (a, b) => { const x = lum(a), y = lum(b); return (Math.max(x, y) + .05) / (Math.min(x, y) + .05); };
      return Object.fromEntries(['text', 'text-muted', 'claude', 'codex'].map(k => [k, ratio(get(k), get('bg-chat'))]));
    });
    if (item.id !== 'cca') for (const [key, ratio] of Object.entries(contrast)) check(ratio >= 4.5, `${item.id} ${key} contrast ${ratio}`);
    // Each theme must show the device borrowed from its source, not only a different palette.
    const signature = await page.evaluate(id => {
      const style = (selector, pseudo, prop) => {
        const el = document.querySelector(selector);
        return el ? getComputedStyle(el, pseudo).getPropertyValue(prop) : '';
      };
      const devices = {
        codex: () => style('.member-head', '::before', 'content').includes('>_'),
        claude: () => style('.member-head', '::before', 'content').includes('✻'),
        gemini: () => style('.member-head', '::before', 'clip-path').startsWith('polygon(50% 0'),
        'tokyo-night': () => style('.chat-head .title', null, 'border-top-width') === '2px',
        'urban-legend': () => style('#main', '::after', 'content').includes('URBAN LEGEND'),
        'the-sky': () => style('#rail', null, 'background-image').includes('linear-gradient'),
        infinite: () => style('.member-head', '::after', 'content').includes(']'),
        connecting: () => style('.chat-head', '::after', 'background-image').includes('linear-gradient') &&
          style('.messages', '::before', 'width') === '2px',
        blessing: () => style('.chat-head', null, 'background-image').includes('radial-gradient'),
        halloween: () => style('.chat-head', '::after', 'background-image').includes('conic-gradient') &&
          style('.chat-scroll', null, 'background-image').includes('repeating-conic-gradient'),
        hunt: () => style('#main', '::after', 'content').includes('追獵行動') &&
          style('.composer', null, 'clip-path').startsWith('polygon'),
      };
      const check = devices[id.replace(/-dark$/, '')];
      return check ? check() : true;
    }, item.id);
    check(signature, `${item.id} shows its own signature device`);
    check(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `${item.id} no horizontal overflow`);
    await page.screenshot({ path: `build/oss-upgrade/screenshots/${item.id}-chat.png` });
    await page.setViewportSize({ width: 1000, height: 600 });
    await open('themes');
    check(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `${item.id} narrow settings width`);
    await page.screenshot({ path: `build/oss-upgrade/screenshots/${item.id}-narrow.png` });
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
