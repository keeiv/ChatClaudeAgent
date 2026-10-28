// Verify actual ConPTY frames through the same decoder used for visual inspection.
const fs = require('node:fs');
const path = require('node:path');
const cp = require('node:child_process');
const assert = require('node:assert/strict');
const dir = process.argv[2];
if (!dir) throw Error('Expected a flow case folder from tests/cli_smoke.py');
function frame(name) {
  const prefix = path.join(dir, 'decoded', name);
  cp.execFileSync(process.execPath, ['tests/replay_cli.cjs', path.join(dir, 'screen.vt'), '100', '34', name, prefix]);
  const data = JSON.parse(fs.readFileSync(prefix + '.json', 'utf8'));
  return { ...data, lines: data.screen.map(row => row.map(c => c.chars).join('')) };
}
let checks = 0;
function check(condition, description) { assert.ok(condition, description); checks++; }
const thinking = frame('thinking'), nextThinking = frame('thinking-next');
function indicator(f) {
  const line = f.lines.find(l => l.includes('│') && l.includes('思考中'));
  return line?.match(/[◜◝◞◟]/)?.[0];
}
check(indicator(thinking) && indicator(nextThinking) && indicator(thinking) !== indicator(nextThinking), 'thinking spinner advances in actual terminal');
check(thinking.lines.some(l => l.includes('待命')), 'other member stays on standby while writer thinks');
const transfer = frame('transfer'), nextTransfer = frame('transfer-next');
function packet(f) {
  const row = f.screen.find(r => r.map(c => c.chars).join('').includes('│') && r.some((c, i) => i > 30 && i < 70 && c.chars === '●'));
  return row?.findIndex((c, i) => i > 30 && i < 70 && c.chars === '●');
}
check(packet(transfer) > 30 && packet(nextTransfer) > packet(transfer), 'actual packet travels toward GPT between two frames');
check(transfer.lines.some(l => l.includes('送交審查')), 'handoff reason stays visible');
const narrow = frame('narrow-flow');
check(narrow.cols === 24 && narrow.lines.some(l => l.includes('Claude') && l.includes('已完成')) &&
  narrow.lines.some(l => l.includes('GPT') && l.includes('審查中')), '24-column flow keeps both member states');
check(narrow.lines.some(l => l.includes('自動編輯') && l.includes('/help')), 'narrow flow keeps permission and help');
const approval = frame('approval');
check(approval.lines.some(l => l.includes('想要執行指令')) && approval.lines.some(l => l.includes('允許')), 'approval controls remain visible above input');
const done = frame('done');
check(done.lines.some(l => l.includes('協作完成')), 'completion reaches terminal');
check(done.lines.some(l => l.includes('2 檔案 +12')), 'completed workflow keeps actual file totals');
const hidden = frame('hidden');
check(!hidden.lines.some(l => l.includes('協作流程')), 'hiding flow clears its region');
check(hidden.lines.some(l => l.includes('自動編輯') && l.includes('/help')), 'permission and help remain visible');
console.log(`${checks} terminal recording checks passed`);
