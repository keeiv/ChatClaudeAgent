// Reproduce the supplied file links and unbroken shell command in real Edge layouts.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  try {
    const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
    const errors = [];
    let checks = 0;
    const check = (value, label) => { assert.ok(value, label); checks++; };
    page.on('pageerror', e => errors.push(e.message));
    await page.addInitScript(() => {
      let backend;
      window.boundRequests = [];
      Object.defineProperty(window, 'MockBackend', { configurable: true, get: () => backend, set(value) {
        backend = value;
        const connect = value.connect, receive = value.receive;
        value.connect = fn => { window.boundDeliver = fn; return connect(fn); };
        value.receive = message => { window.boundRequests.push(message); return receive(message); };
      } });
    });
    await page.goto((process.env.CCA_UI_URL || 'http://127.0.0.1:5220/') + '#mock');
    await page.waitForTimeout(1000);
    await page.evaluate(() => window.CCA_DEV.openConv('dm-claude'));
    await page.waitForTimeout(100);
    const command = '"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" -Command "node tests/gemini_account.cjs; ' +
      '$env:NODE_PATH=(Resolve-Path build/gemini-theme-tools/node_modules).Path; node tests/appearance_browser.cjs"';
    const source = '順序在 [docs/workbench.md](<C:/Users/dev/CCA/docs/workbench.md:47>)。\n\n' +
      '- [桌面版](<C:/My Project/ChatClaudeAgent-1.0.0-terra.exe>)\n- [CLI 版](<C:/My Project/ChatClaudeAgent-CLI-1.0.0-terra.exe>)';
    await page.evaluate(({ command, source }) => window.boundDeliver({ type: 'history', id: 'dm-claude', messages: [{
      id: 'bounds-fixture', author: 'claude', ts: Date.now(), status: 'done', blocks: [
        { type: 'text', text: source },
        { type: 'tool', id: 'long-command', kind: 'command', title: '執行', command, output: 'fixture output', status: 'ok' },
        { type: 'tool', id: 'no-output', kind: 'command', title: '執行', command: 'a'.repeat(900), status: 'running' },
      ],
    }] }), { command, source });
    const message = page.locator('.msg[data-id="bounds-fixture"]');
    await message.waitFor();
    check(await message.locator('a.local-link').count() === 3, 'all screenshot links parsed');
    check(!(await message.innerText()).includes('](<'), 'raw destination syntax hidden');
    await message.getByRole('link', { name: '桌面版', exact: true }).click();
    check(await page.evaluate(() => window.boundRequests.some(m => m.type === 'file.reveal' && m.target.endsWith('.exe'))), 'executable link uses folder action');
    check(await page.evaluate(() => !window.boundRequests.some(m => m.type === 'open' && m.target?.endsWith('.exe'))), 'executable is never directly launched');
    if (await message.locator('.act-fold.closed').count()) await message.locator('.act-fold.closed').click();
    const details = message.locator('.act-detail').first();
    await details.hover();
    const tip = page.getByRole('tooltip');
    await tip.waitFor();
    const contained = () => tip.evaluate(e => {
      const r = e.getBoundingClientRect();
      return e.scrollWidth <= e.clientWidth + 1 && r.left >= 7 && r.top >= 7 && r.right <= innerWidth - 7 && r.bottom <= innerHeight - 7;
    });
    check(await contained(), 'long PowerShell path wraps inside tooltip and viewport');
    fs.mkdirSync('build/routing-link-fix/screenshots', { recursive: true });
    await page.screenshot({ path: 'build/routing-link-fix/screenshots/command-tooltip.png' });
    await details.click();
    check((await message.locator('.act-out pre').first().innerText()).includes(command), 'expanded tool includes complete single-line command');
    await message.locator('.act-row').last().click();
    check(await message.locator('.act-out pre').last().innerText() === 'a'.repeat(900), 'command without output can expand');
    for (const size of [{ width: 1000, height: 600 }, { width: 800, height: 500 }]) {
      await page.setViewportSize(size);
      await page.waitForTimeout(80);
      await page.mouse.move(1, 1);
      await details.hover();
      await tip.waitFor();
      check(await contained(), 'tooltip contained at ' + size.width + 'x' + size.height);
      check(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'no viewport overflow');
    }
    await page.screenshot({ path: 'build/routing-link-fix/screenshots/narrow.png' });
    await page.setViewportSize({ width: 1440, height: 900 });
    await page.evaluate(() => window.CCA_DEV.openSettings('routing'));
    await page.getByRole('textbox', { name: '選擇預覽的需求' }).fill('修復資料遺失並驗證併發');
    await page.getByRole('button', { name: '預覽選擇', exact: true }).click();
    await page.evaluate(() => {
      const request = window.boundRequests.filter(m => m.type === 'routing.preview').at(-1);
      window.boundDeliver({ type: 'routing.preview', requestToken: request.requestToken, result: {
        category: '深入分析', modelName: 'fixture-only', effort: 'high', reason: '多因素測試',
        factors: [{ label: '資料完整性', evidence: '資料遺失' }, { label: '驗證要求', evidence: '<img src=x onerror=alert(1)>' }],
      } });
    });
    check(await page.locator('.route-factors dt').count() === 2, 'preview exposes individual contributing factors');
    check(await page.locator('.route-factors dd').last().innerText() === '<img src=x onerror=alert(1)>', 'factor evidence stays text');
    check(await page.locator('.route-factors img').count() === 0, 'factor evidence cannot inject markup');
    await page.screenshot({ path: 'build/routing-link-fix/screenshots/routing-factors.png' });
    check(errors.length === 0, errors.join('\n'));
    fs.writeFileSync('build/routing-link-fix/browser.json', JSON.stringify({ checks, errors }, null, 2));
    console.log(checks + ' message bounds browser checks passed');
  } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
