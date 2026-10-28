// Exercise project rail navigation, real drag gestures, keyboard sorting and failure recovery in Edge.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');

(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const out = path.resolve('build/group-navigation');
  fs.mkdirSync(out, { recursive: true });
  let checks = 0;
  const errors = [];
  const check = (ok, label) => { assert.ok(ok, label); checks++; };
  const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
  page.on('pageerror', error => errors.push(error.message));
  await page.addInitScript(() => {
    let backend;
    window.navRequests = [];
    Object.defineProperty(window, 'MockBackend', { configurable: true, get: () => backend, set(value) {
      backend = value;
      const connect = value.connect, receive = value.receive;
      value.connect = fn => { window.navDeliver = fn; return connect(fn); };
      value.receive = message => { window.navRequests.push(structuredClone(message)); return receive(message); };
    } });
  });
  const url = process.env.CCA_UI_URL || 'http://127.0.0.1:5218/';
  const rail = id => page.locator(`[data-group-id="${id}"]`);
  const home = () => page.locator('#rail').getByRole('button', { name: '私人訊息', exact: true });
  const order = () => page.locator('[data-group-id]').evaluateAll(items => items.map(item => item.dataset.groupId));
  const load = async flags => {
    await page.goto(url + '?navigation-test=' + Date.now() + '#' + flags);
    await rail('g1').waitFor();
    await page.waitForTimeout(900);
  };
  const create = async name => {
    await page.locator('#rail').getByRole('button', { name: '建立協作群組', exact: true }).click();
    const dialog = page.getByRole('dialog', { name: '建立協作群組', exact: true });
    await dialog.locator('input').first().fill(name);
    await dialog.getByRole('button', { name: '建立', exact: true }).click();
    const button = page.locator('#rail').getByRole('button', { name, exact: true });
    await button.waitFor();
    return button.getAttribute('data-group-id');
  };
  const removeCurrent = async () => {
    await page.locator('.group-nav-heading').click();
    await page.locator('.group-menu').getByRole('button', { name: '清除或刪除群組', exact: true }).click();
    await page.getByRole('dialog', { name: '清除或刪除群組', exact: true }).getByRole('button', { name: '刪除群組', exact: true }).click();
  };
  try {
    await load('');
    check(await rail('g1').getAttribute('aria-current') === 'page', 'group owns its selected rail icon');
    check(await home().getAttribute('aria-current') === null, 'Home is not selected inside a project group');
    check(await page.locator('#sidebar .dm').count() === 0, 'group sidebar does not mix in private conversations');
    check(await page.locator('.group-nav-link[aria-current="page"]').textContent() === '協作對話', 'one real group conversation is shown without fake channels');
    check(await page.locator('.group-nav-heading').textContent() === '協作群組', 'sidebar identifies the selected group');
    await page.locator('#sidebar').getByRole('button', { name: '任務控制', exact: true }).click();
    await page.locator('.group-tools-menu').getByRole('button', { name: '計畫與驗收', exact: true }).click();
    check(await page.getByRole('dialog', { name: '編輯工作計畫', exact: true }).isVisible(), 'task control opens the existing functional plan editor');
    await page.keyboard.press('Escape');
    await page.locator('#sidebar').getByRole('button', { name: '交付驗證', exact: true }).click();
    await page.locator('.group-tools-menu').getByRole('button', { name: '需求與驗收證據', exact: true }).click();
    check(await page.getByRole('dialog', { name: '需求與驗收證據', exact: true }).isVisible(), 'delivery verification opens the existing evidence tools');
    await page.keyboard.press('Escape');
    await page.locator('#sidebar').getByRole('button', { name: '群組設定', exact: true }).click();
    check(await page.getByRole('dialog', { name: '頻道設定', exact: true }).isVisible(), 'group settings retain the actual permission and model controls');
    await page.keyboard.press('Escape');
    await page.locator('#sidebar').getByRole('button', { name: '審查變更', exact: true }).click();
    await page.locator('.review-pane').waitFor();
    await page.locator('#sidebar').getByRole('button', { name: '成員名單', exact: true }).click();
    check(await page.locator('.review-pane').count() === 0 && await page.locator('#panel').evaluate(el => !el.classList.contains('collapsed')),
      'member entry restores the member panel when the review pane was using its space');

    await home().click();
    check(await page.locator('#sidebar .dm').count() === 3 && await page.locator('#sidebar .dm').filter({ hasText: 'Gemini' }).count() === 1,
      'Home contains the Claude, GPT and Gemini private conversations');
    check(await page.locator('#sidebar').getByText('協作群組', { exact: true }).count() === 0, 'group is removed from the private message list');
    await page.locator('#sidebar .dm').filter({ hasText: 'GPT' }).click();
    await rail('g1').click();
    await home().click();
    check((await page.locator('.chat-head .title').textContent()).includes('GPT'), 'Home restores the most recent private conversation');
    await page.screenshot({ path: path.join(out, 'home.png') });

    const bot = await create('BOT. 網頁'), engine = await create('NovaGrid 引擎');
    check(JSON.stringify(await order()) === JSON.stringify(['g1', bot, engine]), 'new groups append to stable positions');
    check(await rail(engine).getAttribute('draggable') === 'true', 'rail icons use the actual HTML draggable state');
    await rail(engine).focus();
    await page.keyboard.press('Alt+ArrowUp');
    await page.waitForTimeout(120);
    check(JSON.stringify(await order()) === JSON.stringify(['g1', engine, bot]), 'Alt+Up changes and saves group order');
    check(await page.locator(':focus').getAttribute('data-group-id') === engine, 'keyboard focus survives the host ordering acknowledgement');
    await page.keyboard.press('Shift+F10');
    await page.locator('.group-menu').getByRole('button', { name: '向上移動', exact: true }).click();
    await page.waitForTimeout(120);
    check((await order())[0] === engine, 'context menu provides an accessible alternate sort method');
    await rail(engine).dragTo(rail(bot), { sourcePosition: { x: 24, y: 24 }, targetPosition: { x: 24, y: 40 } });
    await page.waitForTimeout(180);
    check(JSON.stringify(await order()) === JSON.stringify(['g1', bot, engine]), 'real pointer drag saves the expected destination position');
    const beforeActivity = await order();
    await page.evaluate(() => window.navDeliver({ type: 'msg', convId: 'g1', msg: {
      id: 'nav-incoming', author: 'claude', text: '需要確認', ts: Date.now(),
      blocks: [{ id: 'nav-approval', type: 'approval', state: 'pending', title: '測試核准' }],
    } }));
    await page.waitForTimeout(80);
    check(JSON.stringify(await order()) === JSON.stringify(beforeActivity), 'incoming activity never moves group icons');
    check(await rail('g1').evaluate(el => el.parentElement.classList.contains('unread')), 'unread indicator belongs to the correct group');
    check(await rail('g1').evaluate(el => el.parentElement.querySelector('.rail-pending')?.textContent === '1'), 'pending requests are shown on that group icon');
    await rail('g1').click();
    check(await rail('g1').evaluate(el => !el.parentElement.classList.contains('unread')), 'opening the group clears its unread indicator');
    await rail(engine).click();
    await page.waitForTimeout(180);
    await page.screenshot({ path: path.join(out, 'groups.png') });
    await rail(engine).focus();
    await page.keyboard.press('ArrowUp');
    check(await page.locator(':focus').getAttribute('data-group-id') === bot, 'arrow keys navigate group icons without opening another conversation');
    await page.keyboard.press('Home');
    check(await page.locator(':focus').getAttribute('data-nav-id') === 'home', 'Home key reaches private messages');
    await page.keyboard.press('End');
    check(await page.locator(':focus').getAttribute('data-nav-id') === 'new', 'End key reaches group creation');
    await page.setViewportSize({ width: 1000, height: 720 });
    check(await page.locator('#rail').evaluate(el => el.scrollWidth === el.clientWidth), 'rail fits the narrow desktop layout');
    await page.locator('#sidebar').getByRole('button', { name: '成員名單', exact: true }).click();
    check(await page.locator('#panel').evaluate(el => el.classList.contains('drawer') && !el.classList.contains('collapsed')), 'member entry opens the existing narrow-screen drawer');
    await page.screenshot({ path: path.join(out, 'groups-narrow.png') });
    await page.setViewportSize({ width: 1000, height: 600 });
    const sidebarScroll = await page.locator('#sidebar .side-list').evaluate(el => { el.scrollTop = 80; return el.scrollTop; });
    await page.evaluate(id => window.navDeliver({ type: 'convState', state: { id, typing: [], running: false, group: {} } }), engine);
    await page.waitForTimeout(60);
    check(sidebarScroll > 0 && await page.locator('#sidebar .side-list').evaluate(el => el.scrollTop) === sidebarScroll,
      'streaming redraws preserve the group sidebar scroll position in short windows');
    await page.setViewportSize({ width: 1440, height: 1000 });
    await removeCurrent();
    check(await rail(engine).count() === 0 && await home().getAttribute('aria-current') === 'page', 'deleting selected group returns to private messages');
    await rail(bot).click(); await removeCurrent();
    await rail('g1').click(); await removeCurrent();
    check(await page.locator('[data-group-id]').count() === 0, 'deleting the final group leaves an empty rail');
    check(await page.locator('#rail').getByRole('button', { name: '建立協作群組', exact: true }).isVisible(), 'empty rail retains group creation');

    await load('nav-fail');
    const failedGroup = await create('保存失敗測試');
    const beforeFailure = await order();
    await rail(failedGroup).focus();
    await page.keyboard.press('Alt+ArrowUp');
    await page.waitForTimeout(100);
    check(JSON.stringify(await order()) === JSON.stringify(beforeFailure), 'failed persistence never optimistically replaces the real order');
    check(await page.getByText('群組排序未能儲存，原順序已保留。', { exact: true }).isVisible(), 'failed persistence gives a specific error');
    check(await rail(failedGroup).getAttribute('draggable') === 'true', 'failure releases the pending transaction for retry');
    check(errors.length === 0, 'no browser runtime errors');
    fs.writeFileSync(path.join(out, 'browser-results.json'), JSON.stringify({ checks, errors }, null, 2) + '\n');
    console.log(`${checks} group navigation browser checks passed`);
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
