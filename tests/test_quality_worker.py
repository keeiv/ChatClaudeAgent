"""Real local commands, Git archives and browser workflows without model calls or user data."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import queue
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('quality_worker', ROOT / 'tools/quality_worker.py')
q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(q)


class QualityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='cca-quality-')
        self.root = Path(self.temp.name)
        (self.root / 'value.py').write_text('VALUE = 1\n', encoding='utf-8')

    def tearDown(self):
        self.temp.cleanup()

    def run_request(self, **kwargs):
        return q.run({'cwd': str(self.root), **kwargs})

    def test_fingerprint_is_content_bound(self):
        before = q.fingerprint(self.root)
        (self.root / 'value.py').write_text('VALUE = 2\n', encoding='utf-8')
        self.assertNotEqual(before, q.fingerprint(self.root))

    def test_dependencies_and_secrets_are_excluded(self):
        before = q.fingerprint(self.root)
        (self.root / '.env').write_text('SECRET=fixture\n')
        (self.root / 'node_modules').mkdir()
        (self.root / 'node_modules/x.js').write_text('ignored')
        self.assertEqual(before, q.fingerprint(self.root))

    def test_pass_and_failure_are_real_exit_codes(self):
        result = self.run_request(action='test', command=[sys.executable, '-c', 'print("VERIFIED")'])
        self.assertTrue(result['passed'])
        self.assertEqual(result['exitCode'], 0)
        self.assertIn('VERIFIED', result['output'])
        self.assertFalse(self.run_request(action='test', command=[sys.executable, '-c', 'raise SystemExit(3)'])['passed'])

    def test_ipc_replies_without_reading_the_parent_input(self):
        proc = subprocess.Popen([sys.executable, '-I', '-u', str(ROOT / 'tools/quality_worker.py')],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
        replies = queue.Queue()
        threading.Thread(target=lambda: replies.put(proc.stdout.readline()), daemon=True).start()
        try:
            proc.stdin.write(json.dumps({'cwd': str(self.root), 'action': 'test', 'command': [sys.executable, '-c', 'print("IPC_OK")'],
                                         'convId': 'fixture', 'requestToken': 'fixture'}) + '\n')
            proc.stdin.flush()
            result = json.loads(replies.get(timeout=8))
            self.assertTrue(result['result']['passed'])
            self.assertIn('IPC_OK', result['result']['output'])
        finally:
            proc.stdin.close()
            proc.wait(timeout=10)
            proc.stdout.close(); proc.stderr.close()

    def test_source_modification_invalidates_pass(self):
        result = self.run_request(action='test', command=[sys.executable, '-c', 'open("value.py","w").write("VALUE=5")'])
        self.assertTrue(result['sourceChanged'])
        self.assertFalse(result['passed'])

    def test_path_traversal_and_missing_files(self):
        with self.assertRaises(ValueError):
            self.run_request(action='test', command=[sys.executable, '-c', 'pass'], paths=['../secret'])
        with self.assertRaises(ValueError):
            self.run_request(action='test', command=[sys.executable, '-c', 'pass'], paths=['missing.py'])

    def test_timeout_never_passes(self):
        result = self.run_request(action='test', command=[sys.executable, '-c', 'import time; time.sleep(5)'], timeout=1)
        self.assertTrue(result['timedOut'])
        self.assertFalse(result['passed'])

    def test_descendants_are_stopped_when_the_command_finishes(self):
        script = 'import subprocess,sys; subprocess.Popen([sys.executable,"-c","import time;time.sleep(30)" ]);print("PARENT_DONE")'
        started = time.monotonic()
        result = self.run_request(action='test', command=[sys.executable, '-c', script])
        self.assertTrue(result['passed'])
        self.assertIn('PARENT_DONE', result['output'])
        self.assertLess(time.monotonic() - started, 8)

    def test_output_is_bounded_and_redacted(self):
        result = self.run_request(action='test', command=[sys.executable, '-c', 'print("x"*120000); print("ghp_"+"a"*35)'])
        self.assertLessEqual(len(result['output']), q.MAX_LOG)
        self.assertNotIn('ghp_', result['output'])

    def test_same_case_fails_before_and_passes_after(self):
        for argv in (['git', 'init', '-b', 'main'], ['git', 'config', 'user.name', 'CCA Test'],
                     ['git', 'config', 'user.email', 'cca@example.invalid'], ['git', 'add', '.'], ['git', 'commit', '-m', 'Before']):
            subprocess.run(argv, cwd=self.root, check=True, capture_output=True)
        (self.root / 'value.py').write_text('VALUE = 2\n', encoding='utf-8')
        (self.root / 'regression.py').write_text('from value import VALUE\nassert VALUE == 2, "REGRESSION_EXPECTED_TWO"\nprint("fixed")\n', encoding='utf-8')
        request = {'action': 'repro', 'command': [sys.executable, 'regression.py'], 'cases': ['regression.py'],
                   'expectedFailure': 'REGRESSION_EXPECTED_TWO', 'base': 'HEAD'}
        before = q.fingerprint(self.root)
        result = self.run_request(**request)
        self.assertTrue(result['passed'])
        self.assertNotEqual(result['before']['exitCode'], 0)
        self.assertEqual(result['after']['exitCode'], 0)
        self.assertEqual(before, q.fingerprint(self.root))
        request['expectedFailure'] = 'WRONG_FAILURE'
        self.assertFalse(self.run_request(**request)['passed'])
        request['base'] = '--help'
        with self.assertRaises(ValueError):
            self.run_request(**request)


if __name__ == '__main__':
    unittest.main()
