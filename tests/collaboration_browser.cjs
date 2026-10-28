// Exercise all commander workflows with the browser mock; native tests cover persistence and Git.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const { chromium } = require('playwright');

(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const page = await browser.newPage({ viewport: { width: 1440, height: 980 } });
  const errors = [], checks = [];
  const check = (ok, label) => { assert.ok(ok, label); checks.push(label); };
  page.on('pageerror', e => errors.push(e.message));
  try {
    await page.route('**/mock.js', route => route.fulfill({ contentType: 'text/javascript', body: fs.readFileSync('ui/mock.js', 'utf8') + `
      window.__collabSent = [];
      const receive = window.MockBackend.receive;
      window.MockBackend.receive = message => { window.__collabSent.push(structuredClone(message)); receive(message); };` }));
    await page.goto((process.env.CCA_UI_URL || 'http://127.0.0.1:5208/') + '#mock');
    await page.waitForFunction(() => window.CCA_DEV);
    await page.locator('.gate').waitFor({ state: 'hidden' });
    await page.evaluate(() => window.CCA_DEV.openConv('g1'));
    const dialog = () => page.getByRole('dialog').last();
    const open = async name => { await page.locator('#panel').getByRole('button', { name, exact: true }).click(); };
    const close = async () => { await page.keyboard.press('Escape'); };
    await open('計畫與驗收');
    await page.getByRole('textbox', { name: '計畫標題' }).fill('可靠的訂單驗證');
    await page.getByRole('textbox', { name: '工作步驟', exact: true }).fill('補齊空輸入處理');
    await page.getByRole('textbox', { name: '驗收條件', exact: true }).fill('測試空字串與多位元組輸入');
    await page.getByRole('button', { name: '儲存草稿', exact: true }).click();
    await page.getByRole('status').filter({ hasText: '已儲存版本 1' }).waitFor();
    check(await page.evaluate(() => !window.__collabSent.some(m => m.type === 'plan.execute')), 'saving a plan never executes it');
    await close(); await open('計畫與驗收');
    check(await page.getByRole('button', { name: '執行已核准計畫' }).isDisabled(), 'draft execution is disabled');
    await page.getByRole('button', { name: '核准此版本' }).click();
    await page.getByRole('button', { name: '執行已核准計畫' }).waitFor({ state: 'visible' });
    check(await page.getByRole('button', { name: '執行已核准計畫' }).isEnabled(), 'approval enables a separate execution action');
    await page.getByRole('textbox', { name: '計畫留言' }).fill('<script>not executable</script> 請補上邊界值');
    await page.getByRole('button', { name: '加入留言並退回待核准' }).click();
    await page.getByText('版本 2', { exact: true }).waitFor();
    check(await page.getByRole('button', { name: '執行已核准計畫' }).isDisabled(), 'new feedback invalidates approval');
    check(await page.locator('.artifact-comment script').count() === 0, 'plan comments render as text');
    await page.getByRole('button', { name: '核准此版本' }).click();
    await page.getByRole('checkbox', { name: '指揮官已驗收' }).check();
    await page.getByText('已驗收', { exact: true }).waitFor();
    check(await page.getByRole('checkbox', { name: '指揮官已驗收' }).isChecked(), 'acceptance is a commander action');
    await close();

    await open('多模型意見');
    await page.getByRole('textbox', { name: '意見模式問題' }).fill('如何提高輸入驗證的可靠性？');
    await page.getByRole('button', { name: '開始獨立蒐集' }).click();
    await page.locator('.opinion-summary').waitFor();
    check(await page.getByRole('heading', { name: '分歧與待驗證項目' }).isVisible(), 'consensus and disagreements have separate sections');
    check(await page.locator('.opinion-results .card').count() >= 2, 'individual model replies remain available');
    check(await dialog().evaluate(root => ![...root.querySelectorAll('*')].some(el =>
      [...el.childNodes].some(n => n.nodeType === Node.TEXT_NODE && n.textContent.trim() === 'null'))),
    'optional artifact sections never render null as visible text');
    await page.screenshot({ path: 'build/ui-shots/opinions-after.png', animations: 'disabled' });
    await page.getByRole('button', { name: '將提案存成待核准計畫' }).click();
    check(await page.evaluate(() => !window.__collabSent.some(m => m.type === 'plan.execute')), 'an opinion proposal cannot start implementation automatically');
    await close(); await open('計畫與驗收');
    check(await page.getByRole('button', { name: '執行已核准計畫' }).isDisabled(), 'adopted proposal still needs approval');
    await close();

    await open('審查關注點');
    await page.getByRole('combobox', { name: 'GPT 審查關注點' }).selectOption('security');
    await page.getByRole('combobox', { name: '編輯關注點' }).selectOption('');
    await page.getByRole('textbox', { name: '關注點名稱' }).fill('資料一致性');
    await page.getByRole('textbox', { name: '關注點 Markdown' }).fill('檢查交易失敗、重試與重複寫入。');
    await page.getByRole('button', { name: '儲存關注點' }).click();
    await close(); await open('審查關注點');
    check(await page.getByRole('combobox', { name: 'GPT 審查關注點' }).inputValue() === 'security', 'review assignment persists through reopening');
    check(await page.getByRole('combobox', { name: '編輯關注點' }).locator('option').allTextContents().then(names => names.includes('資料一致性')), 'custom Markdown review profiles are editable and saved');
    await close();

    await open('隔離工作區');
    await page.getByRole('button', { name: '建立隔離 worktree' }).click();
    await dialog().getByText('cca/mock-task', { exact: true }).waitFor();
    check(await page.getByRole('button', { name: '合併已提交變更' }).isDisabled(), 'merging requires a preview');
    await page.getByRole('button', { name: '檢查能否合併' }).click();
    await page.getByText('可無衝突合併', { exact: true }).waitFor();
    await page.getByRole('button', { name: '合併已提交變更' }).click();
    check(await dialog().getByRole('heading', { name: '合併任務變更' }).isVisible(), 'merge confirmation identifies the target');
    await page.getByRole('button', { name: '確認合併' }).click();
    await page.getByText('已合併', { exact: true }).waitFor();
    await page.screenshot({ path: 'build/ui-shots/workspace-after.png', animations: 'disabled' });
    await close();

    await page.locator('#rail').getByRole('button', { name: '私人訊息', exact: true }).click();
    await page.locator('#sidebar').getByRole('button', { name: /^收件匣/ }).click();
    await page.getByText('指揮官收件匣', { exact: true }).waitFor();
    check(await page.locator('.inbox-item.needs-action').count() > 0, 'inbox shows plans requiring a decision across conversations');
    check(await page.locator('.inbox-item').count() > 1, 'completed work is included alongside required actions');
    await page.screenshot({ path: 'build/ui-shots/inbox-after.png', animations: 'disabled' });
    await page.locator('.inbox-item.needs-action').filter({ hasText: '計畫待處理' }).getByRole('button', { name: '開啟對話' }).first().click();
    await page.getByRole('dialog', { name: '計畫與驗收' }).waitFor();
    await page.setViewportSize({ width: 980, height: 680 });
    const bounds = await dialog().boundingBox();
    check(bounds.x >= 0 && bounds.x + bounds.width <= 980 && bounds.y + bounds.height <= 680, 'artifact controls fit a narrow desktop window');
    await page.screenshot({ path: 'build/ui-shots/plan-after.png', animations: 'disabled' });
    check(errors.length === 0, 'no browser script errors: ' + errors.join('; '));
    console.log(checks.length + ' collaboration browser checks passed');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
