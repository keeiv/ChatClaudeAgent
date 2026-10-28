"""Optional API/local agent worker. JSON-lines IPC; secrets never leave this process."""
import base64
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

MAX_BODY = 8 * 1024 * 1024
MAX_FILE = 128 * 1024
REDACTIONS = []


def output(value):
    def redact(item):
        if isinstance(item, str):
            for secret in REDACTIONS:
                if secret:
                    item = item.replace(secret, '[credential hidden]')
        elif isinstance(item, dict):
            return {redact(k): redact(v) for k, v in item.items()}
        elif isinstance(item, list):
            return [redact(v) for v in item]
        return item
    text = json.dumps(redact(value), ensure_ascii=False)
    print(text, flush=True)


def emit(kind, data=None, ident=''):
    output({'event': kind, 'id': ident, 'data': data or {}})


def safe_id(value):
    if not re.fullmatch(r'[a-zA-Z0-9_-]{1,80}', value or ''):
        raise ValueError('識別碼只能使用英數字、底線及連字號')
    return value


def protect(data, decrypt=False):
    class Blob(ctypes.Structure):
        _fields_ = [('size', wintypes.DWORD), ('data', ctypes.POINTER(ctypes.c_ubyte))]
    buffer = ctypes.create_string_buffer(data)
    source = Blob(len(data), ctypes.cast(buffer, ctypes.POINTER(ctypes.c_ubyte)))
    target = Blob()
    api = ctypes.windll.crypt32.CryptUnprotectData if decrypt else ctypes.windll.crypt32.CryptProtectData
    if not api(ctypes.byref(source), None, None, None, None, 1, ctypes.byref(target)):
        raise ValueError('Windows 無法保護或解開這份憑證')
    try:
        return ctypes.string_at(target.data, target.size)
    finally:
        ctypes.windll.kernel32.LocalFree(target.data)


def atomic(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + '.tmp')
    temp.write_bytes(data)
    os.replace(temp, path)


def endpoint(url):
    parts = urllib.parse.urlsplit(url)
    if parts.username or parts.password or parts.query or parts.fragment:
        raise ValueError('服務網址不能包含登入資料、查詢或片段')
    if parts.scheme != 'https' and not (parts.scheme == 'http' and parts.hostname in ('127.0.0.1', '::1')):
        raise ValueError('API 必須使用 HTTPS；HTTP 僅限本機測試')
    if not parts.hostname:
        raise ValueError('服務網址缺少主機名稱')
    return url.rstrip('/')


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def http(url, payload=None, key='', timeout=30, headers=None):
    request_headers = {'Content-Type': 'application/json', **(headers or {})}
    if key:
        request_headers['Authorization'] = 'Bearer ' + key
    body = None if payload is None else json.dumps(payload).encode('utf-8')
    for attempt in range(3):
        request = urllib.request.Request(url, data=body, headers=request_headers)
        try:
            with urllib.request.build_opener(NoRedirect()).open(request, timeout=timeout) as response:
                data = response.read(MAX_BODY + 1)
                if len(data) > MAX_BODY:
                    raise ValueError('服務回應超過大小限制')
                return json.loads(data), dict(response.headers)
        except urllib.error.HTTPError as error:
            if error.code not in (429, 502, 503, 504) or attempt == 2:
                raise ValueError('API 回應 HTTP %d；請檢查服務、模型與認證' % error.code) from None
            delay = min(10, int(error.headers.get('Retry-After', '0')) if error.headers.get('Retry-After', '').isdigit() else 2 ** attempt)
            emit('Retry', {'attempt': attempt + 1, 'max': 3, 'delayMs': delay * 1000, 'error': '服務暫時忙碌'})
            time.sleep(delay)
        except (urllib.error.URLError, TimeoutError):
            raise ValueError('API 連線失敗或逾時') from None


class Store:
    def __init__(self, data_dir):
        self.root = Path(data_dir) / 'provider-credentials'

    def path(self, ident):
        return self.root / (safe_id(ident) + '.bin')

    def save(self, ident, url, key):
        if '\r' in key or '\n' in key or len(key) > 8192:
            raise ValueError('API Key 格式不正確')
        atomic(self.path(ident), protect(json.dumps({'url': endpoint(url), 'key': key}).encode()))

    def key(self, provider):
        path = self.path(provider['id'])
        if not path.exists():
            raise ValueError('尚未儲存這個供應商的 API Key')
        saved = json.loads(protect(path.read_bytes(), True))
        if saved['url'] != endpoint(provider['url']):
            raise ValueError('服務網址已變更，請重新儲存 API Key')
        REDACTIONS.append(saved['key'])
        return saved['key']


