"""Stdio MCP server that gives Claude Code and Codex the same Docker sandbox (sandbox_run, sandbox_status).

CCA registers it for both agents so neither depends on Codex's own Windows sandbox. It reuses
agent_runtime.run_sandbox: source copy without .env/.git, no network, read-only root, non-root user,
and never a fallback to running on the host.
"""
import argparse
import itertools
import json
import os
from pathlib import Path
import shutil
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
import agent_runtime as rt  # noqa: E402

PROTOCOLS = ('2025-06-18', '2025-03-26', '2024-11-05')
MAX_LINE = 1024 * 1024
MAX_COMMAND_ITEMS = 64
MAX_ARGUMENT = 8192
MAX_OUTPUT_FILES = 8
MAX_OUTPUT_FILE_TEXT = 8 * 1024

TOOLS = [
    {'name': 'sandbox_status',
     'description': '檢查 CCA 的 Docker 沙箱是否可用，並列出本機已有的容器映像。沙箱不會自動下載映像。',
     'inputSchema': {'type': 'object', 'properties': {}, 'additionalProperties': False}},
    {'name': 'sandbox_run',
     'description': '在一次性的 Docker 容器裡執行命令：沒有網路、根檔案系統唯讀、非 root、上限 1 CPU／512 MB／120 秒。'
                    '專案原始檔以唯讀方式放在 /input（不含 .env 與 .git），可寫的只有 /tmp 與 /output。'
                    '映像必須已存在本機，先用 sandbox_status 查看。command 是參數陣列，不經過 shell；要用管線請寫成 '
                    '["sh", "-c", "..."]。/output 底下的小型文字檔會附在結果裡。',
     'inputSchema': {'type': 'object',
                     'properties': {'image': {'type': 'string', 'description': '本機已有的映像，例如 python:3.12-slim'},
                                    'command': {'type': 'array', 'items': {'type': 'string'}}},
                     'required': ['image', 'command'], 'additionalProperties': False}},
]


