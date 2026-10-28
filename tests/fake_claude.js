// Stand-in for the `claude` CLI that speaks the stream-json protocol CCA uses,
// so the group loop can be exercised without a Claude login.
// Writes / re-checks code like a (very small) Claude would.
const fs = require('fs');
const path = require('path');
const readline = require('readline');

const args = process.argv.slice(2);
if (args[0] === 'auth' && args[1] === 'status') {
  process.stdout.write(JSON.stringify({ loggedIn: true, authMethod: 'claude.ai', apiProvider: 'firstParty', email: 'fake@example.com', subscriptionType: 'max' }) + '\n');
  process.exit(0);
}
if (args[0] === '--version') { process.stdout.write('9.9.9 (Claude Code)\n'); process.exit(0); }

const out = (o) => process.stdout.write(JSON.stringify(o) + '\n');
const session = 'fake-' + Date.now();
let turn = 0;
const pending = new Map();

function streamText(msgId, index, text) {
  out({ type: 'stream_event', parent_tool_use_id: null, event: { type: 'message_start', message: { id: msgId } } });
  out({ type: 'stream_event', parent_tool_use_id: null, event: { type: 'content_block_start', index, content_block: { type: 'text', text: '' } } });
  for (let i = 0; i < text.length; i += 8) {
    out({ type: 'stream_event', parent_tool_use_id: null, event: { type: 'content_block_delta', index, delta: { type: 'text_delta', text: text.slice(i, i + 8) } } });
  }
  out({ type: 'stream_event', parent_tool_use_id: null, event: { type: 'content_block_stop', index } });
  out({ type: 'assistant', parent_tool_use_id: null, session_id: session, message: { id: msgId, role: 'assistant', content: [{ type: 'text', text }], usage: { input_tokens: 1200, output_tokens: 80, cache_read_input_tokens: 9000, cache_creation_input_tokens: 0 } } });
}

function ask(tool, input) {
  const rid = 'perm-' + Math.random().toString(16).slice(2);
  out({ type: 'control_request', request_id: rid, request: { subtype: 'can_use_tool', tool_name: tool, input, tool_use_id: 'tu-' + rid,
    permission_suggestions: [{ type: 'addRules', rules: [{ toolName: 'Bash', ruleContent: 'python:*' }], behavior: 'allow', destination: 'localSettings' }],
    description: '執行測試' } });
  return new Promise((resolve) => pending.set(rid, resolve));
}

function finish(text) {
  out({ type: 'result', subtype: 'success', is_error: false, duration_ms: 1500, num_turns: 2, result: text, session_id: session,
    total_cost_usd: 0.0123, usage: { input_tokens: 1200, output_tokens: 160, cache_read_input_tokens: 9000, cache_creation_input_tokens: 0 },
    modelUsage: { 'claude-haiku-5-5': { contextWindow: 200000 } } });
}