DENIED_NAMES = ('.git', '.claude', '.codex', '.npmrc', '.netrc', 'credentials', 'provider-credentials')


def denied_name(name):
    folded = name.casefold()
    return folded.startswith('.env') or folded in DENIED_NAMES


def docker_exe(project_root=None):
    """Absolute path of docker.exe, or ''. A bare "docker" would also be looked up in the current folder
    on Windows, and that folder may be a project an agent can write to; a docker.exe inside the project
    is refused for the same reason."""
    os.environ['NoDefaultCurrentDirectoryInExePath'] = '1'
    exe = os.environ.get('CCA_DOCKER', '') or shutil.which('docker') or ''
    if not exe or not os.path.isabs(exe) or not os.path.isfile(exe):
        return ''
    resolved = Path(exe).resolve()
    if project_root and resolved.is_relative_to(Path(project_root).resolve()):
        return ''
    return str(resolved)


class Project:
    def __init__(self, root):
        self.root = Path(root).resolve(strict=True)

    def path(self, relative, create=False):
        part = Path(relative)
        if part.is_absolute() or '..' in part.parts or ':' in relative or '\x00' in relative:
            raise ValueError('只能操作專案內的相對路徑')
        # Windows drops trailing dots and spaces and accepts 8.3 short names, so ".git." or "GIT~1"
        # would reach .git while passing a check on the raw text.
        if any(p != p.rstrip('. ') or re.search(r'~\d', p) for p in part.parts):
            raise ValueError('路徑名稱格式不正確')
        if any(denied_name(p) for p in part.parts):
            raise ValueError('這個位置不開放給代理工具')
        full = self.root / part
        for entry in [full, *full.parents]:
            if entry == self.root:
                break
            if entry.is_symlink() or (entry.exists() and getattr(entry.stat(follow_symlinks=False), 'st_file_attributes', 0) & 0x400):
                raise ValueError('工具不跟隨連結或重新解析點')
        resolved = full.resolve(strict=not create)
        if not resolved.is_relative_to(self.root):
            raise ValueError('路徑超出專案範圍')
        # Check the names the system actually resolved, not only what was typed.
        if any(denied_name(p) for p in resolved.relative_to(self.root).parts):
            raise ValueError('這個位置不開放給代理工具')
        return resolved

    def read(self, relative):
        file = self.path(relative)
        with file.open('rb') as stream:
            data = stream.read(MAX_FILE + 1)
        if len(data) > MAX_FILE:
            raise ValueError('檔案超過 128 KiB，請縮小讀取範圍')
        return data.decode('utf-8-sig')


def sandbox_plan(project, image, command, output, container='', docker=''):
    docker = docker or docker_exe()
    if not docker:
        raise ValueError('BLOCKED：本機沒有 Docker；不會改在宿主機執行，也不會自動安裝')
    if ',' in str(project) or ',' in str(output):
        raise ValueError('沙箱暫存路徑不能包含逗號')
    if not isinstance(image, str) or not re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9./:@_-]{0,180}', image):
        raise ValueError('請指定已安裝的容器映像')
    if not isinstance(command, list) or not command or not all(isinstance(s, str) and '\x00' not in s for s in command):
        raise ValueError('沙箱命令必須是參數陣列')
    if container and not re.fullmatch(r'cca-[a-zA-Z0-9_-]{1,90}', container):
        raise ValueError('沙箱識別碼無效')
    return [docker, 'run', '--rm', *(['--name', container] if container else []), '--pull=never', '--network=none', '--read-only', '--cap-drop=ALL',
            '--security-opt=no-new-privileges', '--pids-limit=128', '--memory=512m', '--cpus=1',
            '--user=65534:65534', '--tmpfs=/tmp:rw,nosuid,nodev,size=128m',
            '--mount', 'type=bind,source=%s,target=/input,readonly' % project,
            '--mount', 'type=bind,source=%s,target=/output' % output,
            '--workdir=/tmp', image, *command]


