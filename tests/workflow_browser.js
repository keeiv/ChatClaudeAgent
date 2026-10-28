// Run through Playwright's browser_run_code tool with tools/serve_ui.js on port 5197.
async (page) => {
  const checks = [], errors = [];
  const check = (ok, label) => { if (!ok) throw Error(label); checks.push(label); };
  const onError = error => errors.push(error.message);
  page.on('pageerror', onError);
  try {
    await page.setViewportSize({ width: 1280, height: 900 });
    await page.emulateMedia({ reducedMotion: 'no-preference' });
    await page.goto('about:blank');  // A same-URL fragment navigation does not reload the fixture.
    await page.addInitScript(() => {
      Object.defineProperty(window, 'CCAWorkflow', { configurable: true, set(api) {
        const update = api.View.prototype.update;
        api.View.prototype.update = function (flow, options) {
          window.__workflowView = this;
          return update.call(this, flow, options);
        };
        Object.defineProperty(window, 'CCAWorkflow', { value: api, configurable: true, writable: true });
      } });
    });
    await page.goto('http://127.0.0.1:5197/#flow');
    await page.waitForFunction(() => document.querySelector('.wf-node[data-agent="claude"]')?.dataset.status === 'thinking');
    check(await page.locator('.wf-node').count() === 4, 'four invited members painted');
    check(await page.locator('.wf-node[data-agent="claude"] .wf-lamp').evaluate(n => getComputedStyle(n).animationName === 'wf-spin'), 'thinking runs actual CSS spinner');
    check(await page.locator('.wf-packets circle').count() === 0, 'thinking alone creates no transfer');
    await page.locator('.wf').screenshot({ path: 'build/workflow-review/browser-thinking.png' });
    await page.waitForFunction(() => document.querySelector('.wf-node[data-agent="local1"]')?.dataset.status === 'thinking');
    await page.waitForFunction(() => document.querySelector('.wf-packets circle'));
    const before = await page.locator('.wf-packets circle').first().evaluate(n => getComputedStyle(n).transform);
    await page.waitForFunction(value => {
      const circle = document.querySelector('.wf-packets circle');
      return circle && getComputedStyle(circle).transform !== value;
    }, before);
    check(await page.locator('.wf-packets circle').first().evaluate(n => n.getAnimations().some(a => a.playState === 'running')), 'handoff uses actual running Web Animation');
    await page.locator('.wf').screenshot({ path: 'build/workflow-review/browser-transfer.png' });
    await page.waitForFunction(() => document.querySelector('.wf-status')?.textContent === '協作完成');
    const painted = await page.locator('.wf').evaluate(root => {
      const graph = root.querySelector('.wf-graph').getBoundingClientRect();
      const boxes = [...root.querySelectorAll('.wf-node')].map(n => n.getBoundingClientRect());
      return {
        contained: boxes.every(b => b.left >= graph.left && b.right <= graph.right && b.top >= graph.top && b.bottom <= graph.bottom),
        apart: boxes.every((a, i) => boxes.every((b, j) => i === j || a.right <= b.left || b.right <= a.left || a.bottom <= b.top || b.bottom <= a.top)),
        allDone: [...root.querySelectorAll('.wf-node')].every(n => n.dataset.status === 'done'),
        graphVisible: getComputedStyle(root.querySelector('.wf-graph')).display !== 'none',
      };
    });
    check(painted.contained && painted.apart && painted.graphVisible, 'wide node boxes fit without overlapping');
    check(painted.allDone && await page.locator('.wf-packets circle').count() === 0, 'completion stops actual transfers');
    await page.screenshot({ path: 'build/workflow-review/browser-wide.png' });

    await page.locator('.wf').getByRole('button', { name: '收合', exact: true }).click();
    await page.waitForFunction(() => document.querySelector('.wf')?.dataset.mode === 'compact');
    check(await page.locator('.wf-graph').evaluate(n => getComputedStyle(n).display === 'none'), 'collapse control hides painted graph');
    await page.locator('.wf').getByRole('button', { name: '展開', exact: true }).click();
    await page.waitForFunction(() => document.querySelector('.wf')?.dataset.mode === 'auto');
    await page.setViewportSize({ width: 780, height: 780 });
    check(await page.locator('.wf-graph').evaluate(n => getComputedStyle(n).display === 'none'), 'narrow container uses text layout');
    check(await page.locator('.wf-compact').evaluate(n => getComputedStyle(n).display !== 'none' && n.textContent.includes('本地模型 2')), 'narrow text retains local member');
    const readable = await page.locator('.composer').evaluate(n => {
      const box = n.getBoundingClientRect();
      return box.top >= 0 && box.bottom <= innerHeight && box.height >= 40;
    });
    check(readable, 'narrow layout preserves input');
    check(await page.locator('.composer').evaluate(n => n.getBoundingClientRect().height <= 66), 'blank input does not expand after viewport changes');
    await page.locator('.composer textarea').fill('第一行\n第二行\n第三行');
    check(await page.locator('.composer').evaluate(n => n.getBoundingClientRect().height >= 88), 'multiline input still grows');
    await page.locator('.composer textarea').fill('');
    check(await page.locator('.composer').evaluate(n => n.getBoundingClientRect().height <= 66), 'clearing multiline input restores compact height');
    await page.screenshot({ path: 'build/workflow-review/browser-narrow.png' });
    await page.setViewportSize({ width: 1280, height: 620 });
    check(await page.locator('.wf-graph').evaluate(n => getComputedStyle(n).display === 'none'), 'short window uses text layout');
    await page.setViewportSize({ width: 1280, height: 900 });

    await page.evaluate(() => CCA_DEV.openConv('dm-claude'));
    await page.waitForFunction(() => document.querySelector('.wf')?.hidden);
    check(await page.locator('.wf').evaluate(n => n.getBoundingClientRect().height === 0), 'solo DM workflow occupies no painted space');
    check(await page.locator('.wf-node').count() === 0 && await page.locator('.wf-packets circle').count() === 0, 'solo DM has no ghost members or transfers');
    await page.screenshot({ path: 'build/workflow-dm-review/browser-dm.png' });
    await page.evaluate(() => CCA_DEV.openConv('g1'));
    await page.waitForFunction(() => document.querySelector('.wf') && !document.querySelector('.wf').hidden);
    check(await page.locator('.wf-node').count() === 4, 'returning from DM restores invited group members');

    await page.evaluate(() => {
      const view = window.__workflowView;
      window.__workflowFixture = {
        runId: 'browser-fixture', status: 'running', label: '思考中',
        nodes: [
          { id: 'claude', status: 'thinking', label: '思考中' },
          { id: 'codex', status: 'waiting', label: '待命' },
          { id: 'local1', status: 'waiting', label: '待命' },
        ], transfers: [],
      };
      view.update(window.__workflowFixture, view.options);
    });
    check(await page.locator('.wf-wire').nth(1).getAttribute('d').then(d => !d.includes('595')), 'single local has no phantom wire');
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await page.waitForFunction(() => document.querySelector('.wf')?.classList.contains('wf-still'));
    await page.waitForFunction(() => getComputedStyle(document.querySelector('.wf-node[data-agent="claude"] .wf-lamp')).animationName === 'none');
    check(await page.locator('.wf-node[data-agent="claude"] .wf-lamp').evaluate(n => getComputedStyle(n).animationName === 'none'), 'system reduced motion stops CSS spinner');
    check(await page.locator('.wf-node[data-agent="claude"]').evaluate(n => getComputedStyle(n, '::before').animationName === 'none'), 'system reduced motion stops breathing');
    await page.emulateMedia({ reducedMotion: 'no-preference' });
    await page.waitForFunction(() => !document.querySelector('.wf')?.classList.contains('wf-still'));
    for (const status of ['waitingUser', 'paused', 'failed']) {
      await page.evaluate(status => {
        const view = window.__workflowView, flow = window.__workflowFixture;
        flow.status = status; flow.nodes[0].status = status;
        flow.nodes[0].label = status;
        flow.transfers = [{ id: status, from: 'claude', to: 'codex', label: 'test dispatch', ts: Date.now() }];
        view.update(flow, view.options);
      }, status);
      check(await page.locator('.wf-packets circle').count() === 0, status + ' has no transfer');
      check(await page.locator('.wf-node[data-agent="claude"] .wf-lamp').evaluate(n => getComputedStyle(n).animationName === 'none'), status + ' has no spinner');
    }
    check(errors.length === 0, 'no JavaScript runtime errors');
    return { passed: checks.length, checks, screenshots: ['build/workflow-review/browser-thinking.png', 'build/workflow-review/browser-transfer.png', 'build/workflow-review/browser-wide.png', 'build/workflow-review/browser-narrow.png', 'build/workflow-dm-review/browser-dm.png'], backend: 'mock only; fixture states for motion and failures' };
  } finally { page.off('pageerror', onError); }
}