async function onUser(text) {
  turn++;
  const cwd = process.cwd();
  out({ type: 'system', subtype: 'init', session_id: session, model: 'claude-haiku-5-5', permissionMode: 'acceptEdits', mcp_servers: [], plugins: [] });
  const msgId = 'msg_' + turn;
  if (process.env.CCA_TEST_FLOW === '1') {
    out({ type: 'stream_event', parent_tool_use_id: null, event: { type: 'message_start', message: { id: msgId + '-thinking' } } });
    out({ type: 'stream_event', parent_tool_use_id: null, event: { type: 'content_block_start', index: 7, content_block: { type: 'thinking', thinking: '' } } });
    await new Promise(resolve => setTimeout(resolve, 1000));
    out({ type: 'stream_event', parent_tool_use_id: null, event: { type: 'content_block_stop', index: 7 } });
  }
  if (text.includes('回報設定') || process.env.CCA_TEST_ADAPTIVE === '1') {
    // Echo what CCA launched us with, so tests can check model, effort, permission and persona.
    const at = args.indexOf('--append-system-prompt-file');
    const prompt = at >= 0 ? fs.readFileSync(args[at + 1], 'utf8') : '';
    const pick = (flag) => { const i = args.indexOf(flag); return i >= 0 ? args[i + 1] : '(none)'; };
    streamText(msgId, 0, `model=${pick('--model')} effort=${pick('--effort')} permission=${pick('--permission-mode')} cwd=${cwd}\n---\n${prompt}`);
    finish('ok');
    return;
  }
  if (text.includes('【CCA 協作')) {
    streamText(msgId, 0, 'GPT 的修改我看過了，邏輯正確，測試也合理。\n\nVERDICT: APPROVE');
    finish('ok');
    return;
  }
  streamText(msgId, 0, '好，我來寫 is_prime 和測試。');
  const file = path.join(cwd, 'prime.py');
  const content = 'def is_prime(n):\n    if n < 2:\n        return False\n    for i in range(2, n):\n        if n % i == 0:\n            return False\n    return True\n';
  out({ type: 'assistant', parent_tool_use_id: null, session_id: session, message: { id: msgId + 'b', role: 'assistant', content: [{ type: 'tool_use', id: 'tu-w1', name: 'Write', input: { file_path: file, content } }] } });
  fs.writeFileSync(file, content);
  out({ type: 'user', parent_tool_use_id: null, session_id: session, message: { role: 'user', content: [{ type: 'tool_result', tool_use_id: 'tu-w1', content: 'File created' }] },
    tool_use_result: { type: 'create', filePath: file, content, structuredPatch: [], originalFile: null } });
  const test = path.join(cwd, 'test_prime.py');
  const testContent = 'from prime import is_prime\nassert is_prime(2)\nassert not is_prime(1)\nassert is_prime(13)\nprint("ok")\n';
  out({ type: 'assistant', parent_tool_use_id: null, session_id: session, message: { id: msgId + 'c', role: 'assistant', content: [{ type: 'tool_use', id: 'tu-w2', name: 'Write', input: { file_path: test, content: testContent } }] } });
  fs.writeFileSync(test, testContent);
  out({ type: 'user', parent_tool_use_id: null, session_id: session, message: { role: 'user', content: [{ type: 'tool_result', tool_use_id: 'tu-w2', content: 'File created' }] },
    tool_use_result: { type: 'create', filePath: test, content: testContent, structuredPatch: [], originalFile: null } });
  out({ type: 'assistant', parent_tool_use_id: null, session_id: session, message: { id: msgId + 'd', role: 'assistant', content: [{ type: 'tool_use', id: 'tu-b1', name: 'Bash', input: { command: 'python test_prime.py', description: '執行測試' } }] } });
  const decision = await ask('Bash', { command: 'python test_prime.py', description: '執行測試' });
  const allowed = decision.behavior === 'allow';
  out({ type: 'user', parent_tool_use_id: null, session_id: session, message: { role: 'user', content: [{ type: 'tool_result', tool_use_id: 'tu-b1', is_error: !allowed, content: allowed ? 'ok' : "The user doesn't want to proceed with this tool use." }] },
    tool_use_result: allowed ? { stdout: 'ok', stderr: '', interrupted: false } : 'denied' });
  out({ type: 'rate_limit_event', rate_limit_info: { status: 'allowed', resetsAt: Math.floor(Date.now() / 1000) + 7200, rateLimitType: 'five_hour', utilization: 0.21 }, session_id: session });
  streamText(msgId + 'e', 0, '完成：`prime.py` 和 `test_prime.py` 都寫好了。');
  finish('done');
}

const rl = readline.createInterface({ input: process.stdin });
rl.on('line', (line) => {
  let m;
  try { m = JSON.parse(line); } catch { return; }
  if (m.type === 'control_request') {
    const sub = m.request.subtype;
    const response = sub === 'initialize'
      ? { models: process.env.CCA_TEST_ADAPTIVE === '1' ? ['haiku', 'sonnet', 'opus'].map(family => ({ value: 'claude-' + family + '-test', displayName: family, supportedEffortLevels: ['low', 'medium', 'high'] })) : [{ value: 'default', displayName: 'Default', description: 'fake', supportedEffortLevels: ['low', 'high'] }, { value: 'haiku', displayName: 'Haiku', description: 'fake fast' }], account: { subscriptionType: 'max' }, claude_code_version: '9.9.9' }
      : {};
    out({ type: 'control_response', response: { subtype: 'success', request_id: m.request_id, response } });
  } else if (m.type === 'control_response') {
    const r = pending.get(m.response.request_id);
    if (r) { pending.delete(m.response.request_id); r(m.response.response); }
  } else if (m.type === 'user') {
    const c = m.message.content;
    onUser(Array.isArray(c) ? c.map((x) => x.text || '').join('') : String(c));
  }
});
rl.on('close', () => process.exit(0));
