// Execute the actual desktop scripts against a DOM, with deterministic mock time.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const { parseHTML } = require('../build/workflow-qa/node_modules/linkedom');
const { window } = parseHTML(fs.readFileSync('ui/index.html', 'utf8'));
const document = window.document;
window.location = new URL('http://localhost/#flow');
window.innerWidth = 1400; window.innerHeight = 1000;
// linkedom omits the browser's HTMLSelectElement.value setter.
const selectValue = Object.getOwnPropertyDescriptor(window.HTMLSelectElement.prototype, 'value');
Object.defineProperty(window.HTMLSelectElement.prototype, 'value', { get: selectValue.get, set(value) {
  for (const option of this.options) option.selected = option.value === value;
} });
const timers = new Map(); let now = Date.now(), seq = 0, animations = 0;
function later(fn, ms = 0) { const id = ++seq; timers.set(id, { at: now + ms, fn }); return id; }
function advance(ms) {
  const end = now + ms;
  for (let n = 0; n < 20000; n++) {
    const first = [...timers].sort((a, b) => a[1].at - b[1].at)[0];
    if (!first || first[1].at > end) { now = end; return; }
    now = first[1].at; timers.delete(first[0]); first[1].fn();
  }
  throw Error('runaway timer queue');
}
const motionListeners = new Set();
window.Element.prototype.animate = function () {
  animations++;
  const result = { cancel() { timers.delete(result.timer); } };
  result.timer = later(() => result.onfinish?.(), 1300);
  return result;
};
window.Element.prototype.scrollIntoView = function () {};
window.Element.prototype.getBoundingClientRect = function () { return { left: 0, right: 100, top: 0, bottom: 20, width: 100, height: 20 }; };
const env = {
  window, document, Node: window.Node, Element: window.Element, console, URL, location: window.location,
  navigator: { clipboard: {} }, CSS: { escape: x => x },
  Date: class extends Date { static now() { return now; } },
  setTimeout: later, clearTimeout: id => timers.delete(id), setInterval: () => 0,
  requestAnimationFrame: fn => later(fn, 16),
  ResizeObserver: class { observe() {} disconnect() {} },
  MutationObserver: class { observe() {} disconnect() {} },
  matchMedia: () => ({ matches: false, addEventListener: (_, fn) => motionListeners.add(fn), removeEventListener: (_, fn) => motionListeners.delete(fn) }),
};
vm.createContext(env);
for (const file of ['icons.js', 'md.js', 'workflow.js', 'profile_media.js', 'platform.js', 'collaboration.js', 'quality.js', 'activity.js', 'desk.js', 'git.js', 'theme_catalog.js', 'appearance.js', 'extensions.js', 'mock.js']) {
  vm.runInContext(fs.readFileSync('ui/' + file, 'utf8'), env, { filename: file });
  for (const key of ['Icons', 'MD', 'CCAWorkflow']) if (window[key]) env[key] = window[key];
}
const events = [];
const connect = window.MockBackend.connect;
window.MockBackend.connect = fn => connect(event => { events.push(JSON.parse(JSON.stringify(event))); fn(event); });
vm.runInContext(fs.readFileSync('ui/app.js', 'utf8'), env, { filename: 'app.js' });
let checks = 0;
function check(condition, description) { assert.ok(condition, description); checks++; }
advance(1000);
check(document.querySelectorAll('#main .wf-node').length === 4, 'desktop boot mounts all invited members');
check(document.querySelector('#main .wf-status').textContent === '等待工作', 'old conversation does not animate old turns');
advance(25000);
const flows = events.filter(e => e.type === 'convState' && e.state.id === 'g1').map(e => e.state.workflow);
for (const who of ['claude', 'local1', 'local2', 'codex']) {
  check(flows.some(f => f.owner === who && f.status === 'running'), who + ' participates in integrated flow');
}
const done = flows.at(-1);
check(done.status === 'done', 'desktop reaches real mock completion');
check(done.transfers.length === 5 && done.transfers[1].from === 'claude' && done.transfers[1].to === 'local2', 'second local input comes from writer');
check(done.transfers.filter(e => e.label === '交付審查建議').length === 2, 'both local replies reach main reviewer');
check(animations === 5, 'one animation for each actual dispatch');
check(document.querySelector('#main .wf-status').textContent === '協作完成', 'completed status reaches desktop component');
const completedReply = [...document.querySelectorAll('#main .msg')].find(m => m.textContent.includes('改好了，測試都通過。'));
const processSummary = completedReply?.querySelector('.act-fold');
check(processSummary?.getAttribute('aria-expanded') === 'false', 'completed activity and progress notes stay folded');
const messageId = processSummary.closest('.msg').dataset.id;
check(!processSummary.closest('.msg').textContent.includes('好，我先看看相關程式碼。'), 'short commentary does not occupy completed message');
processSummary.click(); advance(50);
check([...document.querySelectorAll('#main .msg')].find(m => m.dataset.id === messageId).textContent.includes('好，我先看看相關程式碼。'), 'folded process can be reopened after workflow completion');
document.querySelector('#main .wf-control:last-child').click(); advance(50);
check(document.querySelector('#main .wf').dataset.mode === 'compact', 'desktop collapse persists through settings roundtrip');
document.querySelector('#main .wf-control').click(); advance(50);
check(document.querySelector('#main .wf').classList.contains('wf-still'), 'desktop motion control roundtrip works');
window.CCA_DEV.openView('account'); advance(50);
check(!document.querySelector('#main .wf') && motionListeners.size === 0, 'leaving chat releases view listeners');
window.CCA_DEV.openConv('g1'); advance(100);
check(document.querySelector('#main .wf').dataset.mode === 'compact' && motionListeners.size === 1, 'return to chat restores visual preference');
console.log(`${checks} desktop integration checks passed`);