def run_sandbox(project, image, command, data_dir, owner=''):
    docker = docker_exe(project.root)
    if not docker:
        raise ValueError('BLOCKED：本機沒有可用的 Docker（找不到，或位在專案資料夾裡）；不會改在宿主機執行，也不會自動安裝')
    # Reject invalid requests before copying source or creating a throwaway directory.
    container = 'cca-' + safe_id(owner) if owner else ''
    sandbox_plan(project.root, image, command, Path(data_dir) / 'output', container, docker)
    # Export only allowed source files, never mount the working tree or home into a container.
    job = Path(tempfile.mkdtemp(prefix='cca-sandbox-', dir=data_dir))
    try:
        source, output = job / 'input', job / 'output'
        source.mkdir(); output.mkdir()
        copied = 0
        for folder, dirs, files in os.walk(project.root, followlinks=False):
            dirs[:] = [d for d in dirs if d.casefold() not in ('.git', '.claude', '.codex', 'node_modules', 'build', '.venv')
                       and not (Path(folder) / d).is_symlink() and (Path(folder) / d).resolve() != job.resolve()]
            for name in files:
                path = Path(folder) / name
                rel = path.relative_to(project.root)
                if name.casefold().startswith('.env') or path.is_symlink():
                    continue
                try:
                    checked = project.path(str(rel))
                    size = checked.stat().st_size
                    if size > 5 * 1024 * 1024:
                        continue
                    copied += size
                    if copied > 32 * 1024 * 1024:
                        raise RuntimeError('沙箱來源超過 32 MiB')
                    destination = source / rel
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(checked, destination)
                except (OSError, ValueError):
                    continue
        name = container or 'cca-' + job.name
        args = sandbox_plan(source, image, command, output, name, docker)
        try:
            result = capture(args, timeout=120)
        finally:
            # A killed client alone can leave a daemon-owned container running.
            capture([docker, 'rm', '--force', name], timeout=10)
        return {**result, 'artifact_directory': str(output), 'isolation': 'docker'}
    except BaseException:
        shutil.rmtree(job, ignore_errors=True)
        raise


def clean_env():
    return {key: value for key, value in os.environ.items() if key.upper() in ('PATH', 'SYSTEMROOT', 'WINDIR', 'TEMP', 'TMP', 'PATHEXT')}


def capture(args, timeout=30):
    process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=clean_env(),
                               creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    buffers = [bytearray(), bytearray()]
    def read(stream, target):
        while True:
            chunk = stream.read(4096)
            if not chunk:
                break
            target.extend(chunk)
            if len(target) > 65536:
                del target[:-65536]
    readers = [threading.Thread(target=read, args=(stream, buffers[i])) for i, stream in enumerate((process.stdout, process.stderr))]
    for reader in readers:
        reader.start()
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill(); process.wait()
        raise ValueError('工具執行逾時，沒有取得成功結果')
    finally:
        for reader in readers:
            reader.join(timeout=2)
        process.stdout.close(); process.stderr.close()
    return {'exit_code': process.returncode, 'output': buffers[0].decode('utf-8', 'replace'),
            'error': buffers[1].decode('utf-8', 'replace')}


def tool_fingerprint(tool):
    """SHA-256 of what the model reads about a tool: name, description, input schema and annotations.

    A server that changes any of these after the user approved the tool (a "rug pull") produces a different hash.
    Canonical JSON (sorted keys, no spaces) keeps the hash independent of key order and formatting.
    """
    pinned = {'name': tool.get('name', ''), 'description': tool.get('description') or '',
              'inputSchema': tool.get('inputSchema') or {}, 'annotations': tool.get('annotations') or {}}
    text = json.dumps(pinned, sort_keys=True, ensure_ascii=False, separators=(',', ':'))
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


def pin_state(pins, tool):
    """'ok' when the live tool matches its pin, 'changed' when it differs, 'unpinned' when never approved."""
    pin = (pins or {}).get(tool['name'])
    if not pin:
        return 'unpinned'
    return 'ok' if pin == tool_fingerprint(tool) else 'changed'


