// Actual Chromium DOM, Canvas and image decoding; host messages are deterministic fixtures.
const fs = require('node:fs');
const path = require('node:path');
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
    await page.route('**/mock.js', route => {
      const code = fs.readFileSync('ui/mock.js', 'utf8') + `
        window.__sent = [];
        const receive = window.MockBackend.receive, connect = window.MockBackend.connect;
        window.MockBackend.receive = message => { window.__sent.push(message); receive(message); };
        window.MockBackend.connect = callback => {
          window.__host = callback;
          connect(message => {
            if (message.type === 'boot') window.__boot = message;
            if (message.type === 'convState') window.__stateCount = (window.__stateCount || 0) + 1;
            callback(message);
          });
        };`;
      return route.fulfill({ contentType: 'text/javascript', body: code });
    });
    await page.goto((process.env.CCA_UI_URL || 'http://127.0.0.1:5198/') + '#mock');
    await page.waitForFunction(() => window.CCA_DEV && window.__boot);
    await page.evaluate(() => window.CCA_DEV.openConv('g1'));
    // The pipeline is configured from the channel settings panel (header sliders button).
    await page.getByRole('button', { name: '頻道設定', exact: true }).click();
    const pipelineRow = page.locator('.ops-row').filter({ hasText: '多代理流程' });
    check(await pipelineRow.getByText('雙方協作', { exact: true }).isVisible(), 'legacy group remains the default');
    await pipelineRow.locator('button').click();
    await page.getByText('每位代理有獨立模型與會話', { exact: false }).waitFor();
    check(await page.locator('.pipeline-node').count() === 2, 'editor loads two existing CLI agents');
    await page.getByRole('button', { name: '新增代理', exact: true }).click();
    check(await page.locator('.pipeline-node').count() === 3, 'editor adds a third independent agent');
    await page.locator('.pipeline-node').last().getByRole('button', { name: '上移', exact: true }).click();
    await page.locator('.pipeline-node').nth(1).locator('select').first().selectOption('local');
    check(await page.locator('.pipeline-node').nth(1).locator('select').nth(1).locator('option[value="write"]').count() === 0, 'local reviewer has no writable role');
    await page.getByRole('button', { name: '儲存流程', exact: true }).waitFor({ state: 'visible' });
    await page.waitForFunction(() => ![...document.querySelectorAll('button')].find(b => b.textContent === '儲存流程')?.disabled);
    await page.getByRole('button', { name: '儲存流程', exact: true }).click();
    check(await page.evaluate(() => window.__sent.some(m => m.type === 'pipeline.set' && m.pipeline.nodes.length === 3)), 'configured order is sent to host');
    const nodes = [{ id: 'writer', name: 'API 作者', backend: 'api', role: 'write' }, { id: 'review-a', name: 'API 審查甲', backend: 'api', role: 'review' }, { id: 'review-b', name: 'API 審查乙', backend: 'api', role: 'review' }];
    await page.evaluate(nodes => {
      const c = window.__boot.convs.find(c => c.id === 'g1');
      c.pipeline = { nodes, maxCycles: 3, requireTests: true }; c.cwd = 'C:\\Example\\project';
      window.__host({ type: 'convs', items: window.__boot.convs });
      window.__host({ type: 'auth', ok: false, claude: { state: 'missing' }, codex: { state: 'missing' } });
    }, nodes);
    await page.getByRole('button', { name: '改用 API，設定獨立計費的供應商', exact: true }).click();
    await page.getByRole('button', { name: '新增 API 供應商', exact: true }).click();
    check(await page.locator('input[type="password"]').count() === 1, 'API key field uses password input');
    check(await page.getByText('API 由供應商獨立計費', { exact: false }).isVisible(), 'API billing is distinct from subscriptions');
    await page.getByRole('button', { name: '取消', exact: true }).last().click();
    await page.keyboard.press('Escape');
    const stateCount = await page.evaluate(() => window.__stateCount || 0);
    await page.evaluate(() => window.CCA_DEV.openConv('g1'));
    await page.waitForFunction(before => window.__stateCount > before, stateCount);
    check(await page.locator('.composer textarea').isEnabled(), 'API pipeline can send without subscription login');
    check(await page.locator('.workflow-host').isHidden() || await page.locator('.workflow-host .wf.wf-pipeline').count() === 1,
      'legacy two-node graph is replaced by the configured pipeline layout');
    const folder = path.resolve('build/platform-review'); fs.mkdirSync(folder, { recursive: true });
    const buffer = await page.screenshot({ path: path.join(folder, 'chat-screenshot.png') });
    const image = { id: 'fixture-artifact', task_id: 'task-visual', file: 'data:image/png;base64,' + buffer.toString('base64'), ts: Date.now(), source: 'browser-test', operation: '多代理聊天室截圖', result: 'PASS' };
    await page.evaluate(({ nodes, image }) => {
      window.__host({ type: 'convState', state: { id: 'g1', running: false, typing: [], group: {}, task: { task_id: 'task-visual', pipeline: { nodes }, cursor: 1, cycle: 1, gate: 'NEEDS_FIX', artifacts: [image] } } });
      window.__host({ type: 'msg', convId: 'g1', msg: { id: 'visual-evidence', author: 'system', kind: 'artifact', ts: Date.now(), text: '測試圖片', blocks: [{ type: 'image', artifact: image }] } });
    }, { nodes, image });
    await page.getByRole('button', { name: '放大測試圖片', exact: true }).first().waitFor();
    check(await page.getByText('API 作者 → API 審查甲 → API 審查乙', { exact: true }).isVisible(), 'dynamic order is visible in task panel');
    check(await page.getByText('NEEDS_FIX', { exact: true }).isVisible(), 'review gate is visible');
    await page.getByRole('button', { name: '放大測試圖片', exact: true }).first().click();
    await page.waitForFunction(() => document.querySelector('.task-picture-full')?.naturalWidth > 0);
    check(await page.locator('.task-picture-full').evaluate(el => el.naturalWidth) === 1380, 'actual screenshot is decoded in preview, rather than a path label');
    await page.screenshot({ path: path.join(folder, 'screenshot-preview.png'), animations: 'disabled' });
    await page.keyboard.press('Escape');
    await page.setViewportSize({ width: 720, height: 840 });
    check(await page.locator('body').evaluate(el => el.scrollWidth <= window.innerWidth), 'narrow layout does not overflow');
    check(errors.length === 0, 'no browser script errors: ' + errors.join('; '));
    fs.writeFileSync(path.join(folder, 'browser.json'), JSON.stringify({ passed: checks.length, checks, errors }, null, 2) + '\n');
    console.log(`${checks.length} platform browser checks passed`);
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
