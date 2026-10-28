"""Real terminal verification and browser evidence, with temporary sources and no model requests."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    parent = root / 'build/cli-quality'
    parent.mkdir(exist_ok=True)
    case = Path(tempfile.mkdtemp(prefix='run-', dir=parent))
    data, project = case / 'data', case / 'project'
    data.mkdir(); project.mkdir()
    (project / 'check.py').write_text('print("CLI_QUALITY_OK")\n')
    test = case / 'test.json'
    test.write_text(json.dumps({'command': [sys.executable, 'check.py'], 'label': 'CLI_EVIDENCE_TEST', 'paths': ['check.py']}))
    browser_config = case / 'browser.json'
    browser_config.write_text(json.dumps({'actions': [{'action': 'click', 'selector': '#go'},
        {'action': 'expectText', 'selector': '#result', 'value': 'CLI_BROWSER_OK'}], 'label': 'CLI browser evidence'}))

    class Page(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200); self.send_header('Content-Type', 'text/html'); self.end_headers()
            self.wfile.write(b'<button id="go" onclick="document.querySelector(\'#result\').textContent=\'CLI_BROWSER_OK\'">Go</button><p id="result"></p>')

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Page)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    script = case / 'input.txt'
    script.write_text('\n'.join(['until /help 15000', 'wait 1200', 'type /verify ' + str(test), 'key enter',
        'until CLI_QUALITY_OK 20000', 'wait 400', 'type /evidence', 'key enter', 'wait 400',
        'type /browser http://127.0.0.1:' + str(server.server_address[1]), 'key enter', 'wait 2500',
        'type /browser-run ' + str(browser_config), 'key enter', 'wait 3500', 'type /evidence', 'key enter',
        'wait 400', 'type /exit', 'key enter']) + '\n', encoding='utf-8')
    env = dict(os.environ, CCA_DATA_DIR=str(data))
    for key in ('CCA_MOCK', 'CCA_TEST_CLI_FIXTURE', 'CCA_TEST_FAILURE'):
        env.pop(key, None)
    try:
        result = subprocess.run([str(root / 'build/pty_run.exe'), '120', '40', str(case / 'screen.vt'), str(script), str(args.exe.resolve()), 'gpt'],
                                cwd=project, env=env, capture_output=True, timeout=80)
        (case / 'driver.log').write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0, 'ConPTY command delivery failed'
        saved = json.loads((data / 'collaboration/dm-codex.json').read_text(encoding='utf-8'))
        evidence = saved['evidence']
        assert len(evidence) == 2, 'test or browser evidence missing'
        assert all(e['source'] == 'host' and e['passed'] and not e['stale'] for e in evidence), 'unverified or stale result'
        assert 'CLI_QUALITY_OK' in evidence[0]['output'] and evidence[0]['exitCode'] == 0, 'real output missing'
        assert (data / 'media' / evidence[1]['image']).read_bytes().startswith(b'\x89PNG\r\n\x1a\n'), 'browser image not saved'
        assert 'CLI_EVIDENCE_TEST' in (case / 'screen.vt').read_text(encoding='utf-8', errors='replace'), 'evidence command not displayed'
        print('6 real CLI quality checks passed')
        print('Artifacts: ' + str(case))
    finally:
        server.shutdown(); server.server_close()


if __name__ == '__main__':
    main()
