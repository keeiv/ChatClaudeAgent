"""Compare shipped desktop/runtime resources with source, without executing the EXEs."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import re
import struct


class Resources:
    def __init__(self, path):
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.kernel.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
        self.kernel.LoadLibraryExW.restype = wintypes.HMODULE
        self.kernel.FindResourceW.argtypes = [wintypes.HMODULE, wintypes.LPCWSTR, ctypes.c_void_p]
        self.kernel.FindResourceW.restype = wintypes.HANDLE
        self.kernel.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
        self.kernel.LoadResource.restype = wintypes.HANDLE
        self.kernel.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
        self.kernel.SizeofResource.restype = wintypes.DWORD
        self.kernel.LockResource.argtypes = [wintypes.HANDLE]
        self.kernel.LockResource.restype = ctypes.c_void_p
        self.kernel.FreeLibrary.argtypes = [wintypes.HMODULE]
        self.handle = self.kernel.LoadLibraryExW(str(path.resolve()), None, 2)
        if not self.handle:
            raise OSError(ctypes.get_last_error(), 'Cannot load PE resources')

    def get(self, name, kind=10):
        if isinstance(name, int):
            name = ctypes.cast(ctypes.c_void_p(name), wintypes.LPCWSTR)
        resource = self.kernel.FindResourceW(self.handle, name, ctypes.c_void_p(kind))
        if not resource:
            return None
        size = self.kernel.SizeofResource(self.handle, resource)
        pointer = self.kernel.LockResource(self.kernel.LoadResource(self.handle, resource))
        return ctypes.string_at(pointer, size)

    def close(self):
        self.kernel.FreeLibrary(self.handle)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--desktop', type=Path, required=True)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    worker = (root / 'tools/agent_runtime.py').read_bytes()
    sandbox = (root / 'tools/sandbox_mcp.py').read_bytes()
    results = []
    icon = (root / 'assets/cca.ico').read_bytes()
    icon_count = struct.unpack_from('<HHH', icon)[2]
    for kind, file in [('desktop', args.desktop), ('cli', args.cli)]:
        resources = Resources(file)
        try:
            assert resources.get('RUNTIME_AGENT_PY') == worker, kind + ': worker source mismatch'
            assert resources.get('GEMINI_ACCOUNT_JS') == (root / 'tools/gemini_account.cjs').read_bytes(), kind + ': Gemini account worker mismatch'
            assert resources.get('SANDBOX_MCP_PY') == sandbox, kind + ': sandbox server source mismatch'
            assert resources.get('QUALITY_WORKER_PY') == (root / 'tools/quality_worker.py').read_bytes(), kind + ': quality worker mismatch'
            assert resources.get('BROWSER_WORKER_JS') == (root / 'tools/browser_worker.cjs').read_bytes(), kind + ': browser worker mismatch'
            assert resources.get('UI_MOCK_JS') is None, kind + ': mock host shipped'
            group = resources.get(101, 14)
            assert group and struct.unpack_from('<HHH', group)[2] == icon_count, kind + ': icon group mismatch'
            icon_sizes = []
            for i in range(icon_count):
                width, height, _, _, _, _, length, offset = struct.unpack_from('<BBBBHHII', icon, 6 + 16 * i)
                entry = struct.unpack_from('<BBBBHHIH', group, 6 + 14 * i)
                assert (width, height) == entry[:2], kind + ': embedded icon size mismatch'
                assert resources.get(entry[-1], 3) == icon[offset:offset + length], kind + ': embedded icon pixels mismatch'
                icon_sizes.append(width or 256)
            if kind == 'desktop':
                for source in (root / 'ui').rglob('*'):
                    if source.is_file() and source.name != 'mock.js':
                        name = 'UI_' + re.sub('[^A-Z0-9]', '_', source.relative_to(root / 'ui').as_posix().upper())
                        assert resources.get(name) == source.read_bytes(), name + ': source mismatch'
            else:
                assert resources.get('UI_APP_JS') is None, 'CLI unexpectedly contains desktop UI'
            data = file.read_bytes()
            results.append({'file': file.name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                            'runtime_matches': True, 'mock_host': False, 'icon_sizes': icon_sizes})
        finally:
            resources.close()
    text = json.dumps(results, indent=2) + '\n'
    if args.output:
        args.output.write_text(text, encoding='utf-8', newline='\n')
    print(text, end='')


if __name__ == '__main__':
    main()
