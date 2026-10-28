// DOM and animation lifecycle checks. Dev-only dependency: linkedom (see docs/workflow.md).
const assert = require('node:assert/strict');
const { parseHTML } = require('../build/workflow-qa/node_modules/linkedom');
const { document, window } = parseHTML('<html><body><main></main></body></html>');
global.document = document;
let hidden = false;
Object.defineProperty(document, 'hidden', { get: () => hidden });
const listeners = new Set();
const media = { matches: false, addEventListener: (_, fn) => listeners.add(fn), removeEventListener: (_, fn) => listeners.delete(fn) };
global.matchMedia = () => media;
const played = [];
window.Element.prototype.animate = function (frames, timing) {
  const animation = { frames, timing, currentTime: 0, canceled: false, cancel() { this.canceled = true; } };
  played.push(animation);
  return animation;
};
const { View, edgePoints, packetFrames, stateText, pipelineLayout } = require('../ui/workflow.js');
assert.deepEqual(edgePoints('codex', 'gemini'), [[710, 42], [605, 42]]);
assert.deepEqual(edgePoints('gemini', 'claude'), [[395, 42], [290, 42]]);
let checks = 0;
function check(condition, description) { assert.ok(condition, description); checks++; }
const names = id => ({ claude: '角色甲', codex: '角色乙', local1: '本地甲', local2: '本地乙' }[id]);
const host = document.querySelector('main');
let chosenMode, chosenMotion;
const view = new View(host, { onMode: x => chosenMode = x, onMotion: x => chosenMotion = x });
const options = { names, conversationId: 'group', mode: 'auto', motion: true };
const flow = {
  runId: 'task', status: 'running', label: '撰寫中', round: 1, files: 2, added: 7, removed: 1,
  nodes: [
    { id: 'claude', status: 'thinking', label: '思考中', role: '主刀' },
    { id: 'codex', status: 'waiting', label: '待命', role: '審查' },
    { id: 'local1', status: 'waiting', label: '待命', role: '唯讀審查' },
    { id: 'local2', status: 'waiting', label: '待命', role: '唯讀審查' },
  ], transfers: [],
};
view.update(flow, options);
const node = host.querySelector('[data-agent="claude"]');
check(host.querySelectorAll('.wf-node').length === 4, 'four members displayed');
check(node.dataset.status === 'thinking' && node.classList.contains('wf-active'), 'thinking lights only its node');
check(played.length === 0, 'thinking never fabricates a transfer');
check(view.status.getAttribute('role') === 'status' && view.compact.textContent.includes('本地乙：待命'), 'text alternatives describe every member');
check(view.summary.textContent.includes('2 檔案 +7 −1'), 'real change totals shown');
view.collapseButton.click(); view.motionButton.click();
check(chosenMode === 'compact' && chosenMotion === false, 'controls request persisted settings');

flow.transfers = [{ id: 'handoff', from: 'claude', to: 'codex', label: '送交審查', ts: Date.now() - 100 }];
flow.nodes[0].status = 'done'; flow.nodes[0].label = '已完成';
flow.nodes[1].status = 'writing'; flow.nodes[1].label = '審查中';
view.update(flow, options);
check(played.length === 1 && view.packetLayer.children.length === 1, 'one actual handoff creates one packet');
check(played[0].timing.duration === 1300 && played[0].currentTime >= 100, 'packet resumes from dispatch timestamp');
check(played[0].frames[0].transform.includes('290px') && played[0].frames.at(-1).transform.includes('710px'), 'packet enters correct receiving node');
for (let i = 0; i < 1000; i++) view.update(flow, options);
check(played.length === 1 && host.querySelector('[data-agent="claude"]') === node, 'streaming updates reuse nodes without replay');
check(view.seen.size === 1 && view.nodes.size === 4, 'view state stays bounded');
played[0].onfinish();
check(view.packetLayer.children.length === 0 && view.animations.size === 0, 'finished animation releases packet');
check(view.caption.textContent === '角色甲 → 角色乙 · 送交審查', 'direction and reason remain after animation');
const removedMember = flow.nodes.pop();
view.update(flow, options);
check(played.length === 1, 'changing invited members does not replay a consumed handoff');
check(!view.bus.getAttribute('d').includes('595'), 'single local member has no wire to an absent reviewer');
flow.nodes.push(removedMember);
view.update(flow, options);
check(played.length === 1 && view.bus.getAttribute('d').includes('595'), 'restoring a member restores its wire without replay');

