"""The shared sandbox MCP server: protocol, input checks, host-safety and one real container round trip."""
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
SPEC = importlib.util.spec_from_file_location('sandbox_mcp', TOOLS / 'sandbox_mcp.py')
mcp = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mcp)

IMAGE = 'busybox:1.36'
DOCKER = 'C:\\fake\\docker.exe'  # docker.exe is always run by full path, never as a bare name


def docker_ready():
    exe = shutil.which('docker')
    if not exe:
        return False
    try:
        probe = subprocess.run([exe, 'image', 'inspect', IMAGE], capture_output=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return probe.returncode == 0


class SandboxMcpTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='cca-sandbox-mcp-')
        self.root = Path(self.temp.name)
        self.project = self.root / 'project'
        (self.project / 'src').mkdir(parents=True)
        (self.project / '.git').mkdir()
        (self.project / 'src' / 'a.txt').write_text('hello', encoding='utf-8')
        (self.project / '.env').write_text('SECRET=1', encoding='utf-8')
        self.data = self.root / 'data'
        self.server = mcp.Server(self.project, self.data)

    def tearDown(self):
        self.temp.cleanup()

    def talk(self, *messages):
        sink = io.StringIO()
        mcp.serve(self.server, io.StringIO(''.join(json.dumps(m) + '\n' for m in messages)), sink)
        return [json.loads(line) for line in sink.getvalue().splitlines()]

    def call(self, name, arguments):
        reply = self.talk({'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
                           'params': {'name': name, 'arguments': arguments}})[0]['result']
        text = reply['content'][0]['text']
        try:
            value = json.loads(text)
        except ValueError:
            value = text
        return reply, value

    def test_handshake_and_tool_list(self):
        replies = self.talk(
            {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {'protocolVersion': '2025-06-18'}},
            {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
            {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'},
            {'jsonrpc': '2.0', 'id': 3, 'method': 'ping'},
            {'jsonrpc': '2.0', 'id': 4, 'method': 'resources/list'})
        self.assertEqual([r['id'] for r in replies], [1, 2, 3, 4])
        self.assertEqual(replies[0]['result']['protocolVersion'], '2025-06-18')
        self.assertEqual([t['name'] for t in replies[1]['result']['tools']], ['sandbox_status', 'sandbox_run'])
        self.assertEqual(replies[3]['error']['code'], -32601)

    def test_unknown_protocol_version_falls_back(self):
        reply = self.talk({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {'protocolVersion': '1999-01-01'}})[0]
        self.assertEqual(reply['result']['protocolVersion'], '2025-03-26')

    def test_garbage_lines_are_ignored(self):
        sink = io.StringIO()
        mcp.serve(self.server, io.StringIO('not json\n[1,2]\n{"jsonrpc":"2.0","id":7,"method":"ping"}\n'), sink)
        self.assertEqual(json.loads(sink.getvalue())['id'], 7)

    def test_missing_docker_blocks_instead_of_running_on_host(self):
        with mock.patch.object(mcp.rt, 'docker_exe', return_value=''), \
                mock.patch.object(mcp.rt, 'capture', side_effect=AssertionError('must not execute')):
            for name, args in (('sandbox_status', {}), ('sandbox_run', {'image': IMAGE, 'command': ['echo', 'x']})):
                reply, text = self.call(name, args)
                self.assertTrue(reply['isError'], name)
                self.assertIn('Docker', text)

    def test_bad_arguments_are_rejected_before_docker(self):
        with mock.patch.object(mcp.rt, 'docker_exe', return_value=DOCKER), \
                mock.patch.object(mcp.rt, 'capture', side_effect=AssertionError('must not execute')):
            for args in ({'image': 'python;evil', 'command': ['echo']},
                         {'image': IMAGE, 'command': 'echo;bad'},
                         {'image': IMAGE, 'command': []},
                         {'image': IMAGE, 'command': ['x'] * 65},
                         {'image': IMAGE, 'command': ['x' * 9000]},
                         {}, [], 12, {'image': 7, 'command': ['echo']},
                         {'image': IMAGE, 'command': ['true'], 'privileged': True}):
                reply, _ = self.call('sandbox_run', args)
                self.assertTrue(reply['isError'], args)

    def test_invalid_params_do_not_break_the_connection(self):
        replies = self.talk({'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call', 'params': []},
                            {'jsonrpc': '2.0', 'id': 2, 'method': 'ping'})
        self.assertEqual(replies[0]['error']['code'], -32602)
        self.assertEqual(replies[1]['result'], {})

    def test_output_reads_are_bounded(self):
        def fake_capture(args, timeout=30):
            if args[:2] != [DOCKER, 'rm']:
                output = Path(next(a for a in args if 'target=/output' in a).split('source=')[1].split(',')[0])
                (output / 'large.txt').write_bytes(b'x' * (mcp.MAX_OUTPUT_FILE_TEXT * 4))
            return {'exit_code': 0, 'output': '', 'error': ''}

        with mock.patch.object(mcp.rt, 'docker_exe', return_value=DOCKER), \
                mock.patch.object(mcp.rt, 'capture', fake_capture), \
                mock.patch.object(Path, 'read_bytes', side_effect=AssertionError('unbounded output read')):
            reply, value = self.call('sandbox_run', {'image': IMAGE, 'command': ['true']})
        self.assertFalse(reply['isError'])
        self.assertTrue(value['output_files']['large.txt'].startswith('x' * mcp.MAX_OUTPUT_FILE_TEXT))
        self.assertLess(len(value['output_files']['large.txt']), mcp.MAX_OUTPUT_FILE_TEXT + 40)

    def test_failed_execution_removes_export_and_container(self):
        seen = []

        def fake_capture(args, timeout=30):
            seen.append(args)
            if args[:2] != [DOCKER, 'rm']:
                raise ValueError('execution timed out')
            return {'exit_code': 0, 'output': '', 'error': ''}

        with mock.patch.object(mcp.rt, 'docker_exe', return_value=DOCKER), mock.patch.object(mcp.rt, 'capture', fake_capture):
            reply, _ = self.call('sandbox_run', {'image': IMAGE, 'command': ['true']})
        self.assertTrue(reply['isError'])
        self.assertEqual(list(self.data.iterdir()), [])
        self.assertEqual(seen[-1][:3], [DOCKER, 'rm', '--force'])

    def test_export_limit_removes_partial_copy(self):
        for index in range(7):
            with (self.project / ('large-%d.txt' % index)).open('wb') as stream:
                stream.truncate(5 * 1024 * 1024)
        with mock.patch.object(mcp.rt, 'docker_exe', return_value=DOCKER), \
                mock.patch.object(mcp.rt, 'capture', side_effect=AssertionError('must not start Docker')):
            reply, _ = self.call('sandbox_run', {'image': IMAGE, 'command': ['true']})
        self.assertTrue(reply['isError'])
        self.assertEqual(list(self.data.iterdir()), [])

    def test_unknown_tool_is_an_error_result(self):
        reply, text = self.call('shell', {})
        self.assertTrue(reply['isError'])
        self.assertIn('shell', text)

    def test_run_copies_filtered_source_returns_outputs_and_cleans_up(self):
        seen = {}

        def fake_capture(args, timeout=30):
            if args[:2] == [DOCKER, 'rm']:
                seen['removed'] = args[-1]
                return {'exit_code': 0, 'output': '', 'error': ''}
            mounts = [a for a in args if a.startswith('type=bind')]
            source = Path(next(m for m in mounts if 'target=/input' in m).split('source=')[1].split(',')[0])
            output = Path(next(m for m in mounts if 'target=/output' in m).split('source=')[1].split(',')[0])
            seen['input'] = sorted(p.relative_to(source).as_posix() for p in source.rglob('*') if p.is_file())
            seen['args'] = args
            (output / 'report.txt').write_text('done', encoding='utf-8')
            seen['job'] = source.parent
            return {'exit_code': 0, 'output': 'ok\n', 'error': ''}

        with mock.patch.object(mcp.rt, 'docker_exe', return_value=DOCKER), mock.patch.object(mcp.rt, 'capture', fake_capture):
            reply, value = self.call('sandbox_run', {'image': IMAGE, 'command': ['echo', 'ok']})
        self.assertFalse(reply['isError'])
        self.assertEqual(value['exit_code'], 0)
        self.assertEqual(value['output_files'], {'report.txt': 'done'})
        self.assertNotIn('artifact_directory', value)
        self.assertEqual(seen['input'], ['src/a.txt'])
        self.assertIn('--network=none', seen['args'])
        self.assertIn('--pull=never', seen['args'])
        self.assertTrue(seen['removed'].startswith('cca-mcp-' + self.server.session + '-'))
        self.assertFalse(seen['job'].exists(), 'throwaway source copy was left behind')

    def test_each_call_gets_its_own_container_name(self):
        names = []

        def fake_capture(args, timeout=30):
            if args[:2] == [DOCKER, 'rm']:
                names.append(args[-1])
            return {'exit_code': 0, 'output': '', 'error': ''}

        with mock.patch.object(mcp.rt, 'docker_exe', return_value=DOCKER), mock.patch.object(mcp.rt, 'capture', fake_capture):
            self.call('sandbox_run', {'image': IMAGE, 'command': ['true']})
            self.call('sandbox_run', {'image': IMAGE, 'command': ['true']})
        self.assertEqual(len(set(names)), 2)

    def test_missing_image_gets_a_pull_hint(self):
        def fake_capture(args, timeout=30):
            if args[:2] == [DOCKER, 'rm']:
                return {'exit_code': 1, 'output': '', 'error': ''}
            return {'exit_code': 125, 'output': '', 'error': 'Unable to find image locally'}

        with mock.patch.object(mcp.rt, 'docker_exe', return_value=DOCKER), mock.patch.object(mcp.rt, 'capture', fake_capture):
            reply, value = self.call('sandbox_run', {'image': 'python:3.12', 'command': ['python', '-V']})
        self.assertTrue(reply['isError'])
        self.assertIn('docker pull python:3.12', value['hint'])

    @unittest.skipUnless(docker_ready(), 'needs a running Docker engine with ' + IMAGE)
    def test_real_container_round_trip(self):
        (self.root / 'host-only.txt').write_text('outside project', encoding='utf-8')
        script = 'echo uid=$(id -u); cat /input/src/a.txt; echo; test -e /input/.env && echo env=yes || echo env=no; ' \
                 'test -e /input/.git && echo git=yes || echo git=no; ' \
                 '(echo changed > /input/src/a.txt) 2>/dev/null && echo input=write || echo input=readonly; ' \
                 '(echo changed > /cca-root-probe) 2>/dev/null && echo root=write || echo root=readonly; ' \
                 'test -e /var/run/docker.sock && echo socket=yes || echo socket=no; ' \
                 'test -e /input/host-only.txt && echo host=yes || echo host=no; ' \
                 'grep CapEff /proc/self/status; ' \
                 '(wget -q -T 3 -O- http://1.1.1.1/ >/dev/null 2>&1) && echo net=open || echo net=closed; ' \
                 'echo built > /output/r.txt'
        reply, value = self.call('sandbox_run', {'image': IMAGE, 'command': ['sh', '-c', script]})
        self.assertFalse(reply['isError'], value)
        self.assertEqual(value['exit_code'], 0)
        for expected in ('uid=65534', 'hello', 'env=no', 'git=no', 'net=closed', 'input=readonly',
                         'root=readonly', 'socket=no', 'host=no', '0000000000000000'):
            self.assertIn(expected, value['output'])
        self.assertEqual(value['output_files'], {'r.txt': 'built\n'})
        self.assertEqual((self.project / 'src/a.txt').read_text(encoding='utf-8'), 'hello')
        self.assertEqual(list(self.data.iterdir()), [], 'job folders must be removed')
        status_reply, status = self.call('sandbox_status', {})
        self.assertFalse(status_reply['isError'])
        self.assertIn(IMAGE, status['images'])

    @unittest.skipUnless(docker_ready(), 'needs a running Docker engine with ' + IMAGE)
    def test_real_stdio_client_and_failed_exit_code(self):
        config = {'enabled': True, 'approved': True, 'command': sys.executable,
                  'args': ['-I', '-u', str(TOOLS / 'sandbox_mcp.py'), '--project', str(self.project),
                           '--data-dir', str(self.data)]}
        client = mcp.rt.McpClient(config)
        try:
            listed = client.call('tools/list', {})['tools']
            self.assertEqual([tool['name'] for tool in listed], ['sandbox_status', 'sandbox_run'])
            status = client.call('tools/call', {'name': 'sandbox_status', 'arguments': {}})
            self.assertFalse(status['isError'])
            result = client.call('tools/call', {'name': 'sandbox_run', 'arguments': {
                'image': IMAGE, 'command': ['sh', '-c', 'echo intentional-test-failure; exit 17']}})
            self.assertTrue(result['isError'])
            value = json.loads(result['content'][0]['text'])
            self.assertEqual(value['exit_code'], 17)
            self.assertIn('intentional-test-failure', value['output'])
        finally:
            client.close()
        self.assertEqual(list(self.data.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
