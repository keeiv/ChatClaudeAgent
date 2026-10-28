// Exercise actual heatmap interactions and the neutral persona library in Edge, without model calls.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');

(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const out = path.resolve('build/usage-persona-upgrade');
  fs.mkdirSync(out, { recursive: true });
  let checks = 0;
  const check = (value, label) => { assert.ok(value, label); checks++; };
  try {
    const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.addInitScript(() => {
      let backend;
      Object.defineProperty(window, 'MockBackend', { configurable: true, get: () => backend, set(value) {
        backend = value;
        const connect = value.connect;
        value.connect = fn => { window.activityTestDeliver = fn; return connect(fn); };
      } });
    });
    await page.goto((process.env.CCA_UI_URL || 'http://127.0.0.1:5208/') + '#account');
    await page.locator('.activity-grid button').first().waitFor();
    await page.waitForTimeout(900);
    const cells = page.locator('.activity-grid button');
    check(await cells.count() === 365, 'one real calendar cell for each date');
    check(await page.locator('.activity-months span').count() >= 11, 'month labels fit a year');
    check(await page.locator('.activity-cell.level-4').count() > 0, 'nonzero usage has visible intensity');
    await cells.last().focus();
    await page.keyboard.press('ArrowLeft');
    const end = new Date(await cells.last().getAttribute('data-date') + 'T00:00:00Z');
    const prior = new Date(end - 7 * 86400000).toISOString().slice(0, 10);
    check(await page.locator(':focus').getAttribute('data-date') === prior, 'left arrow moves one week');
    await page.keyboard.press('Home');
    check(await page.locator(':focus').getAttribute('data-date') === await cells.first().getAttribute('data-date'), 'Home moves to first actual day');
    await page.keyboard.press('End');
    await page.keyboard.press('Enter');
    check((await page.locator('.activity-detail').textContent()).includes(await cells.last().getAttribute('data-date')), 'keyboard selection exposes precise date and value');
    await page.getByRole('button', { name: '每週', exact: true }).click();
    check((await page.locator('.activity-note').textContent()).includes('近 7 日'), 'weekly definition is visible');
    check(await page.getByRole('button', { name: '每週', exact: true }).getAttribute('aria-pressed') === 'true', 'mode has accessible selected state');
    await page.getByRole('button', { name: '累計', exact: true }).click();
    check((await cells.last().getAttribute('aria-label')).includes('累計'), 'cumulative data reaches cells');
    await page.getByRole('button', { name: '每日', exact: true }).click();
    await cells.first().hover();
    check((await page.locator('.activity-tip').textContent()).includes(await cells.first().getAttribute('data-date')), 'hover tooltip has date and recorded status');
    const tip = await page.locator('.activity-tip').boundingBox();
    check(tip.x >= 0 && tip.x + tip.width <= 1440, 'tooltip stays in viewport');
    await page.screenshot({ path: path.join(out, 'activity-desktop.png'), fullPage: true });
    await page.setViewportSize({ width: 960, height: 850 });
    // The resize reaches the page asynchronously; wait for the layout instead of measuring a stale one on a busy machine.
    // Crossing 1200px redraws the whole page, so wait until the calendar is back and has a size before measuring it.
    await page.waitForFunction(() => innerWidth === 960 && (() => { const el = document.querySelector('.activity-scroll'); return el && el.clientWidth > 0; })(), null, { timeout: 10000 }).catch(() => {});
    const narrow = await page.evaluate(() => {
      const scroll = document.querySelector('.activity-scroll');
      const card = document.querySelector('.activity-card');
      return { scroll: scroll ? scroll.scrollWidth : -1, client: scroll ? scroll.clientWidth : -1, viewport: innerWidth,
        cards: document.querySelectorAll('.activity-card').length, cells: document.querySelectorAll('.activity-grid button').length,
        display: card ? getComputedStyle(card).display : 'none', hash: location.hash, view: document.body.className };
    });
    check(narrow.scroll > narrow.client, `narrow layout scrolls only the calendar (${JSON.stringify(narrow)})`);
    check(await page.locator('.activity-card').evaluate(el => el.getBoundingClientRect().right <= innerWidth), 'card does not overflow narrow viewport');
    await cells.last().focus();
    await page.screenshot({ path: path.join(out, 'activity-narrow.png') });
    await page.setViewportSize({ width: 1440, height: 1000 });
    await page.evaluate(() => window.CCA_DEV.openSettings('about'));
    await page.waitForTimeout(250);
    check((await page.locator('.about-section').textContent()).includes('進度') && (await page.locator('.about-section').textContent()).includes('開發目的'), 'About explains concrete coordination problems');
    await page.screenshot({ path: path.join(out, 'about-purpose.png') });
    await page.evaluate(() => window.CCA_DEV.openSettings('personas'));
    check(await page.locator('.persona-card').count() === 0, 'clean installation has no bundled characters');
    check((await page.locator('.settings-content').textContent()).includes('persona-presets.json'), 'local preset configuration is discoverable');
    await page.getByRole('button', { name: '建立性格', exact: true }).click();
    await page.locator('.modal input.input').first().fill('測試風格');
    await page.locator('.modal textarea').fill('說明保持簡潔。');
    await page.locator('.modal').getByRole('button', { name: '儲存', exact: true }).click();
    await page.locator('.persona-card').first().waitFor();
    check((await page.locator('.persona-card').textContent()).includes('測試風格'), 'custom style can be created in UI');
    await page.locator('.persona-card').getByRole('button', { name: '編輯', exact: true }).click();
    await page.getByRole('button', { name: '刪除這個性格', exact: true }).click();
    check(await page.locator('.persona-card').count() === 0, 'custom style can be removed');
    await page.keyboard.press('Escape');
    await page.evaluate(() => {
      window.CCA_DEV.openView('account');
      window.activityTestDeliver({ type: 'usage', usage: { daily: {}, activity: {
        days: [{ date: '2024-02-29', daily: 0, weekly: 0, cumulative: 0, recorded: false }],
        total: 0, windowTotal: 0, activeDays: 0,
      } } });
    });
    // Host updates are painted on the next animation frame; the previous 365 cells still exist until then.
    await page.waitForFunction(() => document.querySelectorAll('.activity-grid button').length === 1);
    check((await page.locator('.activity-grid button').getAttribute('aria-label')).includes('尚無當日紀錄'), 'missing data is not claimed as recorded zero');
    await page.evaluate(() => window.activityTestDeliver({ type: 'usage', usage: { daily: {}, activity: {
      days: [{ date: '2024-02-29', daily: 0, weekly: 0, cumulative: 0, recorded: true }], total: 0,
    } } }));
    await page.waitForFunction(() => document.querySelector('.activity-grid button')?.getAttribute('aria-label').includes('0 個 token'));
    check((await page.locator('.activity-grid button').getAttribute('aria-label')).includes('0 個 token'), 'explicit zero stays distinguishable');
    check(!errors.length, 'no browser execution errors');
    fs.writeFileSync(path.join(out, 'browser-result.json'), JSON.stringify({ checks, errors }, null, 2) + '\n');
    console.log(`${checks} activity and persona browser checks passed`);
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