let sequence = 0;
function dispatch(status = 'running', opt = options, age = 0) {
  flow.status = status;
  flow.transfers = [{ id: 'event-' + ++sequence, from: 'local1', to: 'codex', label: '交付審查建議', ts: Date.now() - age }];
  view.update(flow, opt);
}
for (const status of ['waitingUser', 'paused', 'done', 'stopped', 'failed']) {
  const before = played.length;
  dispatch(status);
  check(played.length === before && view.packetLayer.children.length === 0, status + ' stops transfers');
}
dispatch(); const active = played.at(-1);
dispatch('running', { ...options, motion: false });
check(active.canceled && view.animations.size === 0 && view.root.classList.contains('wf-still'), 'motion off cancels active animations');
let before = played.length;
dispatch('running', options, 2000);
check(played.length === before, 'history does not replay old transfers');
dispatch('running', { ...options, mode: 'compact' });
check(played.length === before && view.packetLayer.children.length === 0, 'compact view has no packet work');
dispatch('running', { ...options, mode: 'off' });
check(view.root.hidden && played.length === before, 'hidden view has no packet work');
media.matches = true;
dispatch();
check(played.length === before && view.root.classList.contains('wf-still'), 'system reduced motion overrides preference');
media.matches = false;
dispatch(); const visibleAnimation = played.at(-1);
hidden = true; document.dispatchEvent(new window.Event('visibilitychange'));
check(visibleAnimation.canceled && view.animations.size === 0, 'background page cancels animation');
hidden = false; document.dispatchEvent(new window.Event('visibilitychange'));
check(played.at(-1) === visibleAnimation, 'returning to page does not replay consumed transfer');

const points = edgePoints('claude', 'local1');
check(points[0][0] === 185 && points.at(-1)[0] === 405 && points.at(-1)[1] === 100, 'local reviewer receives from writer through bus');
const returned = edgePoints('local2', 'codex');
check(returned[0][0] === 595 && returned.at(-1)[0] === 815, 'local advice returns to actual reviewer');
check(edgePoints('claude', 'claude').length === 0 && edgePoints('unknown', 'codex').length === 0, 'invalid transfer has no path');
const frames = packetFrames(points);
check(frames[0].offset === 0 && frames.at(-1).offset === 1 && frames.every((f, i) => !i || f.offset >= frames[i - 1].offset), 'path timing is monotonic over corners');
check(edgePoints('codex', 'claude')[0][0] > edgePoints('codex', 'claude').at(-1)[0], 'reverse handoff travels right to left');
check(stateText(flow, names).includes('本地甲：待命'), 'compact mode retains local states');
dispatch('running', { ...options, names: () => '<img src=x onerror=1>' });
check(host.querySelectorAll('img').length === 0 && view.nodes.get('claude').name.textContent.startsWith('<img'), 'profile names cannot inject HTML');
flow.runId = 'new-task'; flow.transfers = [];
view.update(flow, options);
check(view.seen.size === 0 && host.querySelector('[data-agent="claude"]') !== node, 'fresh task clears consumed dispatches');
view.update({ ...flow, nodes: [] }, options);
check(view.root.hidden && view.nodes.size === 0 && view.animations.size === 0, 'solo DM without nodes hides and releases graph');
view.update({ ...flow, nodes: [flow.nodes[0]], transfers: [{ id: 'hidden-event', from: 'claude', to: 'codex', ts: Date.now() }] }, options);
check(view.root.hidden && view.animations.size === 0 && view.packetLayer.children.length === 0, 'single-node fallback stays hidden without packet work');
view.update(flow, options);
check(!view.root.hidden && view.nodes.size === 4, 'returning to group restores the visible graph');
const configured = Array.from({ length: 8 }, (_, i) => ({ id: 'pipeline:n' + i,
  agent: i % 3 === 0 ? 'claude' : 'gemini', name: 'Stage ' + (i + 1), profile: 'Reviewer',
  label: i === 4 ? '思考中' : '待命', status: i === 4 ? 'thinking' : 'waiting' }));
const arranged = pipelineLayout(configured);
check(arranged.height === 260 && Object.keys(arranged.positions).length === 8, 'eight stages occupy three bounded rows');
check(arranged.positions['pipeline:n3'].x > arranged.positions['pipeline:n5'].x, 'second row follows the configured snake order');
view.update({ layout: 'pipeline', runId: 'ordered', status: 'running', nodes: configured,
  transfers: [{ id: 'ordered-handoff', from: 'pipeline:n3', to: 'pipeline:n4', ts: Date.now() }] }, options);
check(view.nodes.size === 8 && host.querySelectorAll('[data-agent="gemini"]').length === 5,
  'multiple Gemini stages retain independent identities');
check(view.nodes.get('pipeline:n4').name.textContent === 'Stage 5 · Reviewer', 'stage alias and profile are both visible');
check(view.nodes.get('pipeline:n4').node.dataset.status === 'thinking', 'active stage is selected by node ID');
check(view.graph.style.height === '260px' && view.pipelineWires.children.length === 7, 'one wire joins each adjacent stage');
check(view.mainWire.style.display === 'none' && view.bus.style.display === 'none', 'pipeline does not invent classic two-agent wires');
check(view.caption.textContent.startsWith('Stage 4 · ' + names('claude') + ' → Stage 5 · Reviewer'), 'handoff names resolve configured IDs');
view.update(flow, options);
check(view.graph.style.height === '' && view.mainWire.style.display === '', 'returning to classic layout restores its geometry');
view.destroy();
check(host.children.length === 0 && listeners.size === 0 && view.animations.size === 0, 'leaving chat releases all component work');
console.log(`${checks} workflow DOM checks passed`);