class McpClient:
    def __init__(self, config):
        if not config.get('enabled') or not config.get('approved'):
            raise ValueError('這個 MCP 伺服器尚未由使用者核准啟用')
        executable = shutil.which(config.get('command', ''))
        if not executable or Path(executable).suffix.lower() in ('.cmd', '.bat', '.ps1'):
            raise ValueError('MCP 必須指定可信任的原生執行檔；不自動安裝套件')
        self.process = subprocess.Popen([executable, *config.get('args', [])], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=clean_env(),
                                        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        self.lines = queue.Queue(maxsize=8)
        self.serial = 0
        def read():
            while True:
                line = self.process.stdout.readline(MAX_BODY + 1)
                if not line:
                    break
                if len(line) > MAX_BODY:
                    self.lines.put(None); break
                self.lines.put(line)
            self.lines.put(None)
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()
        try:
            self.call('initialize', {'protocolVersion': '2025-03-26', 'capabilities': {}, 'clientInfo': {'name': 'CCA', 'version': '0.6'}})
            self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
        except Exception:
            self.close()
            raise

    def send(self, message):
        self.process.stdin.write((json.dumps(message) + '\n').encode()); self.process.stdin.flush()

    def call(self, method, params):
        self.serial += 1
        self.send({'jsonrpc': '2.0', 'id': self.serial, 'method': method, 'params': params})
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            try:
                raw = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if raw is None:
                raise ValueError('MCP 子程序已結束或回應過大')
            message = json.loads(raw)
            for reply in message if isinstance(message, list) else [message]:
                if 'method' in reply and 'id' in reply:
                    answer = {'result': {}} if reply['method'] == 'ping' else {'error': {'code': -32601, 'message': 'Unsupported method'}}
                    self.send({'jsonrpc': '2.0', 'id': reply['id'], **answer})
                    continue
                if reply.get('id') == self.serial:
                    if reply.get('error'):
                        raise ValueError('MCP 拒絕這次呼叫')
                    return reply.get('result', {})
        raise ValueError('MCP 連線逾時')

    def list_tools(self):
        """Every tool the server advertises now, following pagination (bounded so a server cannot loop forever)."""
        tools, cursor = [], None
        for _ in range(10):
            result = self.call('tools/list', {'cursor': cursor} if cursor else {})
            tools.extend(tool for tool in result.get('tools', []) if isinstance(tool, dict) and isinstance(tool.get('name'), str))
            cursor = result.get('nextCursor')
            if not cursor:
                break
        return tools

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3)
        self.reader.join(timeout=2)
        self.process.stdin.close()
        self.process.stdout.close()


TOOL_PROPERTIES = {
    'read_file': {'path': {'type': 'string'}},
    'git_diff': {'path': {'type': 'string'}},
    'list_files': {'path': {'type': 'string'}},
    'write_file': {'path': {'type': 'string'}, 'content': {'type': 'string'}},
    'sandbox_run': {'image': {'type': 'string'}, 'command': {'type': 'array', 'items': {'type': 'string'}}},
    'publish_artifact': {'path': {'type': 'string'}, 'result': {'type': 'string'}, 'operation': {'type': 'string'}},
    'read_artifact': {'id': {'type': 'string'}},
    'task_history': {'after': {'type': 'integer'}},
    'task_read': {'record': {'type': 'string'}, 'offset': {'type': 'integer'}},
    'mcp_discover': {'server': {'type': 'string'}},
    'mcp_call': {'server': {'type': 'string'}, 'tool': {'type': 'string'}, 'arguments': {'type': 'string'}},
}


