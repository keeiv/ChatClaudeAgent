"""Host-recorded tests, two-version reproductions and isolated browser observations. JSON-lines IPC."""
import concurrent.futures
import hashlib
import io
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
import zipfile

MAX_LOG = 24000
SKIP = {'.git', '.venv', 'venv', 'node_modules', 'build', 'dist', '.next', '.vercel', '__pycache__', '.claude', '.codex'}
PRINT_LOCK = threading.Lock()
BROWSERS = {}
LOCKS = {}
STATE_LOCK = threading.Lock()


def emit(value):
    with PRINT_LOCK:
        print(json.dumps(value, ensure_ascii=False), flush=True)


def clean(text):
    text = re.sub(r'(?:gh[pousr]_[A-Za-z0-9_]{20,}|sk-[A-Za-z0-9_-]{16,}|Bearer\s+[A-Za-z0-9._~-]{16,})', '[credential hidden]', text)
    for name, value in os.environ.items():
        if len(value) > 10 and re.search(r'TOKEN|PASSWORD|SECRET|API_KEY', name, re.I):
            text = text.replace(value, '[credential hidden]')
    return text[-MAX_LOG:]


def relative(root, value):
    part = Path(value)
    if part.is_absolute() or '..' in part.parts or ':' in value or '\x00' in value or not part.parts:
        raise ValueError('需要專案內的相對路徑')
    if any(p != p.rstrip('. ') or '~' in p or p.casefold() in SKIP or p.casefold().startswith('.env') for p in part.parts):
        raise ValueError('此位置不開放給驗證工具')
    path = root / part
    for entry in [path, *path.parents]:
        if entry == root:
            break
        if entry.is_symlink() or (entry.exists() and getattr(entry.stat(follow_symlinks=False), 'st_file_attributes', 0) & 0x400):
            raise ValueError('驗證工具不跟隨連結')
    # Resolve both sides: a root given by its 8.3 short name (C:\Users\RUNNER~1\...) never matches the long names resolve() returns.
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError('路徑超出專案')
    return path


def source_files(root):
    git = shutil.which('git')
    if git and (root / '.git').exists():
        result = subprocess.run([git, '-C', str(root), 'ls-files', '-co', '--exclude-standard', '-z'],
                                capture_output=True, timeout=20, creationflags=0x08000000 if os.name == 'nt' else 0)
        if result.returncode == 0:
            names = sorted(set(result.stdout.decode('utf-8').split('\0')) - {''})
        else:
            names = None
    else:
        names = None
    if names is None:
        names = []
        for folder, dirs, files in os.walk(root, followlinks=False):
            dirs[:] = [d for d in dirs if d.casefold() not in SKIP and not (Path(folder) / d).is_symlink()]
            names.extend(str((Path(folder) / n).relative_to(root)) for n in files)
    accepted, total = [], 0
    for name in names:
        try:
            file = relative(root, name)
            if not file.is_file():
                continue
            size = file.stat().st_size
            total += size
            if size > 16 * 1024 * 1024 or total > 192 * 1024 * 1024 or len(accepted) >= 20000:
                raise RuntimeError('來源超過驗證上限，請拆成較小的專案')
            accepted.append((name.replace('\\', '/'), file))
        except ValueError:
            continue
    return sorted(accepted)


def fingerprint(root):
    digest = hashlib.sha256()
    for name, file in source_files(root):
        digest.update(name.encode('utf-8') + b'\0')
        digest.update(hashlib.sha256(file.read_bytes()).digest())
    return digest.hexdigest()


def command(value, root):
    if not isinstance(value, list) or not 1 <= len(value) <= 64 or not all(isinstance(s, str) and 0 < len(s) <= 4000 and '\x00' not in s for s in value):
        raise ValueError('命令須為 1 至 64 個參數的 JSON 陣列')
    os.environ['NoDefaultCurrentDirectoryInExePath'] = '1'
    exe = shutil.which(value[0]) if not Path(value[0]).is_absolute() else value[0]
    if not exe or not Path(exe).is_file():
        raise ValueError('找不到命令程式')
    if Path(exe).stem.casefold() in {'cmd', 'powershell', 'pwsh', 'bash', 'sh', 'wscript', 'cscript'} or Path(exe).suffix.casefold() in {'.cmd', '.bat'}:
        raise ValueError('驗證命令直接指定 python、node 或測試程式，不使用 shell')
    return [str(Path(exe).resolve()), *value[1:]]


