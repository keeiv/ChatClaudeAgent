// MCP tool pinning dialog in a real Edge page; the mock host fakes the worker's tool listing and hashes.
// Needs Playwright, Edge and the UI server (node tools/serve_ui.js with PORT=5220, or CCA_UI_URL).
const assert = require('node:assert/strict');
const { chromium } = require('playwright');

(async () => {
  const checks = [], errors = [];
  const check = (value, label) => { assert.ok(value, label); checks.push(label); };
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const page = await browser.newPage({ viewport: { width: 1380, height: 950 } });
  page.setDefaultTimeout(10000);
  page.on('pageerror', error => errors.push(error.message));
  try {
    await page.addInitScript(() => {
      window.__pins = [];
      Object.defineProperty(window, 'CCA_SENT', { value: window.__pins });
    });
    await page.route('**/mock.js', route => route.fulfill({ contentType: 'text/javascript', body:
      require('node:fs').readFileSync('ui/mock.js', 'utf8') + `
        const receive = window.MockBackend.receive;
        window.MockBackend.receive = message => { if (message.type.startsWith('toolserver.')) window.__pins.push(message); receive(message); };` }));
    await page.goto((process.env.CCA_UI_URL || 'http://127.0.0.1:5220/') + '#mock,toolpin');
    await page.waitForFunction(() => window.CCA_DEV);
    await page.waitForTimeout(1200);
    await page.evaluate(() => window.CCA_DEV.openSettings('platform'));
    const summary = page.locator('[data-pin-summary="docs"]');
    check((await summary.innerText()).includes('已釘住 0／2'), 'a server with no pins says so');

    await page.getByRole('button', { name: '連線與工具發現', exact: true }).click();
    await page.getByText('MCP 工具與釘住：docs').waitFor();
    const rows = page.locator('.tool-pin');
    check(await rows.count() === 3, 'every tool the server advertises is listed');
    check(await page.locator('.tool-pin[data-pin-state="unlisted"] input').isDisabled(), 'a tool outside the allowlist cannot be ticked');
    check(await page.locator('.tool-pin[data-pin-state="unpinned"]').count() === 2, 'new tools start as unpinned');
    check(await page.locator('.tool-pin input:checked').count() === 0, 'nothing is pinned until the user ticks it');
    await page.screenshot({ path: 'build/mcp-pin-dialog.png' });
    for (const name of ['search', 'read_page']) await page.getByRole('checkbox', { name }).check();
    await page.getByRole('button', { name: '釘住所選工具', exact: true }).click();
    await page.waitForFunction(() => window.__pins.some(m => m.type === 'toolserver.pin'));
    const sent = await page.evaluate(() => window.__pins.find(m => m.type === 'toolserver.pin'));
    check(sent.id === 'docs' && Object.keys(sent.pins).sort().join() === 'read_page,search' && Object.values(sent.pins).every(v => /^[0-9a-f]{64}$/.test(v)),
      'the pin request carries the hashes the user was shown');
    await page.waitForFunction(() => document.querySelector('[data-pin-summary="docs"]')?.textContent.includes('已釘住 2／2'));
    check(true, 'the card shows both tools pinned after the host answers');

    await page.evaluate(() => { window.mockMcpDrift = true; });
    await page.getByRole('button', { name: '連線與工具發現', exact: true }).click();
    await page.getByText('MCP 工具與釘住：docs').waitFor();
    check(await page.locator('.tool-pin[data-pin-state="changed"]').count() === 1, 'a tool whose description changed is flagged');
    check(await page.locator('.tool-pin[data-pin-state="changed"] input').isChecked() === false, 'a changed tool is not pre-ticked for re-approval');
    check(await page.locator('.tool-pin[data-pin-state="ok"] input').isChecked(), 'an unchanged pinned tool stays ticked');
    check(await page.getByText('描述已變更，已阻擋').isVisible(), 'the dialog explains that the tool is blocked');
    await page.waitForTimeout(500);
    await page.screenshot({ path: 'build/mcp-pin-changed.png' });
    await page.keyboard.press('Escape');
    check(await page.locator('body').evaluate(el => el.scrollWidth <= window.innerWidth), 'no horizontal overflow');
    check(errors.length === 0, 'no browser script errors: ' + errors.join('; '));
    console.log(`${checks.length} MCP pin browser checks passed`);
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