class Tools:
    def __init__(self, request):
        self.request = dict(request)
        self.request['servers'] = request.get('servers') or []
        self.request['artifacts'] = request.get('artifacts') or []
        self.project = Project(request['cwd'])
        self.readonly = request.get('readonly', False)
        self.reads = set()
        self.clients = {}

    def close(self):
        for client in self.clients.values():
            client.close()
        self.clients.clear()

    def schemas(self):
        allowed = ('read_file', 'git_diff', 'list_files', 'read_artifact', 'task_history', 'task_read') if self.readonly else TOOL_PROPERTIES.keys()
        servers = [server['id'] for server in self.request.get('servers', []) if server.get('enabled') and server.get('approved')]
        result = []
        for name in allowed:
            if name.startswith('mcp_') and not servers:
                continue
            properties = dict(TOOL_PROPERTIES[name])
            if name.startswith('mcp_'):
                properties['server'] = {'type': 'string', 'enum': servers}
            result.append({'type': 'function', 'name': name, 'description': name + '（只限專案；寫入及執行依權限核准）',
                           'parameters': {'type': 'object', 'properties': properties, 'required': list(properties), 'additionalProperties': False}, 'strict': True})
        return result

    def approval(self, name, args, ident):
        if self.request.get('permission') != 'ask' and name == 'write_file':
            return
        emit('Approval', {'agent': 'codex', 'kind': 'command', 'title': name, 'command': json.dumps(args, ensure_ascii=False), 'state': 'pending'}, ident)
        answer = json.loads(sys.stdin.readline())
        if answer.get('id') != ident or answer.get('choice') not in ('allow', 'allowSession'):
            raise ValueError('使用者未核准這項操作')
        emit('Resolved', {'state': 'allowed'}, ident)

    def execute(self, name, args, ident):
        if name not in {t['name'] for t in self.schemas()}:
            raise ValueError('唯讀代理或目前權限不允許這個工具')
        if name == 'read_file':
            text = self.project.read(args['path']); self.reads.add(args['path'])
            return text
        if name == 'git_diff':
            base, head = self.request.get('diff_base', ''), self.request.get('diff_head', '')
            if not all(re.fullmatch(r'[0-9a-f]{40,64}', rev) for rev in (base, head)):
                raise ValueError('尚無完整的前次交接版本；請查原始檔案')
            path = args['path']
            if path:
                self.project.path(path, create=True)
            args = ['git', '--no-pager', '--git-dir=' + self.request['snapshot_repo'],
                    '--work-tree=' + str(self.project.root), 'diff', '--no-ext-diff', '--no-textconv',
                    '--no-renames', base, head, '--', *([path] if path else [])]
            result = capture(args)
            if result['exit_code']:
                raise ValueError('無法讀取交接的實際 Git 差異')
            return {'base_revision': base, 'head_revision': head, 'diff': result['output']}
        if name == 'task_read':
            record = args['record']
            if record != 'request' and not re.fullmatch(r'[1-9][0-9]{0,6}', record):
                raise ValueError('紀錄只能是 request 或交接序號')
            folder = Path(self.request['data_dir']) / 'task-history' / safe_id(self.request['task_id'])
            text = (folder / (record + '.json')).read_text(encoding='utf-8')
            start = int(args['offset'])
            if start < 0 or start > len(text):
                raise ValueError('紀錄讀取位置無效')
            return {'record': record, 'offset': start, 'total_chars': len(text), 'text': text[start:start + 6000],
                    'next_offset': min(len(text), start + 6000)}
        if name == 'task_history':
            folder = Path(self.request['data_dir']) / 'task-history' / safe_id(self.request['task_id'])
            if int(args['after']) < 0:
                raise ValueError('交接索引不能小於零')
            records = []
            for n in range(int(args['after']) + 1, int(args['after']) + 21):
                file = folder / (str(n) + '.json')
                if not file.exists():
                    break
                entry = json.loads(file.read_text(encoding='utf-8'))
                records.append({key: entry.get(key) for key in ('seq', 'agent_id', 'role', 'verdict', 'summary', 'base_revision', 'head_revision', 'diff_reference')})
            return {'request_record': 'request', 'handoffs': records,
                    'artifacts': self.request.get('artifacts', [])}
        if name == 'list_files':
            folder = self.project.path(args['path'])
            return '\n'.join(p.name for p in list(folder.iterdir())[:200] if p.name not in ('.git', '.env', '.claude', '.codex'))
        if name == 'read_artifact':
            if not self.request.get('vision'):
                raise ValueError('這個模型尚未宣告影像能力，請改用可讀圖的代理')
            artifact = next((a for a in self.request.get('artifacts', []) if a['id'] == args['id']), None)
            if not artifact or not re.fullmatch(r'artifact-[a-zA-Z0-9_-]+\.png', artifact['file']):
                raise ValueError('找不到這份任務的圖片證據')
            path = Path(self.request['data_dir']) / 'media' / artifact['file']
            if path.stat().st_size > 5 * 1024 * 1024:
                raise ValueError('圖片超過模型輸入上限')
            self.image = 'data:image/png;base64,' + base64.b64encode(path.read_bytes()).decode('ascii')
            return '已載入驗證過的任務圖片；下一則輸入包含圖片內容。'
        if name == 'write_file':
            if self.request.get('permission') == 'plan':
                raise ValueError('規劃模式不允許寫入')
            self.approval(name, args, ident)
            path = self.project.path(args['path'], True)
            content = args['content'].encode('utf-8')
            if len(content) > MAX_FILE:
                raise ValueError('單次寫入超過 128 KiB')
            old = path.read_bytes() if path.exists() else b''
            atomic(path, content)
            emit('FileChange', {'path': args['path'], 'change': 'update' if old else 'add',
                                'added': len(content.splitlines()), 'removed': len(old.splitlines())}, ident)
            return '檔案已儲存，請重新讀取確認'
        if name == 'sandbox_run':
            if self.request.get('permission') == 'plan':
                raise ValueError('規劃模式不允許執行')
            self.approval(name, args, ident)
            result = run_sandbox(self.project, args['image'], args['command'], self.request['data_dir'], self.request.get('sandbox_owner', ''))
            # Only bounded PNG evidence is promoted; tested source stays in the disposable container.
            folder = Path(result['artifact_directory'])
            exported = []
            try:
                for path in folder.glob('*.png'):
                    if len(exported) >= 8:
                        break
                    if path.is_symlink() or getattr(path.stat(follow_symlinks=False), 'st_file_attributes', 0) & 0x400 or path.stat().st_size > 10 * 1024 * 1024:
                        continue
                    relative = '.cca-artifacts/' + safe_id(self.request['task_id']) + '/' + path.name
                    destination = self.project.path(relative, True)
                    atomic(destination, path.read_bytes())
                    emit('Artifact', {'path': relative, 'source': 'docker', 'operation': ' '.join(args['command'])[:300],
                                      'result': 'PASS' if result['exit_code'] == 0 else 'FAIL'})
                    exported.append(relative)
            finally:
                shutil.rmtree(folder.parent, ignore_errors=True)
            result.pop('artifact_directory', None)
            result['artifacts'] = exported
            return result
        if name == 'publish_artifact':
            path = self.project.path(args['path'])
            if path.suffix.lower() != '.png' or path.stat().st_size > 10 * 1024 * 1024:
                raise ValueError('測試證據必須是 10 MiB 以內的 PNG')
            emit('Artifact', {'path': str(path.relative_to(self.project.root)), 'result': args['result'], 'operation': args['operation'], 'source': 'agent-tool'})
            return '測試圖片已提交主機驗證與加入聊天室'
        config = next((s for s in self.request.get('servers', []) if s['id'] == args['server']), None)
        if not config:
            raise ValueError('找不到這個 MCP 伺服器')
        allowed, pins = config.get('allowedTools', []), config.get('pins') or {}
        if name == 'mcp_call' and args['tool'] not in allowed:
            raise ValueError('這個 MCP 工具尚未列入使用者核准清單')
        if name == 'mcp_call' and args['tool'] not in pins:
            raise ValueError('這個 MCP 工具的描述尚未釘住；請到設定連線測試後核准，主機才會放行')
        self.approval(name, args, ident)
        client = self.clients.get(config['id'])
        if client is None:
            client = self.clients[config['id']] = McpClient(config)
        try:
            # The description the model sees must be the one the user approved, so list again before every use.
            live = {tool['name']: tool for tool in client.list_tools()}
            if name == 'mcp_discover':
                shown, held = [], []
                for tool in live.values():
                    if tool['name'] not in allowed:
                        continue
                    state = pin_state(pins, tool)
                    if state == 'ok':
                        shown.append(tool)
                    else:
                        held.append({'name': tool['name'], 'state': state})
                result = {'tools': shown}
                if held:
                    result['blocked'] = held
                    result['note'] = '這些工具的描述與核准時不同或尚未釘住，已隱藏；請使用者到設定重新核准'
                return result
            tool = live.get(args['tool'])
            if tool is None or pin_state(pins, tool) != 'ok':
                raise ValueError('MCP 工具「' + args['tool'] + '」的描述或參數與核准時不同（或已消失），已阻擋；請使用者到設定重新測試並核准')
            result = client.call('tools/call', {'name': args['tool'], 'arguments': json.loads(args['arguments'])})
            for index, block in enumerate(result.get('content', [])[:8]):
                if block.get('type') != 'image' or block.get('mimeType') != 'image/png':
                    continue
                if len(block.get('data', '')) > 14 * 1024 * 1024:
                    raise ValueError('MCP 圖片超過 10 MiB 上限')
                data = base64.b64decode(block['data'], validate=True)
                relative = '.cca-artifacts/' + safe_id(self.request['task_id']) + '/mcp-' + hashlib.sha256(ident.encode()).hexdigest()[:16] + '-' + str(index) + '.png'
                atomic(self.project.path(relative, True), data)
                emit('Artifact', {'path': relative, 'source': config['id'], 'operation': args['tool'], 'result': 'FAIL' if result.get('isError') else 'UNKNOWN'})
                block.clear(); block.update({'type': 'text', 'text': '圖片已另存任務證據：' + relative})
            return result
        except Exception:
            client.close()
            self.clients.pop(config['id'], None)
            raise