def capture(argv, cwd, timeout=120):
    env = {k: v for k, v in os.environ.items() if not re.search(r'TOKEN|PASSWORD|SECRET|API_KEY|GIT_DIR|GIT_WORK_TREE|GIT_INDEX_FILE', k, re.I)}
    env.update(PYTHONIOENCODING='utf-8', NO_COLOR='1', GIT_TERMINAL_PROMPT='0')
    started = time.monotonic()
    proc = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            creationflags=0x08000000 if os.name == 'nt' else 0, start_new_session=os.name != 'nt')
    job, kernel = None, None
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        class Basic(ctypes.Structure):
            _fields_ = [('per_process', ctypes.c_int64), ('per_job', ctypes.c_int64), ('flags', wintypes.DWORD),
                        ('min_ws', ctypes.c_size_t), ('max_ws', ctypes.c_size_t), ('active', wintypes.DWORD),
                        ('affinity', ctypes.c_size_t), ('priority', wintypes.DWORD), ('scheduling', wintypes.DWORD)]
        class Limits(ctypes.Structure):
            _fields_ = [('basic', Basic), ('io', ctypes.c_uint64 * 6), ('memory', ctypes.c_size_t * 4)]
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateJobObjectW.restype = wintypes.HANDLE
        kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        job = kernel.CreateJobObjectW(None, None)
        limits = Limits(); limits.basic.flags = 0x2000
        if not job or not kernel.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)) or not kernel.AssignProcessToJobObject(job, int(proc._handle)):
            if job:
                kernel.CloseHandle(job)
            proc.kill(); proc.wait(timeout=5); proc.stdout.close()
            raise RuntimeError('無法建立驗證程序的清理範圍')
    chunks = bytearray()
    def read():
        while True:
            data = proc.stdout.read(4096)
            if not data:
                break
            chunks.extend(data)
            if len(chunks) > MAX_LOG * 4:
                del chunks[:-MAX_LOG * 4]
    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    timed_out = False
    try:
        proc.wait(timeout=max(1, min(int(timeout), 180)))
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.kill()
        proc.wait(timeout=10)
    finally:
        if job:
            kernel.CloseHandle(job)
        elif os.name != 'nt':
            import signal
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    reader.join(timeout=2)
    if not reader.is_alive():
        proc.stdout.close()
    return {'exitCode': proc.returncode, 'timedOut': timed_out,
            'output': clean(bytes(chunks).decode('utf-8', errors='replace')),
            'durationMs': int((time.monotonic() - started) * 1000)}


def archive(root, revision, output):
    git = shutil.which('git')
    if not git or not re.fullmatch(r'[A-Za-z0-9_./~^@{}-]{1,120}', revision or '') or revision.startswith('-'):
        raise ValueError('需要有效的 Git 基準版本')
    result = subprocess.run([git, '-C', str(root), 'rev-parse', '--verify', revision + '^{commit}'], capture_output=True, timeout=20)
    if result.returncode:
        raise ValueError('找不到基準提交')
    commit = result.stdout.decode().strip()
    if not re.fullmatch(r'[a-f0-9]{40,64}', commit):
        raise ValueError('基準版本無效')
    packed = subprocess.run([git, '-C', str(root), 'archive', '--format=zip', commit], capture_output=True, timeout=30)
    if packed.returncode or len(packed.stdout) > 192 * 1024 * 1024:
        raise ValueError('無法匯出基準版本')
    total = 0
    with zipfile.ZipFile(io.BytesIO(packed.stdout)) as files:
        for entry in files.infolist():
            if entry.is_dir():
                continue
            if ((entry.external_attr >> 16) & 0o170000) == 0o120000:
                continue
            try:
                target = relative(output, entry.filename)
            except ValueError:
                continue
            total += entry.file_size
            if entry.file_size > 16 * 1024 * 1024 or total > 192 * 1024 * 1024:
                raise ValueError('基準版本超過大小上限')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(files.read(entry))
    return commit


