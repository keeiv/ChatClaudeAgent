// Real Edge interactions for default ownership, display transactions, focus and responsive layouts.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');

(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const out = path.resolve('build/badges');
  fs.mkdirSync(out, { recursive: true });
  let checks = 0;
  const errors = [];
  const check = (ok, label) => { assert.ok(ok, label); checks++; };
  const url = process.env.CCA_UI_URL || 'http://127.0.0.1:5216/';
  const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
  page.on('pageerror', error => errors.push(error.message));
  const settings = async flags => {
    await page.goto(url + '?badge-test=' + Date.now() + '#' + flags);
    await page.getByRole('button', { name: '開啟個人資料', exact: true }).waitFor();
    await page.waitForTimeout(900);
    await page.evaluate(() => window.CCA_DEV.openSettings('me'));
    await page.getByRole('button', { name: '管理徽章', exact: true }).waitFor();
  };
  const open = async () => {
    await page.getByRole('button', { name: '管理徽章', exact: true }).click();
    await page.locator('.badge-modal').waitFor();
    await page.waitForTimeout(160);
  };
  const entry = id => page.locator(`[data-badge-entry="${id}"]`);
  try {
    await settings('');
    check(await page.locator('.preview-card [data-badge]').count() === 1, 'fresh install only displays membership');
    check(await page.locator('.preview-card [data-badge]').getAttribute('data-badge') === 'member', 'membership is always built in');
    await open();
    check(await page.locator('.badge-entry').count() === 7, 'directory lists all seven original badge designs');
    check(await page.locator('.badge-entry.unowned').count() === 6, 'other badges are not auto-awarded to fresh installations');
    await entry('developer').click();
    check(await page.getByRole('button', { name: '隱藏徽章', exact: true }).count() === 0, 'unowned badge cannot be displayed or granted');
    await page.keyboard.press('Escape');
    await page.locator('.settings-content input.input').first().fill('developer');
    await page.waitForTimeout(600);
    check(await page.locator('.preview-card [data-badge]').count() === 1, 'renaming to developer does not grant developer badges');

    await settings('badges');
    check(await page.locator('.preview-card [data-badge]').count() === 6, 'profile preview respects six display slots');
    check((await page.locator('.preview-card .badge-overflow').textContent()) === '+1', 'seventh badge remains in the directory');
    await open();
    check(await page.locator('.badge-entry.unowned').count() === 0, 'local grants own all seven');
    check(await page.locator('.badge-modal').evaluate(el => el.clientWidth === 740), 'directory uses the intended wide layout');
    await page.screenshot({ path: path.join(out, 'directory.png') });
    await entry('member').click();
    await page.getByRole('button', { name: '徽章向前移', exact: true }).click();
    check(await page.locator(':focus').getAttribute('data-badge-entry') === 'member', 'reorder preserves a usable keyboard focus');
    check((await entry('member').textContent()).includes('展示中'), 'moving the seventh badge forward makes it visible');
    await page.getByRole('button', { name: '隱藏徽章', exact: true }).click();
    check((await entry('member').textContent()).includes('已隱藏'), 'hide changes the preview without removing ownership');
    await page.getByRole('button', { name: '儲存顯示設定', exact: true }).click();
    await page.locator('.badge-modal').waitFor({ state: 'detached' });
    check(await page.locator(':focus').getAttribute('data-manage-badges') !== null, 'save restores focus after host rerenders the profile');
    check(await page.locator('.preview-card [data-badge="member"]').count() === 0, 'saved hidden badge is absent from the profile');
    await open();
    check((await entry('member').textContent()).includes('已隱藏'), 'reopening retains the host-confirmed preferences');
    await entry('member').click();
    await page.getByRole('button', { name: '顯示徽章', exact: true }).click();
    await page.keyboard.press('Escape');
    await open();
    check((await entry('member').textContent()).includes('已隱藏'), 'cancel discards changes to visibility');
    await page.setViewportSize({ width: 520, height: 800 });
    await page.waitForTimeout(100);
    check(await page.locator('.badge-modal').evaluate(el => el.scrollWidth <= el.clientWidth), 'narrow directory does not overflow horizontally');
    check(await page.locator('.badge-directory').evaluate(el => getComputedStyle(el).gridTemplateColumns.split(' ').length === 1), 'narrow layout stacks the details below the list');
    await page.screenshot({ path: path.join(out, 'directory-narrow.png') });
    await page.setViewportSize({ width: 360, height: 760 });
    check(await page.locator('.badge-modal').evaluate(el => el.scrollWidth <= el.clientWidth), 'directory also fits a 360 px viewport');
    await page.setViewportSize({ width: 1440, height: 1000 });
    await page.keyboard.press('Escape');
    await page.keyboard.press('Escape');
    await page.getByRole('button', { name: '開啟個人資料', exact: true }).click();
    check(await page.locator('.user-popout .profile-badges').count() === 1, 'local userbar opens profile with badge row');
    await page.waitForTimeout(180);
    await page.screenshot({ path: path.join(out, 'profile-popout.png') });
    await page.getByRole('button', { name: '檢視完整個人資料', exact: true }).last().click();
    check(await page.locator('.full-profile .profile-badges').count() === 1, 'full profile also shows badges near the name');
    await page.waitForTimeout(180);
    await page.screenshot({ path: path.join(out, 'profile-full.png') });
    await page.keyboard.press('Escape');

    await settings('badges');
    await open();
    for (const id of ['developer', 'community-developer', 'origin', 'familiar', 'present', 'pioneer', 'member']) {
      await entry(id).click();
      await page.getByRole('button', { name: '隱藏徽章', exact: true }).click();
    }
    check(await page.locator('.badge-slot-preview [data-badge]').count() === 0, 'all badges can be hidden without empty unusable profile controls');
    check(await page.locator('.badge-slot-preview .badge-overflow').count() === 1, 'empty row retains a directory entry point');
    await page.getByRole('button', { name: '儲存顯示設定', exact: true }).click();
    await open();
    check((await page.locator('.badge-summary').textContent()).includes('7／7'), 'hiding all badges retains ownership');
    await page.keyboard.press('Escape');

    await settings('badges,badge-fail');
    await open();
    await entry('developer').click();
    await page.getByRole('button', { name: '隱藏徽章', exact: true }).click();
    await page.getByRole('button', { name: '儲存顯示設定', exact: true }).click();
    check((await page.locator('.badge-error').textContent()).includes('原設定已保留'), 'failed host save remains open with an explicit error');
    check(await page.getByRole('button', { name: '重試儲存', exact: true }).isEnabled(), 'failed save can be retried');
    check((await entry('developer').textContent()).includes('已隱藏'), 'failed save preserves the unsaved draft for retry');
    await page.keyboard.press('Escape');
    check(await page.locator('.preview-card [data-badge="developer"]').count() === 1, 'failure does not mutate the saved profile');
    check(errors.length === 0, 'no browser runtime errors');
    fs.writeFileSync(path.join(out, 'browser-results.json'), JSON.stringify({ checks, errors }, null, 2) + '\n');
    console.log(`${checks} badge browser checks passed`);
  } finally {
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
