// A fresh Playwright context per conversation; no personal browser profile or arbitrary page evaluation from clients.
const readline = require('node:readline');
const { chromium } = require('playwright');
let browser, context, page, origin;
const logs = [];
const localUrl = value => {
  const url = new URL(value);
  if (url.username || url.password || !['http:', 'https:'].includes(url.protocol)) throw Error('網址格式不正確');
  if (url.protocol === 'http:' && !['127.0.0.1', 'localhost', '[::1]'].includes(url.hostname)) throw Error('HTTP 預覽僅限本機');
  return url;
};
async function start(url, width = 1280) {
  if (!browser) browser = await chromium.launch({ channel: 'msedge', headless: true });
  if (context) await context.close();
  context = await browser.newContext({ viewport: { width: Math.max(360, Math.min(1600, width)), height: 800 }, acceptDownloads: false });
  page = await context.newPage(); origin = url.origin;
  context.on('page', async other => { if (other !== page) await other.close(); });
  await context.route('**/*', route => {
    const req = route.request();
    if (req.isNavigationRequest() && req.frame() === page.mainFrame() && new URL(req.url()).origin !== origin) return route.abort();
    return route.continue();
  });
  page.on('console', m => { if (m.type() === 'error') { logs.push(m.text().slice(0, 1000)); if (logs.length > 20) logs.shift(); } });
  page.on('pageerror', e => { logs.push(e.message.slice(0, 1000)); if (logs.length > 20) logs.shift(); });
  await page.goto(url.href, { waitUntil: 'domcontentloaded', timeout: 20000 });
}
async function action(m) {
  const op = m.operation || 'open';
  if (op === 'close') {
    if (browser) await browser.close(); browser = context = page = null;
    return { closed: true };
  }
  if (op === 'open') { logs.length = 0; await start(localUrl(m.url), Number(m.width) || 1280); }
  if (!page) throw Error('請先開啟預覽');
  let selected;
  if (op === 'click' || op === 'inspect') {
    const x = Number(m.x), y = Number(m.y), size = page.viewportSize();
    if (!Number.isFinite(x) || !Number.isFinite(y) || x < 0 || y < 0 || x >= size.width || y >= size.height) throw Error('座標超出預覽');
    if (op === 'click') await page.mouse.click(x, y, { timeout: 5000 });
    else selected = await page.evaluate(({ x, y }) => {
      const el = document.elementFromPoint(x, y);
      if (!el) return null;
      const r = el.getBoundingClientRect();
      return { tag: el.tagName, id: el.id, text: (el.innerText || el.getAttribute('aria-label') || '').slice(0, 600),
        bounds: { x: r.x, y: r.y, width: r.width, height: r.height } };
    }, { x, y });
  }
  if (op === 'type') await page.keyboard.insertText(String(m.text || '').slice(0, 6000));
  if (op === 'key') {
    if (!['Enter', 'Tab', 'Escape', 'Backspace', 'ArrowDown', 'ArrowUp'].includes(m.key)) throw Error('不支援此按鍵');
    await page.keyboard.press(m.key);
  }
  if (op === 'scroll') await page.mouse.wheel(0, Math.max(-1600, Math.min(1600, Number(m.delta) || 600)));
  if (op === 'reload') await page.reload({ waitUntil: 'domcontentloaded' });
  if (op === 'resize') await page.setViewportSize({ width: Math.max(360, Math.min(1600, Number(m.width) || 1280)), height: 800 });
  let passed, steps;
  if (op === 'verify') {
    if (!Array.isArray(m.actions) || !m.actions.length || m.actions.length > 30 || !m.actions.some(s => s.action?.startsWith('expect'))) throw Error('驗收流程需含斷言，最多 30 步');
    steps = [];
    for (const step of m.actions) {
      try {
        if (!['click', 'fill', 'expectText', 'expectVisible', 'expectUrl'].includes(step.action)) throw Error('不支援的驗收動作');
        if (step.action === 'expectUrl') {
          if (!page.url().includes(String(step.value))) throw Error('網址未符合預期');
        } else {
          if (typeof step.selector !== 'string' || step.selector.length > 800) throw Error('需要有效的選取器');
          const el = page.locator(step.selector).first();
          if (step.action === 'click') await el.click({ timeout: 7000 });
          if (step.action === 'fill') await el.fill(String(step.value || ''), { timeout: 7000 });
          if (step.action === 'expectVisible') await el.waitFor({ state: 'visible', timeout: 7000 });
          if (step.action === 'expectText') {
            await el.waitFor({ state: 'visible', timeout: 7000 });
            const value = String(step.value || '');
            if (!value || !(await el.innerText()).includes(value)) throw Error('畫面文字未符合預期');
          }
        }
        steps.push({ action: step.action, selector: step.selector, passed: true });
      } catch (e) { steps.push({ action: step.action, selector: step.selector, passed: false, error: e.message.slice(0, 500) }); break; }
    }
    passed = steps.length === m.actions.length && steps.every(s => s.passed);
  }
  const png = await page.screenshot({ animations: 'disabled', timeout: 10000 });
  if (png.length > 2 * 1024 * 1024) throw Error('預覽圖片超過大小限制');
  return { url: page.url(), title: await page.title(), viewport: page.viewportSize(), selected, steps, passed,
    screenshot: 'data:image/png;base64,' + png.toString('base64'), consoleErrors: logs.slice(-20),
    note: '畫面與操作屬於指定網址；本機來源指紋不代表外部部署版本已經核對。',
    output: steps ? steps.map(s => `${s.passed ? 'PASS' : 'FAIL'} ${s.action} ${s.selector || ''} ${s.error || ''}`).join('\n') : '' };
}
let serial = Promise.resolve();
readline.createInterface({ input: process.stdin }).on('line', line => {
  serial = serial.then(async () => {
    try { process.stdout.write(JSON.stringify(await action(JSON.parse(line))) + '\n'); }
    catch (error) { process.stdout.write(JSON.stringify({ error: error.message.slice(0, 1500) }) + '\n'); }
  });
}).on('close', () => serial.finally(async () => { if (browser) await browser.close(); }));
