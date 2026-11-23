// Exercise the conversation sidebar in Edge: group rows, their tools, real drag gestures, keyboard sorting and
// failure recovery. Groups and private conversations share one list; the host owns the group order.
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
  const row = id => page.locator(`#sidebar [data-group-id="${id}"]`);
  const dm = name => page.locator('#sidebar .dm:not(.group-row)').filter({ hasText: name });
  const order = () => page.locator('#sidebar [data-group-id]').evaluateAll(items => items.map(item => item.dataset.groupId));
  const load = async flags => {
    await page.goto(url + '?navigation-test=' + Date.now() + '#' + flags);
    await row('g1').waitFor();
    await page.waitForTimeout(900);
  };
  const create = async name => {
    await page.locator('#sidebar').getByRole('button', { name: '建立協作群組', exact: true }).click();
    const dialog = page.getByRole('dialog', { name: '建立協作群組', exact: true });
    await dialog.locator('input').first().fill(name);
    await dialog.getByRole('button', { name: '建立', exact: true }).click();
    const created = page.locator('#sidebar [data-group-id]').filter({ hasText: name });
    await created.waitFor();
    return created.getAttribute('data-group-id');
  };
  const removeCurrent = async () => {
    await page.locator('#sidebar .group-row[aria-current="page"]').click({ button: 'right' });
    await page.locator('.group-menu').getByRole('button', { name: '清除或刪除群組', exact: true }).click();
    await page.getByRole('dialog', { name: '清除或刪除群組', exact: true }).getByRole('button', { name: '刪除群組', exact: true }).click();
  };
  try {
    await load('');
    check(await row('g1').getAttribute('aria-current') === 'page', 'the open group is the selected row');
    check(await dm('Claude').count() === 1 && await dm('GPT').count() === 1 && await dm('Gemini').count() === 1,
      'private conversations stay in the same list as the groups');
    check(await page.locator('#sidebar .group-tools').count() === 1, 'only the open group shows its tools');
    await page.locator('#sidebar').getByRole('button', { name: '任務控制', exact: true }).click();
    await page.locator('.group-tools-menu').getByRole('button', { name: '計畫與驗收', exact: true }).click();
    check(await page.getByRole('dialog', { name: '編輯工作計畫', exact: true }).isVisible(), 'task control opens the plan editor');
    await page.keyboard.press('Escape');
    await page.locator('#sidebar').getByRole('button', { name: '交付驗證', exact: true }).click();
    await page.locator('.group-tools-menu').getByRole('button', { name: '需求與驗收證據', exact: true }).click();
    check(await page.getByRole('dialog', { name: '需求與驗收證據', exact: true }).isVisible(), 'delivery verification opens the evidence tools');
    await page.keyboard.press('Escape');
    await page.locator('#sidebar').getByRole('button', { name: '群組設定', exact: true }).click();
    check(await page.getByRole('dialog', { name: '頻道設定', exact: true }).isVisible(), 'group settings keep the permission and model controls');
    await page.keyboard.press('Escape');
    await page.locator('#sidebar').getByRole('button', { name: '審查變更', exact: true }).click();
    await page.locator('.review-pane').waitFor();
    check(await page.locator('#panel').evaluate(el => el.classList.contains('collapsed')), 'the review pane takes the inspector space');

    await dm('GPT').click();
    check((await page.locator('.chat-head .title').textContent()).includes('GPT'), 'a private conversation opens from the same list');
    check(await page.locator('#sidebar .group-tools').count() === 0, 'group tools close when a private conversation is open');
    await page.screenshot({ path: path.join(out, 'private.png') });
    await row('g1').click();

    const bot = await create('BOT. 網頁'), engine = await create('NovaGrid 引擎');
    check(JSON.stringify(await order()) === JSON.stringify(['g1', bot, engine]), 'new groups append to stable positions');
    check(await row(engine).getAttribute('draggable') === 'true', 'rows use the actual HTML draggable state');
    await row(engine).focus();
    await page.keyboard.press('Alt+ArrowUp');
    await page.waitForTimeout(120);
    check(JSON.stringify(await order()) === JSON.stringify(['g1', engine, bot]), 'Alt+Up changes and saves the group order');
    check(await page.locator(':focus').getAttribute('data-group-id') === engine, 'keyboard focus survives the host acknowledgement');
    await page.keyboard.press('Shift+F10');
    await page.locator('.group-menu').getByRole('button', { name: '向上移動', exact: true }).click();
    await page.waitForTimeout(120);
    check((await order())[0] === engine, 'the context menu offers an alternative way to sort');
    const box = await row(bot).boundingBox();
    await row(engine).dragTo(row(bot), { sourcePosition: { x: 40, y: 20 }, targetPosition: { x: 40, y: Math.round(box.height - 6) } });
    await page.waitForTimeout(180);
    check(JSON.stringify(await order()) === JSON.stringify(['g1', bot, engine]), 'a real pointer drag saves the expected position');
    const beforeActivity = await order();
    await page.evaluate(() => window.navDeliver({ type: 'msg', convId: 'g1', msg: {
      id: 'nav-incoming', author: 'claude', text: '需要確認', ts: Date.now(),
      blocks: [{ id: 'nav-approval', type: 'approval', state: 'pending', title: '測試核准' }],
    } }));
    await page.waitForTimeout(80);
    check(JSON.stringify(await order()) === JSON.stringify(beforeActivity), 'incoming activity never moves groups');
    check(await row('g1').evaluate(el => el.classList.contains('unread')), 'the unread mark belongs to the right group');
    check(await row('g1').evaluate(el => el.querySelector('.badge')?.textContent === '1'), 'pending requests are counted on that group');
    await row('g1').click();
    check(await row('g1').evaluate(el => !el.classList.contains('unread')), 'opening the group clears its unread mark');
    await row(engine).click();
    await page.waitForTimeout(180);
    await page.screenshot({ path: path.join(out, 'groups.png') });
    await row(engine).focus();
    await page.keyboard.press('ArrowUp');
    check(await page.locator(':focus').getAttribute('data-group-id') === bot, 'arrow keys move between rows without opening them');
    await page.keyboard.press('Home');
    check(await page.locator(':focus').getAttribute('data-group-id') === 'g1', 'Home reaches the first row');
    await page.keyboard.press('End');
    check((await page.locator(':focus').getAttribute('data-nav-id') || '').startsWith('dm:'), 'End reaches the last private conversation');

    await page.setViewportSize({ width: 1000, height: 720 });
    check(await page.locator('#sidebar').evaluate(el => el.scrollWidth === el.clientWidth), 'the sidebar fits the narrow desktop layout');
    await page.getByRole('button', { name: '檢視面板', exact: true }).click();
    await page.locator('#panel').getByRole('tab', { name: '成員', exact: true }).click();
    check(await page.locator('#panel').evaluate(el => el.classList.contains('drawer') && !el.classList.contains('collapsed')),
      'the inspector opens as a drawer on narrow windows');
    await page.screenshot({ path: path.join(out, 'groups-narrow.png') });
    await page.getByRole('button', { name: '隱藏檢視面板', exact: true }).click();
    await page.setViewportSize({ width: 1000, height: 420 });
    const sidebarScroll = await page.locator('#sidebar .side-list').evaluate(el => { el.scrollTop = 80; return el.scrollTop; });
    await page.evaluate(id => window.navDeliver({ type: 'convState', state: { id, typing: [], running: false, group: {} } }), engine);
    await page.waitForTimeout(60);
    check(sidebarScroll > 0 && await page.locator('#sidebar .side-list').evaluate(el => el.scrollTop) === sidebarScroll,
      'streaming redraws keep the sidebar scroll position in short windows');
    await page.setViewportSize({ width: 1440, height: 1000 });
    await removeCurrent();
    check(await row(engine).count() === 0, 'deleting the open group removes its row');
    await row(bot).click(); await removeCurrent();
    await row('g1').click(); await removeCurrent();
    check(await page.locator('#sidebar [data-group-id]').count() === 0, 'deleting the final group leaves no group rows');
    check(await page.locator('#sidebar .side-empty').isVisible(), 'an empty list offers to create the first group');
    check(await dm('Claude').count() === 1, 'private conversations remain available without groups');

    await load('nav-fail');
    const failedGroup = await create('保存失敗測試');
    const beforeFailure = await order();
    await row(failedGroup).focus();
    await page.keyboard.press('Alt+ArrowUp');
    await page.waitForTimeout(100);
    check(JSON.stringify(await order()) === JSON.stringify(beforeFailure), 'failed persistence never replaces the real order');
    check(await page.getByText('群組排序未能儲存，原順序已保留。', { exact: true }).isVisible(), 'failed persistence gives a specific error');
    check(await row(failedGroup).getAttribute('draggable') === 'true', 'failure releases the pending transaction for retry');
    check(errors.length === 0, 'no browser runtime errors: ' + errors.join('\n'));
    fs.writeFileSync(path.join(out, 'browser-results.json'), JSON.stringify({ checks, errors }, null, 2) + '\n');
    console.log(`${checks} group navigation browser checks passed`);
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
