// The console's real DOM: contrast, draft survival, request ordering and narrow review layouts.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { chromium } = require('playwright');

(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const page = await browser.newPage({ viewport: { width: 1440, height: 940 } });
  const errors = [], checks = [];
  const check = (value, label) => { assert.ok(value, label); checks.push(label); };
  page.on('pageerror', e => errors.push(e.message));
  page.setDefaultTimeout(10000);
  const url = (process.env.CCA_UI_URL || 'http://127.0.0.1:5208/') + '#mock';
  const shots = path.resolve('build/ui-shots');
  fs.mkdirSync(shots, { recursive: true });
  try {
    await page.route('**/mock.js', route => route.fulfill({ contentType: 'text/javascript', body:
      fs.readFileSync('ui/mock.js', 'utf8') + `
      const originalReceive = window.MockBackend.receive, originalConnect = window.MockBackend.connect;
      window.__identityRequests = [];
      window.MockBackend.connect = callback => originalConnect(message => {
        if (message.type === 'review') window.__lastReview = structuredClone(message);
        window.__identityHost = callback; callback(message);
      });
      window.MockBackend.receive = message => {
        window.__identityRequests.push(structuredClone(message));
        if (message.type === 'conv.send' && window.__holdNotes) return;
        if (message.type === 'review.open' && window.__holdReviews) return;
        originalReceive(message);
      };` }));
    const boot = async () => {
      await page.goto(url);
      await page.waitForFunction(() => window.CCA_DEV);
      await page.locator('.gate').waitFor({ state: 'hidden' });
      await page.evaluate(() => window.CCA_DEV.openConv('g1'));
    };
    await boot();
    await page.evaluate(() => window.CCA_DEV.openReview());
    const pane = page.getByRole('region', { name: '審查變更' });
    await page.locator('.rv-file').first().waitFor();
    const filter = page.getByRole('searchbox', { name: '篩選變更檔案' });
    await filter.fill('cart');
    check(await page.locator('.rv-file').count() === 1, 'file filtering selects the matching diff');
    await filter.fill('nothing-matches');
    check(await pane.innerText().then(t => t.includes('沒有符合的檔案')), 'empty filter is distinct from no changes');
    await filter.fill('');
    const files = page.locator('.rv-file');
    await files.first().focus();
    await page.keyboard.press('ArrowDown');
    check(await files.nth(1).getAttribute('aria-pressed') === 'true', 'arrow navigation changes selected file');
    await page.keyboard.press('Home');
    check(await files.first().evaluate(el => el === document.activeElement), 'Home returns focus to the first file');

    await page.locator('.add-note').first().click();
    const editor = page.getByRole('textbox', { name: '逐行審查意見' });
    const draft = '檢查邊界：<img src=x onerror=alert(1)> 不應成為可執行內容';
    await editor.fill(draft);
    await page.getByRole('button', { name: '關閉審查窗格' }).click();
    await page.reload();
    await page.waitForFunction(() => window.CCA_DEV);
    await page.locator('.gate').waitFor({ state: 'hidden' });
    await page.evaluate(() => { window.CCA_DEV.openConv('g1'); window.CCA_DEV.openReview(); });
    await page.locator('.rv-file').first().waitFor();
    await page.locator('.add-note').first().click();
    check(await editor.inputValue() === draft, 'unfinished line draft survives closing and reload');
    await page.getByRole('button', { name: '加入意見', exact: true }).click();
    check(await page.locator('.rv-note img').count() === 0, 'review notes are text, never injected markup');
    await page.locator('.rv-notes summary').click();
    await page.screenshot({ path: path.join(shots, 'review-notes-after.png') });
    await page.evaluate(() => { window.__holdReviews = true; window.CCA_DEV.openReview('task'); window.CCA_DEV.openReview('task'); });
    await page.evaluate(() => {
      const requests = window.__identityRequests.filter(m => m.type === 'review.open');
      const old = requests.at(-2), latest = requests.at(-1), reply = window.__lastReview;
      window.__identityHost({ ...reply, requestToken: latest.requestToken });
      window.__identityHost({ ...reply, requestToken: old.requestToken, files: [] });
    });
    check(await page.locator('.rv-file').count() === 3, 'late response cannot overwrite a newer review of the same scope');
    check(await page.locator('.rv-notes summary').innerText().then(t => t.includes('1 則')), 'saved comments survive refreshing the scope');
    await page.evaluate(() => { window.__holdNotes = true; });
    const submit = page.getByRole('button', { name: /^交給 .+ 處理$/ });
    await submit.click();
    check(await page.getByRole('button', { name: '正在送出…' }).isDisabled(), 'submission waits for host acknowledgement');
    check(await page.evaluate(() => JSON.parse(localStorage.getItem('cca.review.g1')).notes.length) === 1,
      'notes remain persisted while a send has not been acknowledged');
    await page.evaluate(() => {
      const sent = window.__identityRequests.filter(m => m.type === 'conv.send').at(-1);
      window.__identityHost({ type: 'msg', convId: 'g1', msg: { id: 'identity-note-ack', author: 'user', text: sent.text, ts: Date.now() } });
    });
    await pane.waitFor({ state: 'hidden' });
    check(await page.evaluate(() => JSON.parse(localStorage.getItem('cca.review.g1')).notes.length) === 0,
      'notes clear only after the host records the user message');
    await page.evaluate(() => {
      window.__holdReviews = false;
      const write = Storage.prototype.setItem;
      Storage.prototype.setItem = function (key, value) {
        if (key.startsWith('cca.review.')) throw new Error('test storage unavailable');
        return write.call(this, key, value);
      };
      window.CCA_DEV.openReview();
    });
    await page.locator('.add-note').first().click();
    await editor.fill('儲存失敗時仍應保留在記憶體');
    await page.getByRole('button', { name: '加入意見', exact: true }).click();
    check(await page.locator('.rv-foot').innerText().then(t => t.includes('本機儲存不可用')),
      'unavailable storage shows a warning rather than claiming the draft is saved');
    await page.getByRole('button', { name: '關閉審查窗格' }).click();

    await page.evaluate(() => { window.__holdReviews = false; window.CCA_DEV.openModelPicker('claude'); });
    const item = page.locator('.pop-item').first();
    await item.hover();
    const colours = await item.evaluate(el => ({ foreground: getComputedStyle(el).color, background: getComputedStyle(el).backgroundColor }));
    const luminance = colour => colour.match(/[\d.]+/g).slice(0, 3).map(Number).map(v => {
      v /= 255; return v <= .04045 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4;
    }).reduce((n, v, i) => n + v * [.2126, .7152, .0722][i], 0);
    const fg = luminance(colours.foreground), bg = luminance(colours.background);
    check((Math.max(fg, bg) + .05) / (Math.min(fg, bg) + .05) >= 4.5, 'hovered menu text clears 4.5:1 contrast');
    await page.keyboard.press('Escape');
    check(await page.getByRole('button', { name: '頻道設定', exact: true }).count() > 0,
      'the settings entry remains reachable after closing a picker');
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await page.evaluate(() => window.CCA_DEV.openReview());
    await page.locator('.rv-file').first().waitFor();
    check(await page.evaluate(() => document.getAnimations().filter(a => a.playState === 'running').length) === 0,
      'reduced motion disables the timeline, streaming and pane animations');
    await page.screenshot({ path: path.join(shots, 'review-after-wide.png') });
    for (const width of [820, 1100]) {
      await page.setViewportSize({ width, height: 680 });
      const bounds = await pane.boundingBox();
      const foot = await page.locator('.rv-foot').boundingBox();
      const input = await filter.boundingBox();
      check(bounds.x >= 0 && bounds.x + bounds.width <= width + 1 && foot.y + foot.height <= 681 && input.width > 80,
        `${width}px: review controls and submission remain inside the window`);
    }
    await page.screenshot({ path: path.join(shots, 'review-after-narrow.png') });
    await page.getByRole('button', { name: '關閉審查窗格' }).click();
    await page.screenshot({ path: path.join(shots, 'identity-after.png') });
    check(errors.length === 0, 'no browser script errors: ' + errors.join('; '));
    console.log(checks.length + ' visual identity browser checks passed');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
