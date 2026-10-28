"""Drive the native sandbox probe with a local CLI fixture; no model or Docker container is called."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'build/sandbox_live.exe'
NODE = shutil.which('node')

# These are simulated transport replies, including the Docker-shaped payload. Actual container
# isolation is tested separately by test_sandbox_mcp.py; this suite checks the probe's verdict.
FIXTURE = r'''
const fs = require('node:fs');
const path = require('node:path');
const readline = require('node:readline');
const scenario = process.env.CCA_PROBE_SCENARIO;
const out = value => process.stdout.write(JSON.stringify(value) + '\n');
function tool(name, id, input, result, failed = false) {
  out({ type: 'assistant', parent_tool_use_id: null, message: {
    id: 'fixture-' + id, content: [{ type: 'tool_use', id, name, input }] } });
  out({ type: 'user', parent_tool_use_id: null, message: {
    content: [{ type: 'tool_result', tool_use_id: id, is_error: failed, content: JSON.stringify(result) }] } });
}
readline.createInterface({ input: process.stdin }).on('line', line => {
  const message = JSON.parse(line);
  if (message.type === 'control_request') {
    const subtype = message.request.subtype;
    let response = {};
    if (subtype === 'initialize') response = { claude_code_version: 'fixture' };
    if (subtype === 'mcp_status') response = { mcpServers: [{ name: 'cca-sandbox',
      status: scenario === 'disconnected' ? 'failed' : 'connected',
      tools: [{ name: 'sandbox_status' }, { name: 'sandbox_run' }] }] };
    out({ type: 'control_response', response: { subtype: 'success', request_id: message.request_id, response } });
    if (subtype === 'mcp_status' && scenario === 'disconnected')
      process.stdout.write('', () => process.exit(0));
    return;
  }
  if (message.type !== 'user') return;
  const token = fs.readFileSync(path.join(process.cwd(), 'src/probe.txt'), 'utf8').trim();
  tool('mcp__cca-sandbox__sandbox_status', 'status', {}, { ready: true }, scenario === 'status-error');
  tool('mcp__cca-sandbox__sandbox_run', 'run', { command: ['fixture-only'] }, {
    isolation: 'docker', exit_code: 0, output: 'uid=65534\n' + token + '\nenv=hidden\n' });
  if (scenario === 'crash') {
    process.stdout.write('', () => process.exit(7));
    return;
  }
  const failed = scenario === 'turn-error';
  out({ type: 'result', subtype: failed ? 'error_during_execution' : 'success', is_error: failed,
    result: failed ? 'fixture turn failed after successful tools' : 'fixture complete' });
});
'''


@unittest.skipUnless(os.name == 'nt' and PROBE.is_file() and NODE and shutil.which('docker'),
                     'build sandbox-live first; needs existing Node, Python and Docker executables')
class SandboxLiveTests(unittest.TestCase):
    def run_probe(self, scenario):
        with tempfile.TemporaryDirectory(prefix='cca-probe-fixture-') as directory:
            root = Path(directory)
            (root / 'fixture.js').write_text(FIXTURE, encoding='utf-8')
            command = root / 'fixture.cmd'
            command.write_bytes(('@"%s" "%%~dp0fixture.js" %%*\r\n' % NODE).encode('utf-8'))
            env = dict(os.environ, TEMP=str(root), TMP=str(root), CCA_PROBE_SCENARIO=scenario)
            mode = '--connect-only' if scenario == 'disconnected' else '--run'
            completed = subprocess.run([str(PROBE), str(command), mode], capture_output=True, timeout=15, env=env)
            text = completed.stdout.decode('utf-8', 'replace')
            result = None
            for line in text.splitlines():
                if line.startswith('Evidence: '):
                    evidence = Path(line.removeprefix('Evidence: ')).resolve()
                    self.assertTrue(evidence.is_relative_to(root.resolve()))
                    result = json.loads((evidence / 'result.json').read_text(encoding='utf-8'))
            return completed.returncode, result, text

    def test_completed_turn_with_verified_tools_passes(self):
        code, report, output = self.run_probe('complete')
        self.assertEqual(code, 0, output)
        self.assertTrue(report['ok'])
        self.assertTrue(report['connected'])
        self.assertTrue(report['turn_completed'])

    def test_failed_turn_cannot_pass_after_successful_tools(self):
        code, report, output = self.run_probe('turn-error')
        self.assertEqual(code, 1, output)
        self.assertFalse(report['ok'])
        self.assertFalse(report['turn_completed'])

    def test_crash_cannot_pass_after_successful_tools(self):
        code, report, output = self.run_probe('crash')
        self.assertEqual(code, 1, output)
        self.assertFalse(report['ok'])
        self.assertFalse(report['turn_completed'])

    def test_failed_status_tool_cannot_pass(self):
        code, report, output = self.run_probe('status-error')
        self.assertEqual(code, 1, output)
        self.assertFalse(report['ok'])

    def test_unconnected_server_cannot_pass(self):
        code, report, output = self.run_probe('disconnected')
        self.assertEqual(code, 1, output)
        self.assertIsNone(report)


if __name__ == '__main__':
    unittest.main()
