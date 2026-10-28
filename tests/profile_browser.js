// Run this function with the Playwright browser test tool against tools/serve_ui.js on port 5198.
async (page) => {
  const checks = [], errors = [];
  page.on('pageerror', error => errors.push(error.message));
  const check = (ok, label) => { if (!ok) throw new Error(label); checks.push(label); };
  // Keep upload in the test DOM so the MCP runner does not pause at an OS chooser.
  await page.addInitScript(() => {
    const click = HTMLInputElement.prototype.click;
    HTMLInputElement.prototype.click = function () { if (this.type !== 'file') click.call(this); };
  });
  await page.setViewportSize({ width: 1360, height: 900 });
  await page.goto('about:blank');
  await page.goto('http://127.0.0.1:5198/#mock');
  await page.waitForFunction(() => !!window.CCA_DEV);
  const fixture = {
    name: 'crop.png', mimeType: 'image/png',
    buffer: Buffer.from(await page.evaluate(() => {
      const canvas = document.createElement('canvas'); canvas.width = 600; canvas.height = 300;
      const ctx = canvas.getContext('2d');
      ['#ff0000', '#00ff00', '#0000ff'].forEach((color, i) => { ctx.fillStyle = color; ctx.fillRect(i * 200, 0, 200, 300); });
      return canvas.toDataURL('image/png').split(',')[1];
    }), 'base64'),
  };
  const card = () => page.locator('.modal').last();
  const member = name => page.locator('#panel').getByRole('button', { name: '查看 ' + name + ' 的資料卡', exact: true });
  const openCard = async name => {
    await member(name).click();
    await page.locator('.user-popout .up-full').click();
    return await card().innerText();
  };
  const closeCard = async () => { await page.keyboard.press('Escape'); await card().waitFor({ state: 'detached' }); };
  check((await openCard('Claude')).includes('2026-09-22'), 'Claude uses the selected model release date');
  check((await card().innerText()).includes('關於我') && !(await card().innerText()).includes('性格：'), 'neutral profile uses the default role introduction');
  check((await card().locator('.profile-source').getAttribute('href')).startsWith('https://www.anthropic.com/'), 'date links to its primary source');
  await page.waitForFunction(() => !!document.activeElement.closest('.modal'));
  const firstFocus = card().locator('button:not([disabled]), a[href], input, select, textarea, [tabindex="0"]').first();
  await firstFocus.focus();
  await page.keyboard.press('Shift+Tab');
  check(await card().evaluate(el => el.contains(document.activeElement)) && !await firstFocus.evaluate(el => el === document.activeElement), 'profile keyboard focus wraps inside its dialog');
  await page.keyboard.press('Tab');
  check(await firstFocus.evaluate(el => el === document.activeElement), 'forward Tab also stays within the profile dialog');
  await page.screenshot({ path: 'build/profile-review/profile-card.png', animations: 'disabled' });
  await closeCard();
  check((await openCard('GPT')).includes('2026-09-29'), 'GPT default resolves from backend model metadata');
  await closeCard();
  check((await openCard('本地模型 1')).includes('2025-04-29'), 'local model tag resolves to the known release');
  await closeCard();
  await page.evaluate(() => window.MockBackend.receive({ type: 'conv.settings', id: 'g1', settings: { codexModel: 'unlisted-model' } }));
  check((await openCard('GPT')).includes('未收錄'), 'unknown model never receives an invented date');
  await closeCard();
  await page.evaluate(() => window.MockBackend.receive({ type: 'conv.settings', id: 'g1', settings: { codexModel: '' } }));
  const choose = async (button, file = fixture) => {
    await button.click(); await page.locator('input[type="file"]').setInputFiles(file);
    await page.getByRole('heading', { name: '裁切頭像', exact: true }).waitFor();
  };
  const ready = async () => { await page.getByRole('button', { name: '儲存頭像', exact: true }).waitFor();
    await page.waitForFunction(() => ![...document.querySelectorAll('.modal button')].find(b => b.textContent === '儲存頭像')?.disabled); };
  const center = async () => page.locator('.crop-stage canvas').evaluate(canvas => Array.from(canvas.getContext('2d').getImageData(256, 256, 1, 1).data));
  const savedImage = async selector => page.locator(selector).evaluate(async img => {
    await img.decode(); const canvas = document.createElement('canvas'); canvas.width = canvas.height = 512;
    canvas.getContext('2d').drawImage(img, 0, 0, 512, 512);
    return { width: img.naturalWidth, height: img.naturalHeight, src: img.src,
      pixel: Array.from(canvas.getContext('2d').getImageData(256, 256, 1, 1).data) };
  });
  await page.evaluate(() => window.CCA_DEV.openSettings('me'));
  await choose(page.getByRole('button', { name: '更換頭像', exact: true })); await ready();
  check((await center()).join() === '0,255,0,255', 'crop initially centers without stretching');
  await page.getByRole('slider', { name: '縮放圖片' }).fill('2');
  const bounds = await page.locator('.crop-stage canvas').boundingBox();
  await page.mouse.move(bounds.x + bounds.width * 0.9, bounds.y + bounds.height / 2);
  await page.mouse.down(); await page.mouse.move(bounds.x + 10, bounds.y + bounds.height / 2, { steps: 8 }); await page.mouse.up();
  check((await center()).join() === '0,0,255,255', 'zoom and pointer drag select a different image region');
  await page.screenshot({ path: 'build/profile-review/crop-editor.png', animations: 'disabled' });
  await page.getByRole('button', { name: '儲存頭像', exact: true }).click();
  await page.locator('.crop-stage').waitFor({ state: 'detached' });
  const user = await savedImage('.preview-card .avatar-img');
  check(user.width === 512 && user.height === 512 && user.pixel.join() === '0,0,255,255', 'saving exports the actual cropped pixels at 512 square');
  await choose(page.getByRole('button', { name: '更換頭像', exact: true })); await ready();
  await page.keyboard.press('Escape');
  check((await savedImage('.preview-card .avatar-img')).src === user.src, 'cancel replacement preserves the existing user avatar');
  check(await page.locator('.settings-layer').count() > 0, 'Escape on crop leaves the settings underneath open');
  await page.evaluate(() => window.CCA_DEV.openSettings('claude'));
  await choose(page.getByRole('button', { name: '更換頭像', exact: true })); await ready();
  await page.locator('.crop-stage canvas').focus();
  await page.keyboard.press('Shift+ArrowRight');
  await page.getByRole('button', { name: '儲存頭像', exact: true }).click();
  await page.locator('.crop-stage').waitFor({ state: 'detached' });
  check((await savedImage('.preview-card .avatar-img')).width === 512, 'AI avatar also uses the crop workflow');
  await page.getByLabel('自訂加入日期').fill('2024-02-29');
  await page.getByLabel('自訂加入日期').dispatchEvent('change');
  await page.getByRole('button', { name: '關閉設定', exact: true }).click();
  check((await openCard('Claude')).includes('2024-02-29') && await card().locator('.profile-source').count() === 0, 'custom date is displayed without a misleading source');
  await closeCard();
  await page.evaluate(() => window.CCA_DEV.openSettings('claude'));
  await page.getByLabel('自訂加入日期').fill(''); await page.getByLabel('自訂加入日期').dispatchEvent('change');
  await page.locator('.persona-card').filter({ hasText: '無性格' }).click();
  await page.getByRole('button', { name: '關閉設定', exact: true }).click();
  check((await openCard('Claude')).includes('協助撰寫、審查與執行專案工作。'), 'removing persona updates the introduction');
  check((await card().innerText()).includes('2026-09-22'), 'clearing custom date restores model date');
  await closeCard();
  await page.locator('.chat-head button.title').click();
  await choose(page.getByRole('button', { name: '更換群組頭像', exact: true })); await ready();
  await page.getByRole('button', { name: '儲存頭像', exact: true }).click();
  await page.locator('.crop-stage').waitFor({ state: 'detached' });
  const group = await savedImage('.group-photo-preview .avatar-img');
  check(group.width === 512 && await page.getByRole('button', { name: '移除群組頭像', exact: true }).isVisible(), 'group avatar preview updates after crop save');
  await card().getByRole('button', { name: '儲存名稱', exact: true }).click();
  check((await savedImage('.chat-head .title .avatar-img')).src === group.src, 'chat header reflects the custom group avatar');
  await page.locator('.chat-head button.title').click();
  await choose(page.getByRole('button', { name: '更換群組頭像', exact: true })); await ready();
  await page.keyboard.press('Escape');
  check(await page.getByRole('heading', { name: '群組資料', exact: true }).isVisible() && (await savedImage('.group-photo-preview .avatar-img')).src === group.src, 'nested crop cancellation preserves group editor and saved avatar');
  await page.getByRole('button', { name: '移除群組頭像', exact: true }).click();
  check(await page.locator('.group-photo-preview > .group-avatar').count() === 1, 'group can restore its original paired avatar');
  await card().getByRole('button', { name: '關閉', exact: true }).click();
  await page.evaluate(() => window.CCA_DEV.openSettings('me'));
  await page.setViewportSize({ width: 620, height: 720 });
  await choose(page.getByRole('button', { name: '更換頭像', exact: true })); await ready();
  const crop = await card().boundingBox();
  check(crop.x >= 0 && crop.y >= 0 && crop.x + crop.width <= 621 && crop.y + crop.height <= 721, 'crop dialog fits a narrow window');
  await page.screenshot({ path: 'build/profile-review/crop-narrow.png', animations: 'disabled' });
  await page.keyboard.press('Escape');
  await choose(page.getByRole('button', { name: '更換頭像', exact: true }), { name: 'bad.png', mimeType: 'image/png', buffer: Buffer.from('broken image') });
  await page.locator('.crop-error').filter({ hasText: '讀不到圖片' }).waitFor();
  check(await page.getByRole('button', { name: '儲存頭像', exact: true }).isDisabled(), 'unreadable image cannot be saved');
  await page.getByRole('button', { name: '取消', exact: true }).click();
  check((await savedImage('.preview-card .avatar-img')).src === user.src, 'bad image cancellation also keeps the original');
  // Reproduce the two desktop origins; data: mock uploads do not exercise canvas CORS.
  const source = await (await page.request.get('http://127.0.0.1:5198/profile_media.js')).text();
  await page.route('https://cca.app/**', route => route.fulfill({ status: 200,
    contentType: route.request().url().endsWith('.js') ? 'text/javascript' : 'text/html',
    body: route.request().url().endsWith('.js') ? source : '<!doctype html><script src="/profile_media.js"></script><div id="crop"></div>' }));
  await page.route('https://media.cca.app/fixture.png', route => route.fulfill({ status: 200, contentType: 'image/png',
    headers: { 'Access-Control-Allow-Origin': 'https://cca.app' }, body: fixture.buffer }));
  await page.goto('https://cca.app/profile-test');
  const crossOrigin = await page.evaluate(() => new Promise(resolve => {
    const h = (tag, props, ...children) => {
      const el = document.createElement(tag);
      for (const [key, value] of Object.entries(props || {})) el.setAttribute(key, value);
      el.append(...children); return el;
    };
    const editor = new window.CCAProfileMedia.Cropper(document.querySelector('#crop'), h, 'https://media.cca.app/fixture.png',
      () => { const data = editor.png(); editor.destroy(); resolve(data.startsWith('data:image/png;base64,')); }, () => resolve(false));
  }));
  check(crossOrigin, 'desktop cross-origin media can be cropped and exported with its CORS policy');
  check(errors.length === 0, 'no browser script errors: ' + errors.join('; '));
  return { passed: checks.length, checks, errors };
}