def reproduction(root, request):
    argv = command(request.get('command'), root)
    marker = request.get('expectedFailure', '')
    cases = request.get('cases', [])
    if not isinstance(marker, str) or not 3 <= len(marker) <= 1000 or not isinstance(cases, list) or not 1 <= len(cases) <= 20:
        raise ValueError('需要相同的重現測試檔案與至少 3 字元的預期失敗訊息')
    sources = [(str(Path(p)).replace('\\', '/'), relative(root, p)) for p in cases]
    if any(not f.is_file() or f.stat().st_size > 256000 for _, f in sources):
        raise ValueError('重現測試檔案不存在或超過 256 KB')
    with tempfile.TemporaryDirectory(prefix='cca-repro-') as folder:
        old, new = Path(folder) / 'before', Path(folder) / 'after'
        old.mkdir(); new.mkdir()
        base = archive(root, request.get('base', ''), old)
        for name, file in source_files(root):
            target = new / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(file, target)
        # Exactly the same regression files are run against both source versions.
        for name, file in sources:
            content = file.read_bytes()
            for dest in (old, new):
                target = relative(dest, name)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(content)
        before = capture(argv, old, request.get('timeout', 120))
        after = capture(argv, new, request.get('timeout', 120))
        passed = before['exitCode'] != 0 and not before['timedOut'] and marker in before['output'] and after['exitCode'] == 0 and not after['timedOut']
        return {'passed': passed, 'baseCommit': base, 'before': before, 'after': after,
                'output': '修正前：\n' + before['output'] + '\n修正後：\n' + after['output'],
                'exitCode': after['exitCode'], 'cases': [n for n, _ in sources],
                'note': '已在兩份暫存來源執行相同案例；沒有複製 node_modules、虛擬環境或機密設定。'}


class Browser:
    def __init__(self, request):
        node = shutil.which('node')
        script = request.get('browserScript', '')
        if not node or not Path(script).is_file():
            raise ValueError('需要 Node.js 與瀏覽器工作程序')
        self.proc = subprocess.Popen([node, script], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, encoding='utf-8',
                                     creationflags=0x08000000 if os.name == 'nt' else 0)
        self.replies = queue.Queue()
        def read():
            for line in self.proc.stdout:
                try:
                    self.replies.put(json.loads(line))
                except ValueError:
                    pass
        threading.Thread(target=read, daemon=True).start()

    def call(self, request):
        self.proc.stdin.write(json.dumps(request, ensure_ascii=False) + '\n')
        self.proc.stdin.flush()
        return self.replies.get(timeout=50)

    def close(self):
        self.proc.kill()
        self.proc.wait(timeout=10)


def run(request):
    root = Path(request['cwd']).resolve(strict=True)
    if not root.is_dir() or root.parent == root or root == Path.home():
        raise ValueError('需要專案資料夾')
    action = request.get('action')
    binding = fingerprint(root)
    if action == 'fingerprint':
        return {'binding': binding}
    paths = request.get('paths', [])
    if not isinstance(paths, list) or len(paths) > 40:
        raise ValueError('最多對應 40 個檔案')
    mapped = []
    for name in paths:
        file = relative(root, name)
        if not file.is_file():
            raise ValueError('對應的檔案不存在')
        mapped.append({'path': name, 'sha256': hashlib.sha256(file.read_bytes()).hexdigest()})
    if action == 'browser':
        ident = request['convId']
        with STATE_LOCK:
            if ident not in BROWSERS or BROWSERS[ident].proc.poll() is not None:
                BROWSERS[ident] = Browser(request)
            browser = BROWSERS[ident]
        result = browser.call(request)
        if result.get('error'):
            raise ValueError(result['error'])
    elif action == 'repro':
        result = reproduction(root, request)
    elif action == 'test':
        result = capture(command(request.get('command'), root), root, request.get('timeout', 120))
        result['passed'] = result['exitCode'] == 0 and not result['timedOut']
    else:
        raise ValueError('未知驗證動作')
    after = fingerprint(root)
    result['paths'] = mapped
    result.update(binding=binding, currentBinding=after, sourceChanged=binding != after,
                  environment={'platform': sys.platform, 'python': sys.version.split()[0]})
    if binding != after:
        result['passed'] = False
        result['note'] = '驗證期間來源已改變，結果不可作為目前版本的通過證據'
    return result


def handle(request):
    token, conv = request.get('requestToken'), request.get('convId', '')
    with STATE_LOCK:
        lock = LOCKS.setdefault(conv, threading.Lock())
    with lock:
        try:
            result = run(request)
            emit({'requestToken': token, 'convId': conv, 'ok': True, 'result': result})
        except Exception as error:
            emit({'requestToken': token, 'convId': conv, 'ok': False, 'error': clean(str(error))})


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stdin.reconfigure(encoding='utf-8')
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        for line in sys.stdin:
            if len(line) > 200000:
                continue
            try:
                request = json.loads(line)
                pool.submit(handle, request)
            except ValueError:
                continue
    for browser in BROWSERS.values():
        browser.close()