class ResponsesAdapter:
    def __init__(self, provider, key):
        self.url, self.key = endpoint(provider['url']), key

    def request(self, model, messages, tools, cap):
        result, _ = http(self.url + '/responses', {'model': model, 'input': messages, 'tools': tools, 'store': False,
                                                'include': ['reasoning.encrypted_content'], 'max_output_tokens': cap}, self.key)
        return result.get('output', []), result.get('usage', {})


class ChatAdapter(ResponsesAdapter):
    def request(self, model, messages, tools, cap):
        chat = []
        pending_calls = []
        for m in messages:
            if m.get('type') == 'function_call':
                pending_calls.append({'id': m['call_id'], 'type': 'function', 'function': {'name': m['name'], 'arguments': m['arguments']}})
            elif m.get('type') == 'function_call_output':
                if pending_calls:
                    chat.append({'role': 'assistant', 'tool_calls': pending_calls}); pending_calls = []
                chat.append({'role': 'tool', 'tool_call_id': m['call_id'], 'content': m['output']})
            elif m.get('type') == 'message':
                chat.append({'role': 'assistant', 'content': ''.join(c.get('text', '') for c in m.get('content', []))})
            elif m.get('role'):
                copy = dict(m)
                if isinstance(copy.get('content'), list):
                    copy['content'] = [{'type': 'image_url', 'image_url': {'url': p['image_url']}} if p.get('type') == 'input_image' else p for p in copy['content']]
                chat.append(copy)
        payload = {'model': model, 'messages': chat, 'tools': [{'type': 'function', 'function': {k: v for k, v in t.items() if k != 'type'}} for t in tools], 'max_tokens': cap}
        result, _ = http(self.url + '/chat/completions', payload, self.key)
        reply = result['choices'][0]['message']
        output = [{'type': 'message', 'role': 'assistant', 'content': [{'type': 'output_text', 'text': reply.get('content') or ''}]}]
        output += [{'type': 'function_call', 'call_id': t['id'], 'name': t['function']['name'], 'arguments': t['function']['arguments']} for t in reply.get('tool_calls', [])]
        usage = result.get('usage', {})
        return output, {'input_tokens': usage.get('prompt_tokens', 0), 'output_tokens': usage.get('completion_tokens', 0)}