class Server:
    def __init__(self, project, data_dir):
        self.project = rt.Project(project)
        self.data_dir = Path(data_dir)
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.session = uuid.uuid4().hex[:8]

    def status(self):
        docker = rt.docker_exe(self.project.root)
        if not docker:
            raise ValueError('本機沒有 Docker；沙箱不會改在宿主機執行。請使用者安裝 Docker Desktop。')
        try:
            version = rt.capture([docker, 'version', '--format', '{{.Server.Version}}'], timeout=15)
        except (OSError, ValueError) as error:
            raise ValueError('Docker 沒有回應：%s。請使用者啟動 Docker Desktop。' % error) from None
        if version['exit_code']:
            raise ValueError('Docker 引擎沒有在執行（%s）。請使用者啟動 Docker Desktop。' % version['error'].strip()[:200])
        images = rt.capture([docker, 'images', '--format', '{{.Repository}}:{{.Tag}}'], timeout=15)
        names = sorted({line.strip() for line in images['output'].splitlines() if line.strip() and '<none>' not in line})
        return {'ready': True, 'engine': version['output'].strip(), 'images': names,
                'note': '' if names else '本機沒有任何映像；請使用者先執行 docker pull <映像>。'}

    def run(self, args):
        image, command = args.get('image'), args.get('command')
        if not isinstance(command, list) or len(command) > MAX_COMMAND_ITEMS or \
                any(isinstance(item, str) and len(item) > MAX_ARGUMENT for item in command):
            raise ValueError('command 必須是參數陣列（最多 %d 項、每項 %d 字元）' % (MAX_COMMAND_ITEMS, MAX_ARGUMENT))
        owner = 'mcp-%s-%s' % (self.session, uuid.uuid4().hex[:8])
        result = rt.run_sandbox(self.project, image, command, str(self.data_dir), owner)
        job = Path(result.pop('artifact_directory'))
        files = {}
        try:
            for path in itertools.islice(job.rglob('*'), 200):
                if len(files) >= MAX_OUTPUT_FILES:
                    break
                if path.is_file() and not path.is_symlink() and not getattr(path.stat(follow_symlinks=False), 'st_file_attributes', 0) & 0x400:
                    with path.open('rb') as stream:
                        data = stream.read(MAX_OUTPUT_FILE_TEXT + 1)
                    files[path.relative_to(job).as_posix()] = data[:MAX_OUTPUT_FILE_TEXT].decode('utf-8', 'replace') + \
                        ('…（已截斷）' if len(data) > MAX_OUTPUT_FILE_TEXT else '')
        finally:
            # The container is already gone; its throwaway copy of the source must not pile up.
            shutil.rmtree(job.parent, ignore_errors=True)
        result['output_files'] = files
        error = result.get('error', '')
        if result['exit_code'] == 125 and 'Unable to find image' in error:
            result['hint'] = '本機沒有這個映像；沙箱不會自動下載，請使用者先執行 docker pull %s' % image
        elif result['exit_code'] == 125 and ('daemon' in error or 'pipe' in error):
            result['hint'] = 'Docker 引擎沒有在執行，請使用者啟動 Docker Desktop'
        return result

    def call(self, name, args):
        try:
            if args is None:
                args = {}
            if not isinstance(args, dict):
                raise ValueError('工具參數必須是 JSON 物件')
            allowed = {'image', 'command'} if name == 'sandbox_run' else set()
            if set(args) - allowed:
                raise ValueError('工具參數包含未支援的欄位')
            if name == 'sandbox_status':
                value = self.status()
            elif name == 'sandbox_run':
                value = self.run(args)
            else:
                raise ValueError('沒有這個工具：%s' % name)
        except (ValueError, OSError, RuntimeError) as error:
            return {'content': [{'type': 'text', 'text': str(error)}], 'isError': True}
        return {'content': [{'type': 'text', 'text': json.dumps(value, ensure_ascii=False)}],
                'isError': value.get('exit_code', 0) != 0}

    def handle(self, message):
        method, ident = message.get('method'), message.get('id')
        params = message.get('params')
        if params is None:
            params = {}
        if not isinstance(params, dict):
            raise ValueError('params must be an object')
        if method == 'initialize':
            wanted = params.get('protocolVersion')
            return {'protocolVersion': wanted if wanted in PROTOCOLS else PROTOCOLS[1],
                    'capabilities': {'tools': {}}, 'serverInfo': {'name': 'cca-sandbox', 'version': '1'}}
        if method == 'ping':
            return {}
        if method == 'tools/list':
            return {'tools': TOOLS}
        if method == 'tools/call':
            return self.call(params.get('name'), params.get('arguments'))
        if ident is None:
            return None
        raise LookupError(method)


def serve(server, source, sink):
    while True:
        line = source.readline(MAX_LINE + 1)
        if not line:
            return
        if len(line) > MAX_LINE:
            return
        try:
            message = json.loads(line)
        except ValueError:
            continue
        if not isinstance(message, dict):
            continue
        ident = message.get('id')
        try:
            result = server.handle(message)
            reply = None if ident is None else {'jsonrpc': '2.0', 'id': ident, 'result': result}
        except LookupError:
            reply = {'jsonrpc': '2.0', 'id': ident, 'error': {'code': -32601, 'message': 'Method not found'}}
        except ValueError:
            reply = None if ident is None else {'jsonrpc': '2.0', 'id': ident, 'error': {'code': -32602, 'message': 'Invalid params'}}
        if reply is not None:
            sink.write(json.dumps(reply) + '\n')
            sink.flush()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--project', required=True)
    parser.add_argument('--data-dir', required=True)
    parser.add_argument('--docker', default='')
    options = parser.parse_args()
    if options.docker and os.path.isabs(options.docker) and os.path.isfile(options.docker):
        # The host that started us may hand MCP servers a trimmed environment; use the docker.exe CCA found,
        # by its full path, instead of searching for a bare name.
        os.environ['CCA_DOCKER'] = options.docker
    sys.stdin.reconfigure(encoding='utf-8')
    server = Server(options.project, options.data_dir)
    # Claude Code and Codex start MCP servers in the project folder; Windows would look for programs there first.
    os.chdir(server.data_dir)
    serve(server, sys.stdin, sys.stdout)


if __name__ == '__main__':
    main()
