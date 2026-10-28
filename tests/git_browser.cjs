// Version-control pane and quota resume settings in real Edge against the mock host; no Git or model is run.
const { chromium } = require('playwright');
const fs = require('fs');
const assert = require('assert');

(async () => {
  let checks = 0;
  const check = (ok, label) => { assert.ok(ok, label); checks++; };
  const browser = await chromium.launch({ channel: 'msedge' });
  const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  const errors = [];
  page.on('pageerror', e => errors.push(String(e)));
  await page.addInitScript(() => {
    window.gitRequests = [];
    const watch = () => {
      const mock = window.MockBackend;
      if (!mock) return setTimeout(watch, 5);
      const receive = mock.receive;
      mock.receive = message => { window.gitRequests.push(structuredClone(message)); return receive(message); };
    };
    watch();
  });
  fs.mkdirSync('build/git-review', { recursive: true });
  await page.goto((process.env.CCA_UI_URL || 'http://127.0.0.1:5221/') + '#mock,quota');
  await page.waitForSelector('.msg');
  await page.getByRole('button', { name: '版本控制', exact: true }).first().click();
  // A signed-out remote and a missing author open the sign-in page before any file is offered.
  await page.waitForSelector('.git-login');
  check(await page.getByRole('heading', { name: '需要登入 github.com' }).isVisible(), 'signed-out remote opens the sign-in page first');
  check(await page.locator('.git-tile').count() === 0 && await page.locator('.git-pane input[type=password]').count() === 0,
    'no file tiles and no password field before signing in');
  await page.screenshot({ path: 'build/git-review/login.png' });
  await page.getByRole('button', { name: '使用瀏覽器登入 github.com' }).click();
  await page.waitForSelector('.git-login-card.done');
  check(await page.locator('.git-login-card.done').first().innerText().then(t => t.includes('github.com 已登入')), 'sign-in result is shown');
  check(await page.evaluate(() => window.gitRequests.some(m => m.type === 'git.login' && m.setupHelper === false)),
    'sign-in goes through the host without enabling a helper that already exists');
  await page.getByLabel('名稱', { exact: true }).fill('Finn');
  await page.getByLabel('e-mail', { exact: true }).fill('user@example.com');
  await page.getByRole('button', { name: '保存提交身分' }).click();
  await page.waitForSelector('.git-tile');
  check(await page.evaluate(() => window.mockGitIdentity?.email === 'user@example.com' && window.mockGitIdentity.global === true),
    'author is saved for every repository by default');

  const tone = path => page.locator(`.git-tile-main[data-path="${path}"]`).evaluate(el => {
    const tile = el.closest('.git-tile'), css = getComputedStyle(document.documentElement);
    const want = v => css.getPropertyValue(v).trim();
    const border = getComputedStyle(tile).getPropertyValue('--tone').trim();
    return { border, yellow: want('--yellow'), green: want('--green'), red: want('--red'), cls: tile.className };
  });
  const changed = await tone('src/cart.ts'), added = await tone('src/discount.ts'), risky = await tone('build/shop.exe');
  check(changed.cls.includes('tone-changed') && changed.border === changed.yellow, 'modified file is a yellow tile');
  check(added.cls.includes('tone-new') && added.border === added.green, 'new file is a green tile');
  check(risky.cls.includes('tone-risk') && risky.border === risky.red, 'not-recommended file is a red tile');
  check(await page.locator('.git-tile.locked .git-tile-main[data-path=".env"]').count() === 1, '.env is locked by a rule');
  const pressed = path => page.locator(`.git-tile-main[data-path="${path}"]`).getAttribute('aria-pressed');
  check(await pressed('src/cart.ts') === 'true' && await pressed('build/shop.exe') === 'false', 'only ordinary changes are preselected');
  await page.locator('.git-tile-main[data-path=".env"]').click();
  check(await pressed('.env') === 'false', 'a locked tile cannot be selected');
  await page.locator('.git-tile-main[data-path="build/shop.exe"]').click();
  check(await page.getByText('這個檔案不建議提交', { exact: true }).isVisible(), 'a red tile asks before it is selected');
  await page.getByRole('button', { name: '仍要提交', exact: true }).click();
  check(await pressed('build/shop.exe') === 'true', 'confirmed red tile is selected');
  await page.locator('.git-tile-main[data-path="build/shop.exe"]').click();
  check(await pressed('build/shop.exe') === 'false', 'clicking again deselects');
  await page.screenshot({ path: 'build/git-review/pane.png' });

  await page.getByRole('button', { name: '依任務紀錄草擬' }).click();
  check((await page.locator('.git-message').inputValue()).startsWith('幫購物車加上折扣碼功能'), 'draft uses the task request without a model call');
  await page.getByRole('button', { name: '提交', exact: true }).click();
  await page.waitForSelector('.git-secrets');
  check(await page.locator('.git-secrets').innerText().then(t => t.includes('config/keys.js') && t.includes('第 2 行')), 'secret scan names file and line');
  const firstCommit = await page.evaluate(() => window.gitRequests.filter(m => m.type === 'git.commit').pop());
  check(!firstCommit.paths.includes('.env') && !firstCommit.paths.includes('build/shop.exe') && firstCommit.task === true,
    'commit request carries only chosen files and the task trailer flag');
  await page.screenshot({ path: 'build/git-review/secrets.png' });
  await page.getByRole('button', { name: '確認不是金鑰，仍要提交' }).click();
  await page.waitForFunction(() => document.querySelector('.git-row .git-subject')?.textContent.startsWith('幫購物車'));
  check(await page.evaluate(() => window.gitRequests.filter(m => m.type === 'git.commit').pop().allowSecrets === true), 'confirmation resends with allowSecrets');
  check(await page.locator('.git-row').first().locator('.git-ref.task').count() === 1, 'commit with a task shows the CCA badge in history');
  check(await page.locator('.git-tile-main[data-path="src/cart.ts"]').count() === 0, 'committed files leave the tile list');

  await page.locator('.git-sync input[type=checkbox]').first().check();
  check(await page.evaluate(() => window.gitRequests.some(m => m.type === 'git.auto' && m.fetch === true)), 'auto fetch uses the host protocol');
  await page.locator('.git-sync input[type=checkbox]').nth(1).check();
  check(await page.evaluate(() => window.gitRequests.some(m => m.type === 'git.auto' && m.pull === true)), 'auto pull uses the host protocol');

  await page.locator('.git-tile-main[data-path="build/shop.exe"]').hover();
  await page.getByRole('button', { name: '永遠不提交 build/shop.exe' }).click();
  await page.waitForSelector('.git-tile.locked .git-tile-main[data-path="build/shop.exe"]');
  check(true, 'a tile can be added to the never-commit rules');
  await page.evaluate(() => window.CCA_DEV.openSettings('git'));
  await page.getByRole('button', { name: '移除規則 build/shop.exe' }).click();
  await page.waitForFunction(() => !document.querySelector('[aria-label="移除規則 build/shop.exe"]'));
  check(true, 'rule editor removes a rule');
  await page.getByPlaceholder('例如 .env、*.pem、secrets/、docs/private').fill('docs/private/');
  await page.getByRole('button', { name: '加入', exact: true }).click();
  await page.waitForSelector('[aria-label="移除規則 docs/private/"]');
  check(true, 'rule editor adds a rule');
  await page.screenshot({ path: 'build/git-review/rules.png' });

  await page.evaluate(() => window.CCA_DEV.openSettings('resume'));
  await page.getByRole('checkbox', { name: '額度恢復後自動接續任務' }).check();
  await page.locator('#resume-delay').fill('7');
  await page.locator('#resume-delay').press('Tab');
  const resume = await page.evaluate(() => window.gitRequests.filter(m => m.type === 'autoResume.set').pop());
  check(resume.settings.enabled === true && resume.settings.delayMinutes === 7, 'resume settings save through the host');
  await page.getByRole('checkbox', { name: '只在指定時段接續' }).check();
  check(await page.evaluate(() => window.gitRequests.filter(m => m.type === 'autoResume.set').pop().settings.window.enabled), 'time window is saved');
  await page.screenshot({ path: 'build/git-review/resume.png' });
  await page.keyboard.press('Escape');
  check(await page.locator('#panel').evaluate(el => el.classList.contains('collapsed')), 'the member panel yields its space to the pane');
  await page.getByRole('button', { name: '關閉版本控制' }).click();
  await page.getByRole('button', { name: '排定額度恢復後自動接續' }).click();
  await page.waitForSelector('.resume-plan');
  check(await page.locator('.resume-plan').innerText().then(t => t.includes('自動接續')), 'task card shows the schedule');
  await page.getByRole('button', { name: '取消自動接續' }).click();
  await page.waitForFunction(() => !document.querySelector('.resume-plan'));
  check(true, 'a scheduled resume can be cancelled');
  await page.setViewportSize({ width: 1000, height: 700 });
  await page.evaluate(() => window.CCA_DEV.openGit('g1'));
  await page.waitForSelector('.git-tile');
  check(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'narrow window has no horizontal overflow');
  await page.screenshot({ path: 'build/git-review/narrow.png' });
  check(errors.length === 0, errors.join('\n'));
  console.log(`${checks} version-control browser checks passed`);
  await browser.close();
})().catch(e => { console.error(e); process.exit(1); });
