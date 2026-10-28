"""Local Responses fixture: reviewers pass only after reading the preceding agent's edit."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import time


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, value, code=200):
        body = json.dumps(value).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        self.reply({'data': [{'id': name} for name in ('author-model', 'review-a', 'review-b')]})

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        model = request['model']
        if model == 'slow-author':
            time.sleep(1)
        author = model in ('author-model', 'slow-author')
        context = json.dumps(request['input'], ensure_ascii=False)
        if 'STEER_REQUIRED' in context and 'STEER_TOKEN' not in context:
            self.reply({'error': 'latest user instruction is missing'}, 409); return
        if model == 'broken-model':
            self.reply({'error': 'fixture outage'}, 503); return
        results = [m for m in request['input'] if m.get('type') == 'function_call_output']
        if not results:
            name = 'write_file' if author else 'read_file' if model == 'stuck-model' else 'git_diff'
            arguments = {'path': 'result.txt', **({'content': 'fixed by previous agent\n'} if name == 'write_file' else {})}
            output = [{'type': 'function_call', 'call_id': 'fixture-call', 'name': name, 'arguments': json.dumps(arguments)}]
        elif model == 'stuck-model':
            output = [{'type': 'message', 'role': 'assistant', 'content': [{'type': 'output_text', 'text': '同一問題仍待修正\nREVIEW: NEEDS_FIX'}]}]
        elif len(results) == 1 and not author:
            try:
                delta = json.loads(results[0]['output'])
                if 'fixed by previous agent' not in delta['diff']:
                    raise ValueError('missing preceding diff')
            except (ValueError, KeyError):
                self.reply({'error': 'reviewer did not inspect the real Git diff'}, 409); return
            output = [{'type': 'function_call', 'call_id': 'read-call', 'name': 'read_file', 'arguments': json.dumps({'path': 'result.txt'})}]
        else:
            if not author and 'fixed by previous agent' not in results[-1]['output']:
                self.reply({'error': 'reviewer did not receive the edit'}, 409); return
            text = '已修改 result.txt，交給下一位實際讀取確認。\nREVIEW: NEEDS_FIX' if author else '已讀取 result.txt，確認前一位的修改保留。\nREVIEW: PASS'
            output = [{'type': 'message', 'role': 'assistant', 'content': [{'type': 'output_text', 'text': text}]}]
        self.reply({'output': output, 'usage': {'input_tokens': 100, 'output_tokens': 40}})


server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
print(json.dumps({'port': server.server_port}), flush=True)
server.serve_forever()