def run_agent(request):
    provider = request['provider']
    local = request.get('readonly', False) and provider.get('kind') == 'local'
    key = '' if local else Store(request['data_dir']).key(provider)
    adapter = ChatAdapter(provider, key) if provider.get('protocol') == 'chat' else ResponsesAdapter(provider, key)
    tools = Tools(request)
    try:
        return run_agent_turn(request, provider, local, adapter, tools)
    finally:
        tools.close()


def run_agent_turn(request, provider, local, adapter, tools):
    messages = [{'role': 'user', 'content': request['prompt']}]
    if request.get('instructions'):
        messages.insert(0, {'role': 'system', 'content': request['instructions']})
    total_in = total_out = 0
    budget = max(1, min(64000, int(request.get('max_tokens', 16000))))
    emit('TurnStarted')
    for _ in range(16):
        remaining = budget - total_in - total_out
        if remaining < 1:
            raise ValueError('BLOCKED：達到這位代理的 Token 上限')
        emit('Thinking')
        output, usage = adapter.request(request['model'], messages, tools.schemas(), min(4096, remaining))
        total_in += usage.get('input_tokens', 0); total_out += usage.get('output_tokens', 0)
        cost = (total_in * provider.get('inputRate', 0) + total_out * provider.get('outputRate', 0)) / 1e6
        emit('Usage', {'inputTokens': usage.get('input_tokens', 0), 'outputTokens': usage.get('output_tokens', 0),
                       'costUsd': (usage.get('input_tokens', 0) * provider.get('inputRate', 0) + usage.get('output_tokens', 0) * provider.get('outputRate', 0)) / 1e6,
                       'costKnown': bool(provider.get('inputRate') or provider.get('outputRate')), 'billing': 'api' if not local else 'local'})
        messages.extend(output)
        calls = [part for part in output if part.get('type') == 'function_call']
        for part in output:
            if part.get('type') == 'message':
                text = ''.join(c.get('text', '') for c in part.get('content', []) if c.get('type') == 'output_text')
                if text:
                    emit('Text', {'text': text}, 'text-' + str(len(messages)))
        if total_in + total_out >= budget or cost > request.get('max_usd', 1):
            raise ValueError('BLOCKED：達到這位代理的 Token 或費用上限')
        if not calls:
            emit('TurnDone', {'status': 'completed'}); return
        for call in calls:
            ident = call['call_id']
            args = json.loads(call['arguments'])
            emit('ToolStart', {'kind': 'read' if call['name'] in ('read_file', 'git_diff', 'list_files') else 'command', 'tool': call['name'], 'title': call['name'], 'command': ' '.join(args.get('command', [])) if call['name'] == 'sandbox_run' else '', 'path': args.get('path', '')}, ident)
            try:
                result = tools.execute(call['name'], args, ident)
                encoded = result if isinstance(result, str) else json.dumps(result, ensure_ascii=False)
                data = {'status': 'ok', 'output': encoded[:32000]}
                if call['name'] == 'sandbox_run':
                    data['exitCode'] = result['exit_code']
                    if data['exitCode']:
                        data['status'] = 'failed'
            except (ValueError, OSError, subprocess.TimeoutExpired) as error:
                encoded = str(error)
                data = {'status': 'failed', 'output': encoded[:1000]}
            emit('ToolEnd', data, ident)
            messages.append({'type': 'function_call_output', 'call_id': ident, 'output': encoded[:32000]})
            if getattr(tools, 'image', None):
                messages.append({'role': 'user', 'content': [{'type': 'input_image', 'image_url': tools.image}]})
                tools.image = None
    raise ValueError('BLOCKED：工具呼叫已達 16 輪上限')


