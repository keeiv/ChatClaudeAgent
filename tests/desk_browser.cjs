// Real Edge checks for context boundaries, queue acknowledgements and responsive workspace controls.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const { chromium } = require('playwright');

(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const page = await browser.newPage({ viewport: { width: 1440, height: 960 } });
  const errors = [];
  let checks = 0;
  const check = (value, message) => { assert.ok(value, message); checks++; };
  page.on('pageerror', error => errors.push(error.message));
  await page.addInitScript(() => {
    let backend;
    window.deskRequests = [];
    Object.defineProperty(window, 'MockBackend', { configurable: true, get: () => backend, set(value) {
      backend = value;
      const connect = value.connect, receive = value.receive;
      value.connect = fn => { window.deskDeliver = fn; return connect(message => {
        if (message.type === 'boot') window.deskBoot = structuredClone(message);
        fn(message);
      }); };
      value.receive = message => { window.deskRequests.push(structuredClone(message)); return receive(message); };
    } });
  });
  const url = process.env.CCA_UI_URL || 'http://127.0.0.1:5219/';
  await page.goto(url + '#mock');
  await page.waitForTimeout(1100);
  await page.getByRole('button', { name: '上下文與待送指令', exact: true }).click();
  check(await page.getByText('尚無來源紀錄。', { exact: false }).isVisible(), 'old turns not fabricated as receipts');
  await page.evaluate(() => window.deskDeliver({ type: 'context', id: 'g1', items: [{ id: 'test', agent: 'claude', ts: Date.now(), status: 'completed',
    model: 'fixture', permission: 'ask', snapshot: 'host-snapshot-123', history: 'external-session',
    request: { text: 'original', bytes: 8 }, input: { text: '<img src=x onerror=alert(1)>', bytes: 99999, truncated: true },
    instructions: { text: 'rules', bytes: 5 }, observations: [{ tool: 'read', path: 'main.cpp', version: 'unknown' }] }] }));
  await page.locator('.desk-receipt').first().locator('summary').first().click();
  check(await page.getByText('來源檢查點：host-snapshot-123', { exact: false }).isVisible(), 'host snapshot rendered');
  check(await page.getByText('接續既有工作階段，內容未知', { exact: false }).isVisible(), 'external history explicitly unknown');
  await page.getByText('實際交付文字（含 CCA 交接及摘要）', { exact: false }).click();
  check(await page.getByText('<img src=x onerror=alert(1)>', { exact: true }).isVisible(), 'untrusted context rendered as text');
  check(await page.locator('.desk-dialog img').count() === 0, 'context cannot inject image or script');
  check(await page.getByText('顯示已截短', { exact: false }).isVisible(), 'truncation labeled');
  await page.getByRole('tab', { name: '待送指令', exact: true }).click();
  check(await page.getByText('佇列已暫停', { exact: true }).isVisible(), 'queue initial state paused');
  await page.keyboard.press('Escape');
  await page.evaluate(() => {
    const state = window.deskBoot.states.find(s => s.id === 'g1');
    window.deskDeliver({ type: 'convState', state: { ...state, running: true, typing: ['claude'], group: { ...state.group, active: true, paused: false } } });
  });
  const input = page.locator('.composer textarea');
  await input.fill('preserve draft');
  await page.evaluate(() => { window.mockDeskSaveFailure = true; });
  await input.press('Enter');
  await page.waitForTimeout(80);
  check(await input.inputValue() === 'preserve draft', 'failed persistence preserves composer text');
  await page.evaluate(() => {
    window.mockDeskSaveFailure = false;
    const state = window.deskBoot.states.find(s => s.id === 'g1');
    window.deskDeliver({ type: 'convState', state: { ...state, running: true, typing: ['claude'], group: { ...state.group, active: true, paused: false } } });
  });
  await input.press('Enter');
  await page.waitForTimeout(80);
  check(await input.inputValue() === '', 'composer clears only after host acknowledgement');
  check(await page.evaluate(() => window.deskRequests.filter(m => m.type === 'queue.add').length) === 2, 'busy sends use durable queue');
  await page.getByRole('button', { name: '上下文與待送指令', exact: true }).click();
  await page.getByRole('tab', { name: '待送指令', exact: true }).click();
  check(await page.locator('.desk-queue-text').innerText() === 'preserve draft', 'queued text visible');
  await page.getByRole('button', { name: '編輯', exact: true }).click();
  const edit = page.getByRole('dialog', { name: '編輯待送指令', exact: true });
  await edit.locator('textarea').fill('edited instruction');
  await edit.getByRole('button', { name: '儲存', exact: true }).click();
  await page.waitForTimeout(80);
  check(await page.locator('.desk-queue-text').innerText() === 'edited instruction', 'queue edit acknowledged');
  await page.getByRole('button', { name: '取消', exact: true }).click();
  check(await page.locator('.desk-queue-item').count() === 0, 'queued item cancelled');
  await page.keyboard.press('Escape');
  await page.getByRole('button', { name: '使用者設定', exact: true }).click();
  await page.getByRole('button', { name: '版面與閱讀', exact: true }).click();
  await page.getByRole('button', { name: '閱讀', exact: true }).click();
  check(await page.locator('html').getAttribute('data-layout') === 'reading', 'reading preset applied');
  await page.getByRole('button', { name: '審查', exact: true }).click();
  check(await page.locator('html').getAttribute('data-density') === 'compact', 'review preset density');
  await page.evaluate(() => { window.mockDeskSaveFailure = true; });
  await page.getByRole('button', { name: '閱讀', exact: true }).click();
  check(await page.locator('html').getAttribute('data-layout') === 'review', 'failed save restores previous layout');
  await page.evaluate(() => { window.mockDeskSaveFailure = false; });
  await page.getByRole('button', { name: '軟體更新', exact: true }).click();
  check(await page.getByText('遠端發行站', { exact: true }).isVisible() && await page.getByText('未啟用', { exact: true }).isVisible(),
    'remote channel is shown but not enabled');
  check(await page.locator('.update-source.disabled input').count() === 0, 'remote channel offers no editable source');
  check(await page.getByText('installers', { exact: false }).isVisible(), 'automatically created local folder is shown');
  check(await page.getByRole('button', { name: '更新並重新開啟', exact: true }).count() === 0, 'no fake available update');
  check(!(await page.locator('.settings-content').innerText()).includes('null'), 'missing release metadata omitted');
  await page.getByRole('button', { name: '立即掃描', exact: true }).click();
  check(await page.getByText('資料夾中沒有安裝包', { exact: false }).isVisible(), 'empty folder is reported truthfully');
  const auto = page.getByRole('checkbox', { name: /發現新安裝包時自動更新/ });
  check(await auto.isChecked(), 'automatic install defaults on');
  await auto.uncheck();
  check(await page.evaluate(() => window.deskRequests.some(m => m.type === 'update.auto' && m.auto === false)), 'auto switch uses the host protocol');
  await page.evaluate(() => window.deskDeliver({ type: 'update.state', status: 'scheduled', available: true, auto: true, current: '1.0.0-', version: '1.0.0-oss',
    package: 'ChatClaudeAgent-1.0.0-oss.exe', folder: 'C:\\Users\\you\\AppData\\Roaming\\CCA\\installers', notes: 'Fixture release', busy: false, remote: { enabled: false } }));
  check(await page.getByRole('button', { name: '取消這次自動更新', exact: true }).isVisible(), 'scheduled automatic install can be cancelled');
  check(await page.getByRole('button', { name: '更新並重新開啟', exact: true }).isVisible(), 'verified candidate exposes update button');
  await page.screenshot({ path: 'build/context-queue-layout-update/update-available.png' });
  await page.keyboard.press('Escape');
  const splitter = page.getByRole('separator', { name: '調整頻道欄寬' });
  await splitter.focus(); await page.keyboard.press('ArrowRight');
  check(await page.evaluate(() => parseInt(getComputedStyle(document.documentElement).getPropertyValue('--desk-side'))) === 210, 'keyboard resizing persisted');
  const box = await splitter.boundingBox();
  await page.mouse.move(box.x + 3, box.y + 50); await page.mouse.down();
  await page.mouse.move(box.x + 53, box.y + 50); await page.mouse.up();
  check(await page.evaluate(() => parseInt(getComputedStyle(document.documentElement).getPropertyValue('--desk-side'))) >= 250, 'real pointer resizing');
  await page.setViewportSize({ width: 1000, height: 600 });
  await page.waitForTimeout(80);
  const main = await page.locator('#main').boundingBox();
  check(main.width >= 400, 'narrow window retains reading width');
  check(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), 'no horizontal viewport overflow');
  await page.screenshot({ path: 'build/context-queue-layout-update/narrow.png' });
  check(errors.length === 0, errors.join('\n'));
  fs.writeFileSync('build/context-queue-layout-update/browser-results.json', JSON.stringify({ checks, errors }, null, 2));
  console.log(`${checks} workbench browser checks passed`);
  await browser.close();
})().catch(error => { console.error(error); process.exit(1); });
