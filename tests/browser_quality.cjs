// Real Edge interactions against a disposable local page, with no login or model inference.
const assert = require('node:assert/strict');
const http = require('node:http');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const readline = require('node:readline');
(async () => {
  const server = http.createServer((req, res) => { res.setHeader('Content-Type', 'text/html; charset=utf-8'); res.end('<title>Fixture</title><input id="name"><button id="go" onclick="document.getElementById(\'out\').textContent=document.getElementById(\'name\').value">Go</button><p id="out">Ready</p>'); });
  server.listen(0, '127.0.0.1'); await once(server, 'listening');
  const worker = spawn(process.execPath, ['tools/browser_worker.cjs'], { stdio: ['pipe', 'pipe', 'pipe'] });
  const output = readline.createInterface({ input: worker.stdout }), queue = [];
  output.on('line', text => queue.shift()?.(JSON.parse(text)));
  const call = value => new Promise(resolve => { queue.push(resolve); worker.stdin.write(JSON.stringify(value) + '\n'); });
  try {
    let r = await call({ operation: 'open', url: 'http://127.0.0.1:' + server.address().port });
    assert.equal(r.title, 'Fixture'); assert.ok(r.screenshot.startsWith('data:image/png;base64,'));
    r = await call({ operation: 'resize', width: 390 }); assert.equal(r.viewport.width, 390);
    r = await call({ operation: 'inspect', x: 30, y: 15 }); assert.ok(r.selected.tag);
    r = await call({ operation: 'verify', actions: [{ action: 'fill', selector: '#name', value: 'Verified' }, { action: 'click', selector: '#go' }, { action: 'expectText', selector: '#out', value: 'Verified' }] });
    assert.equal(r.passed, true); assert.equal(r.steps.length, 3);
    r = await call({ operation: 'verify', actions: [{ action: 'expectText', selector: '#out', value: 'Missing' }] }); assert.equal(r.passed, false);
    r = await call({ operation: 'verify', actions: [{ action: 'click', selector: '#go' }] }); assert.ok(r.error);
    r = await call({ operation: 'open', url: 'file:///C:/Windows/win.ini' }); assert.ok(r.error);
    r = await call({ operation: 'close' }); assert.equal(r.closed, true);
    console.log('10 real browser quality checks passed');
  } finally { worker.stdin.end(); await once(worker, 'exit'); server.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