def control(request):
    provider = request.get('provider', {})
    action = request['action']
    if action == 'key.save':
        REDACTIONS.append(request['key'])
        Store(request['data_dir']).save(provider['id'], provider['url'], request['key'])
        return {'ok': True, 'id': provider['id'], 'hasKey': True}
    if action in ('provider.test', 'provider.models'):
        result, _ = http(endpoint(provider['url']) + '/models', key=Store(request['data_dir']).key(provider))
        return {'ok': True, 'id': provider['id'], 'models': [{'id': m['id'], 'capabilities': m.get('capabilities', {})} for m in result.get('data', [])][:300]}
    if action == 'sandbox.status':
        exe = docker_exe(request.get('cwd') or None)
        if not exe:
            return {'ok': False, 'backend': 'docker', 'error': '未安裝 Docker；不會在宿主機執行沙箱命令'}
        result = subprocess.run([exe, 'info', '--format', '{{json .ServerVersion}}'], capture_output=True, timeout=10, env=clean_env(), creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        return {'ok': result.returncode == 0, 'backend': 'docker', 'version': result.stdout.decode('utf-8', 'replace').strip()}
    if action == 'mcp.test':
        client = McpClient(request['server'])
        try:
            tools = client.list_tools()
            return {'ok': True, 'id': request['server']['id'], 'tools': tools,
                    'hashes': {tool['name']: tool_fingerprint(tool) for tool in tools}}
        finally:
            client.close()
    raise ValueError('未知的管理操作')


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stdin.reconfigure(encoding='utf-8')
    try:
        if sys.version_info < (3, 10):
            raise ValueError('API 與工具工作程序需要 Python 3.10 以上')
        request = json.loads(sys.stdin.readline())
        # Never run from the project folder: Windows looks for bare program names there first.
        if request.get('data_dir') and os.path.isdir(request['data_dir']):
            os.chdir(request['data_dir'])
        if request.get('action') == 'run':
            run_agent(request)
        else:
            output({'control': control(request)})
    except Exception as error:
        # Avoid leaking arbitrary exception text or HTTP bodies that may echo credentials.
        text = str(error) if isinstance(error, ValueError) else '執行失敗，請檢查服務或工具設定'
        if 'request' in locals() and request.get('action') != 'run':
            output({'control': {'ok': False, 'error': text}})
        else:
            emit('TurnDone', {'status': 'failed', 'error': text})


if __name__ == '__main__':
    main()
