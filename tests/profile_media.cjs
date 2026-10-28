// Pure crop and model-date edge cases; browser painting is checked separately.
const assert = require('node:assert/strict');
const { cropRect, modelInfo } = require('../ui/profile_media.js');
let checks = 0;
function check(ok, name) { assert.ok(ok, name); checks++; }
const centered = cropRect(600, 300);
check(centered.x === 150 && centered.y === 0 && centered.side === 300, 'landscape crops centrally without stretching');
const portrait = cropRect(200, 500, 2, -999, 999);
check(portrait.x === 0 && portrait.y === 400 && portrait.side === 100, 'pan stays inside all image edges');
check(cropRect(200, 500, 999).side === 50 && cropRect(200, 500, 0).side === 200, 'zoom bounds are enforced');
const table = [
  { id: 'claude-opus-5-5', date: '2026-09-22' },
  { id: 'qwen2.5-coder:7b', date: '2024-09-19' },
  { id: 'qwen2.5-coder:14b', date: '2024-11-12' },
  { id: 'qwen3', date: '2025-04-29' },
];
const aliases = { opus: 'claude-opus-5-5', 'qwen2.5-coder:1.5b': 'qwen2.5-coder:7b' };
check(modelInfo('opus', table, aliases).date === '2026-09-22', 'known alias resolves');
check(modelInfo('claude-opus-5-5-20260922', table, aliases).date === '2026-09-22', 'snapshot resolves');
check(modelInfo('qwen2.5-coder:14b-instruct-q4_K_M', table, aliases).date === '2024-11-12', 'local size is preserved');
check(modelInfo('qwen2.5-coder:1.5b', table, aliases).date === '2024-09-19', 'initial local size uses initial release');
check(modelInfo('qwen3:14b', table, aliases).date === '2025-04-29', 'local quantization tag resolves');
for (const id of ['qwen3-coder', 'qwen3:4b-thinking-2507', 'qwen3:14b-custom', 'qwen2.5-coder:14b-custom', 'llama3.1:99b', 'claude-opus-5-5-custom', 'claude-opus-5-5-20260922-foo', 'unlisted']) {
  check(modelInfo(id, table, aliases) === null, 'unknown variant is not assigned a guessed date: ' + id);
}
console.log(`${checks} profile media checks passed`);
