// Real browser coverage for model catalogs, delayed API discovery and editing without model IDs.
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
  const rows = page.locator('.pipeline-node');
  const models = row => row.getByRole('combobox', { name: '模型', exact: true });
  const provider = row => row.getByRole('combobox', { name: 'API 供應商', exact: true });
  const save = page.getByRole('button', { name: '儲存流程', exact: true });
  const reply = (id, result) => page.evaluate(({ id, result }) => window.__replyModels(id, result), { id, result });
  try {
    await page.route('**/mock.js', route => route.fulfill({ contentType: 'text/javascript', body: fs.readFileSync('ui/mock.js', 'utf8') + `
      window.__sent = [];
      window.__providers = {
        first: { id: 'first', name: '測試 API 甲', url: 'http://127.0.0.1/first', protocol: 'responses' },
        second: { id: 'second', name: '測試 API 乙', url: 'http://127.0.0.1/second', protocol: 'chat' },
      };
      const receive = window.MockBackend.receive, connect = window.MockBackend.connect;
      window.__platform = () => window.__host({ type: 'platform', providers: structuredClone(window.__providers), servers: [], sandbox: {} });
      window.MockBackend.receive = message => {
        window.__sent.push(message);
        if (message.type === 'provider.models') return;
        if (message.type === 'platform.get') { window.__platform(); return; }
        receive(message);
      };
      window.MockBackend.connect = callback => {
        window.__host = callback;
        connect(message => { if (message.type === 'boot') window.__boot = message; callback(message); });
      };
      window.__replyModels = (id, result) => {
        const request = window.__sent.filter(m => m.type === 'provider.models' && m.id === id).at(-1);
        window.__host({ type: 'platform.result', action: 'provider.models', id, requestToken: request.requestToken, result });
        if (result.ok) window.__providers[id].models = result.models;
        window.__platform();
      };` }));
    await page.goto((process.env.CCA_UI_URL || 'http://127.0.0.1:5198/') + '#mock');
    await page.waitForFunction(() => window.CCA_DEV && window.__boot);
    await page.evaluate(() => window.CCA_DEV.openConv('g1'));
    // The pipeline is configured from the channel settings panel (header sliders button).
    await page.getByRole('button', { name: '頻道設定', exact: true }).click();
    await page.locator('.ops-row').filter({ hasText: '多代理流程' }).locator('button').click();
    check(await rows.count() === 2, 'existing CLI agents load without reconfiguration');
    check(await models(rows.first()).locator('option').count() > 2, 'Claude models come from the existing CLI catalog');
    check(await models(rows.nth(1)).locator('option').count() > 2, 'Codex models come from the existing CLI catalog');
    check(await page.locator('.pipeline-node input[list], .pipeline-node datalist').count() === 0, 'model IDs no longer require free text input');
    await models(rows.first()).selectOption('sonnet');
    await page.evaluate(() => window.__host({ type: 'models', models: window.__boot.models }));
    check(await models(rows.first()).inputValue() === 'sonnet', 'model updates preserve the selected CLI model');
    await rows.nth(1).locator('select').first().selectOption('api');
    check(await provider(rows.nth(1)).inputValue() === 'first', 'API automatically selects an available provider');
    check(await models(rows.nth(1)).isDisabled() && await save.isDisabled(), 'empty pending catalog cannot save a model-less API agent');
    check(await page.evaluate(() => window.__sent.filter(m => m.type === 'provider.models' && m.id === 'first').length) === 1, 'API discovery is automatic and deduplicated');
    const name = rows.nth(1).locator('input[type="text"]').first();
    await name.fill('尚未離開欄位的名稱');
    await reply('first', { ok: true, models: [{ id: 'first-small', name: '甲的小型模型' }, { id: 'default' }] });
    check(await name.inputValue() === '尚未離開欄位的名稱' && await name.evaluate(el => el === document.activeElement), 'delayed catalog responses preserve focused input and unsaved text');
    check(await models(rows.nth(1)).inputValue() === 'first-small', 'API automatically fills the first model returned by its provider');
    check(await models(rows.nth(1)).locator('option[value="default"]').count() === 1, 'API model named default remains a valid model ID');
    await models(rows.nth(1)).selectOption('default');
    await provider(rows.nth(1)).selectOption('second');
    check(await models(rows.nth(1)).locator('option[value="first-small"]').count() === 0, 'switching providers removes the preceding provider catalog');
    await reply('second', { ok: false, error: '測試連線失敗' });
    check(await rows.nth(1).getByRole('status').textContent().then(text => text.includes('測試連線失敗')), 'discovery failure is shown on the affected agent');
    check(await save.isDisabled() && await rows.nth(1).getByRole('button', { name: '更新清單' }).isEnabled(), 'failed discovery can retry without saving an invalid model');
    await rows.nth(1).getByRole('button', { name: '更新清單' }).click();
    await reply('second', { ok: true, models: [] });
    check(await save.isDisabled() && await models(rows.nth(1)).isDisabled(), 'empty API catalogs are handled explicitly');
    await rows.nth(1).getByRole('button', { name: '更新清單' }).click();
    await reply('second', { ok: true, models: [{ id: 'second-small' }, { id: 'second-main', isDefault: true }] });
    check(await models(rows.nth(1)).inputValue() === 'second-main', 'API provider default model is preferred when supplied');
    await reply('first', { ok: true, models: [{ id: 'first-new' }] });
    check(await models(rows.nth(1)).inputValue() === 'second-main', 'late results from another provider cannot overwrite the selected model');
    await models(rows.nth(1)).selectOption('second-small');
    await rows.nth(1).getByRole('button', { name: '更新清單' }).click();
    const stale = await page.evaluate(() => window.__sent.filter(m => m.type === 'provider.models' && m.id === 'second').at(-1));
    await reply('second', { ok: true, models: [{ id: 'second-main' }] });
    check(await models(rows.nth(1)).inputValue() === 'second-small' && await save.isDisabled(), 'removed models remain visible and require an explicit replacement');
    await models(rows.nth(1)).selectOption('second-main');
    await rows.nth(1).getByRole('button', { name: '更新清單' }).click();
    await page.evaluate(stale => window.__host({ type: 'platform.result', action: 'provider.models', id: 'second', requestToken: stale.requestToken,
      result: { ok: true, models: [{ id: 'stale-result' }] } }), stale);
    check(await models(rows.nth(1)).inputValue() === 'second-main', 'older request tokens cannot replace a newer in-flight catalog');
    await reply('second', { ok: true, models: [{ id: 'second-main' }] });
    await page.getByRole('button', { name: '新增代理', exact: true }).click();
    await rows.last().locator('select').first().selectOption('local');
    await page.waitForFunction(() => document.querySelectorAll('.pipeline-node').length === 3 &&
      !document.querySelector('.pipeline-node:last-child select[aria-label="模型"]').disabled);
    check(await models(rows.last()).locator('option').count() > 0, 'local catalog automatically loads installed Ollama models');
    check(await rows.last().locator('select').nth(1).locator('option[value="write"]').count() === 0, 'local models retain their read-only role');
    const localModel = await models(rows.last()).inputValue();
    await rows.last().getByRole('button', { name: '上移' }).click();
    check(await models(rows.nth(1)).inputValue() === localModel, 'reordering retains each agent model');
    const folder = path.resolve('build/pipeline-models-review'); fs.mkdirSync(folder, { recursive: true });
    await page.screenshot({ path: path.join(folder, 'model-selects.png'), animations: 'disabled' });
    await page.setViewportSize({ width: 720, height: 840 });
    check(await page.getByRole('dialog').evaluate(el => el.scrollWidth <= el.clientWidth), 'model dropdowns fit the existing narrow dialog');
    await save.click();
    const pipeline = await page.evaluate(() => window.__sent.filter(m => m.type === 'pipeline.set').at(-1).pipeline);
    check(pipeline.nodes[0].model === 'sonnet' && pipeline.nodes[1].model === localModel &&
      pipeline.nodes[2].provider === 'second' && pipeline.nodes[2].model === 'second-main', 'host receives the selected models and matching provider after reorder');
    check(pipeline.nodes[2].name === '尚未離開欄位的名稱', 'name edits survive asynchronous model discovery and save');
    await page.evaluate(() => {
      window.__providers = {};
      window.CCAPlatform.update({ providers: {}, servers: [], sandbox: {} });
      window.CCAPlatform.edit({ id: 'fixture', pipeline: { nodes: [
        { id: 'writer', backend: 'claude', role: 'write', model: '' },
        { id: 'api', backend: 'api', role: 'review', model: 'retired', provider: 'removed' },
      ] } });
    });
    check(await save.isDisabled() && await provider(rows.nth(1)).isDisabled(), 'missing providers cannot silently select another provider');
    await page.keyboard.press('Escape');
    check(errors.length === 0, 'no browser script errors: ' + errors.join('; '));
    fs.writeFileSync(path.join(folder, 'browser.json'), JSON.stringify({ passed: checks.length, checks, errors }, null, 2) + '\n');
    console.log(`${checks.length} pipeline model browser checks passed`);
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
