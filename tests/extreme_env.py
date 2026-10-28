"""Run CCA under hostile conditions and check it degrades instead of crashing or hanging.

Build tests, cli, pty and takeover-test first. Uses isolated data folders only; no network, no model quota.
Scenarios: a hard memory cap (Windows job object), one CPU at idle priority, a stripped environment, non-ASCII / spaced /
emoji / very long folder names, corrupt or hostile data files, a data folder that cannot be written, a second instance
on the same data, and tiny or huge terminals.
python tests\\extreme_env.py            run everything
python tests\\extreme_env.py -k cap     only scenarios whose name contains "cap"
"""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
sys.stdout.reconfigure(encoding="utf-8")

CREATE_SUSPENDED = 0x4
JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x100
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
JobObjectExtendedLimitInformation = 9
CRASH_CODES = {0xC0000005, 0xC00000FD, 0xC0000409, 0xC0000374, 0x80000003, 0xC0000142}


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in ("a", "b", "c", "d", "e", "f")]


class BASIC(ctypes.Structure):
    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong),
                ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]


class EXTENDED(ctypes.Structure):
    _fields_ = [("Basic", BASIC), ("Io", IO_COUNTERS), ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcess", ctypes.c_size_t), ("PeakJob", ctypes.c_size_t)]


kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
kernel32.CreateJobObjectW.restype = wintypes.HANDLE
kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
kernel32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
kernel32.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
kernel32.SetPriorityClass.argtypes = [wintypes.HANDLE, wintypes.DWORD]
ntdll.NtResumeProcess.argtypes = [wintypes.HANDLE]


class Failure(Exception):
    pass


def require(condition, message):
    if not condition:
        raise Failure(message)


def start(command, env, cwd, memory_limit=None, one_cpu=False):
    """Starts a process suspended, applies the limits, then lets it run, so its children inherit them."""
    flags = CREATE_SUSPENDED if (memory_limit or one_cpu) else 0
    proc = subprocess.Popen(command, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=flags)
    if flags:
        handle = int(proc._handle)
        if memory_limit:
            job = kernel32.CreateJobObjectW(None, None)
            info = EXTENDED()
            info.Basic.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            info.ProcessMemoryLimit = memory_limit
            require(kernel32.SetInformationJobObject(job, JobObjectExtendedLimitInformation, ctypes.byref(info),
                                                     ctypes.sizeof(info)), "cannot set the memory limit")
            require(kernel32.AssignProcessToJobObject(job, handle), f"cannot assign the job ({ctypes.get_last_error()})")
            proc._job = job
        if one_cpu:
            kernel32.SetProcessAffinityMask(handle, 1)
            kernel32.SetPriorityClass(handle, 0x40)  # IDLE_PRIORITY_CLASS
        ntdll.NtResumeProcess(handle)
    return proc


def finish(proc, timeout):
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
        raise Failure(f"hung for more than {timeout} s")
    return proc.returncode, out.decode("utf-8", errors="replace")


def crashed(code):
    return (code & 0xFFFFFFFF) in CRASH_CODES


def clean_env(**extra):
    env = dict(os.environ)
    for key in ("CCA_MOCK", "CCA_TEST_FAILURE", "CCA_TEST_WRITE", "CCA_TEST_FLOW"):
        env.pop(key, None)
    env.update(extra)
    return env


def cli_session(work, data, project, script="until /help 20000\nwait 400\ntype /exit\nkey enter\n", cols=100, rows=34,
                env=None, memory_limit=None, timeout=90, name="s"):
    """Runs the CLI in a pseudoconsole through pty_run and returns (exit code of the CLI, screen bytes, driver log)."""
    work.mkdir(parents=True, exist_ok=True)
    script_path = work / f"{name}.txt"
    script_path.write_text(script, encoding="utf-8")
    screen = work / f"{name}.vt"
    env = env or clean_env()
    env.setdefault("CCA_DATA_DIR", str(data))
    env.setdefault("CCA_TEST_CLI_FIXTURE", "1")
    command = [str(BUILD / "pty_run.exe"), str(cols), str(rows), str(screen), str(script_path),
               str(BUILD / "ChatClaudeAgent-CLI.exe")]
    proc = start(command, env, str(project), memory_limit=memory_limit)
    _, log = finish(proc, timeout)
    match = re.search(r"exit code (\d+)", log)
    code = int(match.group(1)) if match else -1
    return code, screen.read_bytes() if screen.exists() else b"", log


def basic_settings(data):
    data.mkdir(parents=True, exist_ok=True)
    settings = {"claudePath": str(ROOT / "tests/fake_claude.cmd"), "codexPath": str(BUILD / "takeover_test.exe"),
                "guard": {"enabled": False, "retries": 2}}
    (data / "settings.json").write_text(json.dumps(settings), encoding="utf-8")


# ---------------------------------------------------------------- native unit tests under pressure

def unit_run(env=None, memory_limit=None, one_cpu=False, cwd=None, timeout=300):
    proc = start([str(BUILD / "tests.exe")], env or clean_env(), cwd or str(ROOT), memory_limit=memory_limit,
                 one_cpu=one_cpu)
    code, out = finish(proc, timeout)
    require(not crashed(code), f"tests.exe crashed with 0x{code & 0xFFFFFFFF:X}\n{out[-600:]}")
    match = re.search(r"(\d+) passed, (\d+) failed", out)
    require(match, f"no summary line\n{out[-600:]}")
    require(code == 0 and match.group(2) == "0", f"{match.group(2)} unit checks failed\n{out[-1200:]}")
    return int(match.group(1))


def sc_unit_memory_cap(work):
    return f"{unit_run(memory_limit=512 << 20)} checks passed with a 512 MiB per-process cap"


def sc_unit_one_cpu(work):
    return f"{unit_run(one_cpu=True)} checks passed on one CPU at idle priority"


def sc_unit_stripped_env(work):
    root = os.environ.get("SystemRoot", r"C:\Windows")
    temp = work / "tmp"
    temp.mkdir(parents=True, exist_ok=True)
    env = {"SystemRoot": root, "windir": root, "TEMP": str(temp), "TMP": str(temp), "PATH": rf"{root}\System32"}
    return f"{unit_run(env=env)} checks passed with only SystemRoot, TEMP and a bare PATH"


def sc_unit_odd_temp(work):
    temp = work / "暫存 資料夾 ünï 😀 with spaces"
    temp.mkdir(parents=True, exist_ok=True)
    env = clean_env(TEMP=str(temp), TMP=str(temp))
    return f"{unit_run(env=env)} checks passed with TEMP in a CJK / emoji / spaced folder"


def sc_unit_locale(work):
    env = clean_env(LANG="C", LC_ALL="C", PYTHONIOENCODING="ascii")
    return f"{unit_run(env=env)} checks passed with a C locale"


# ---------------------------------------------------------------- CLI under pressure

def sc_cli_version_tiny_cap(work):
    proc = start([str(BUILD / "ChatClaudeAgent-CLI.exe"), "--version"], clean_env(), str(work), memory_limit=48 << 20)
    code, out = finish(proc, 30)
    require(code == 0 and "ChatClaudeAgent" in out, f"--version under a 48 MiB cap failed ({code}): {out[:200]}")
    return "starts and prints its version inside 48 MiB"


def sc_cli_memory_cap(work):
    data, project = work / "data", work / "project"
    project.mkdir(parents=True)
    basic_settings(data)
    code, screen, log = cli_session(work, data, project, memory_limit=256 << 20)
    require(code == 0 and b"ChatClaudeAgent" in screen, f"exit {code} under a 256 MiB cap\n{log[-400:]}")
    return "full session in a pseudoconsole inside 256 MiB"


def sc_cli_odd_paths(work):
    names = ["中文 資料夾", "data with  double  spaces", "emoji 😀 目錄", "ünï-cödé", "trailing dot.d"]
    for index, label in enumerate(names):
        data = work / label / "數據 data"
        project = work / label / "專案 project"
        project.mkdir(parents=True)
        basic_settings(data)
        code, screen, log = cli_session(work / f"case{index}", data, project, name="odd")
        require(code == 0 and b"ChatClaudeAgent" in screen, f"{label!r}: exit {code}\n{log[-400:]}")
        require((data / "settings.json").is_file(), f"{label!r}: settings.json vanished")
    return f"{len(names)} awkward folder names worked for both data and project"


def sc_cli_long_paths(work):
    segment = "長" * 20 + "x" * 20
    inner = work
    while len(str(inner)) < 170:
        inner = inner / segment
    data, project = inner / "d", inner / "p"
    prefix = "\\\\?\\"
    for path in (data, project):
        os.makedirs(prefix + str(path), exist_ok=True)
    basic_settings(Path(prefix + str(data)))
    code, screen, log = cli_session(work / "long", data, project, name="long")
    require(not crashed(code) and code in (0, 1), f"170+ character path: exit {code}\n{log[-400:]}")
    # Past MAX_PATH the app may refuse, but it must say so and exit rather than crash or hang.
    over = work
    while len(str(over)) < 300:
        over = over / segment
    os.makedirs(prefix + str(over / "d"), exist_ok=True)
    os.makedirs(prefix + str(over / "p"), exist_ok=True)
    code, screen, log = cli_session(work / "over", over / "d", ROOT, name="over", timeout=60)
    require(not crashed(code), f"data path over 300 characters crashed with 0x{code & 0xFFFFFFFF:X}\n{log[-400:]}")
    return "paths of 170 and 300+ characters: no crash, no hang"


def hostile_files(data, shift):
    """Rewrites every JSON file the app made into something nasty; returns how many were touched."""
    files = sorted(data.rglob("*.json"))
    garbage = [b"", b"\x00" * 4096, b"\xff\xfe\xfd not utf-8 \xc3\x28", b"[" * 300000, b"{" * 50000,
               b'{"profiles": 5, "guard": "yes", "claudePath": []}', b"\xef\xbb\xbf" + b'{"a":' * 20,
               b'{"version": 99999999999999999999999999, "x": "\\ud800"}', b"null", b"[]", b"1e999999",
               b'"' + b"a" * (6 << 20) + b'"']
    for index, path in enumerate(files):
        path.write_bytes(garbage[(index + shift) % len(garbage)])
    return len(files)


def sc_cli_hostile_data(work):
    data, project = work / "data", work / "project"
    project.mkdir(parents=True)
    basic_settings(data)
    code, _, log = cli_session(work / "first", data, project, name="first")
    require(code == 0, f"first run failed ({code})\n{log[-300:]}")
    touched = 0
    for round_index in range(4):
        # Each round shifts which file gets which payload, so all twelve forms hit every file.
        touched = max(touched, hostile_files(data, round_index * 3))
        require(touched > 0, "the first run left no JSON files to corrupt")
        code, screen, log = cli_session(work / f"r{round_index}", data, project, name="again", timeout=60)
        require(not crashed(code) and code in (0, 1), f"round {round_index}: crashed or odd exit {code}\n{log[-500:]}")
        # The app may start with defaults; it must not leave itself stuck.
        shutil.rmtree(data / "messages", ignore_errors=True)
    for variant in (b"", b"\x00\x00", b"{", b"\xff" * 100):
        (data / "settings.json").write_bytes(variant)
        code, _, log = cli_session(work / "v", data, project, name="variant", timeout=60)
        require(not crashed(code) and code in (0, 1), f"settings.json={variant[:6]!r}: exit {code}\n{log[-300:]}")
    return f"corrupt settings and {touched} stored files in 12 forms: no crash, no hang"


def sc_cli_unwritable_data(work):
    data, project = work / "data", work / "project"
    project.mkdir(parents=True)
    basic_settings(data)
    code, _, log = cli_session(work / "warm", data, project, name="warm")
    require(code == 0, f"warm-up run failed ({code})\n{log[-300:]}")
    user = os.environ.get("USERNAME", "")
    require(user, "USERNAME is not set")
    denied = subprocess.run(["icacls", str(data), "/deny", f"{user}:(OI)(CI)(WD,AD,WEA,WA,DC,D)"], capture_output=True)
    require(denied.returncode == 0, f"icacls deny failed: {denied.stdout.decode(errors='replace')}")
    try:
        (data / "probe.tmp").write_text("x")
        subprocess.run(["icacls", str(data), "/remove:d", user], capture_output=True)
        raise Failure("the deny rule did not block writing, so this scenario would prove nothing")
    except PermissionError:
        pass
    try:
        code, screen, log = cli_session(work / "ro", data, project, name="ro", timeout=60)
    finally:
        subprocess.run(["icacls", str(data), "/remove:d", user], capture_output=True)
    require(not crashed(code), f"write-protected data folder crashed with 0x{code & 0xFFFFFFFF:X}\n{log[-500:]}")
    return f"write-protected data folder: exit {code}, no crash"


def sc_cli_second_instance(work):
    data, project = work / "data", work / "project"
    project.mkdir(parents=True)
    basic_settings(data)
    first_script = work / "first.txt"
    first_script.write_text("until /help 20000\nwait 7000\ntype /exit\nkey enter\n", encoding="utf-8")
    env = clean_env(CCA_DATA_DIR=str(data), CCA_TEST_CLI_FIXTURE="1")
    first = start([str(BUILD / "pty_run.exe"), "100", "34", str(work / "first.vt"), str(first_script),
                   str(BUILD / "ChatClaudeAgent-CLI.exe")], env, str(project))
    try:
        time.sleep(4)
        code, screen, log = cli_session(work / "second", data, project, script="wait 1500\n", name="second", timeout=60)
    finally:
        first_code, first_log = finish(first, 90)
    require("正在使用同一份對話資料".encode() in screen, f"second instance was not refused\n{log[-300:]}")
    require(code != 0, "second instance exited 0 although it was refused")
    require("exit code 0" in first_log, f"first instance disturbed by the second\n{first_log[-300:]}")
    return "second instance refused with a message; the first kept running"


def sc_cli_terminal_sizes(work):
    data, project = work / "data", work / "project"
    project.mkdir(parents=True)
    basic_settings(data)
    for cols, rows in ((20, 5), (30, 8), (300, 100), (500, 200)):
        code, screen, log = cli_session(work / f"{cols}x{rows}", data, project,
                                        script="wait 1500\ntype /help\nkey enter\nwait 600\ntype /exit\nkey enter\n",
                                        cols=cols, rows=rows, name="size", timeout=60)
        require(not crashed(code) and code in (0, 1), f"{cols}x{rows}: exit {code}\n{log[-300:]}")
    return "20x5, 30x8, 300x100 and 500x200 terminals: no crash"


def sc_cli_input_flood(work):
    data, project = work / "data", work / "project"
    project.mkdir(parents=True)
    basic_settings(data)
    lines = ["until /help 20000", "wait 400"]
    lines += ["type " + ("中文 🙂 " * 200)] + ["key backspace"] * 40 + ["key enter"]
    lines += ["type " + "x" * 8000, "key enter"]
    lines += ["type /nonexistent " + "y" * 3000, "key enter", "wait 600", "type /exit", "key enter"]
    code, screen, log = cli_session(work, data, project, script="\n".join(lines) + "\n", name="flood", timeout=120)
    require(not crashed(code), f"input flood crashed with 0x{code & 0xFFFFFFFF:X}\n{log[-400:]}")
    return "very long and odd input lines: no crash"


SCENARIOS = [
    ("unit-memory-cap", sc_unit_memory_cap),
    ("unit-one-cpu", sc_unit_one_cpu),
    ("unit-stripped-env", sc_unit_stripped_env),
    ("unit-odd-temp", sc_unit_odd_temp),
    ("unit-c-locale", sc_unit_locale),
    ("cli-version-tiny-cap", sc_cli_version_tiny_cap),
    ("cli-memory-cap", sc_cli_memory_cap),
    ("cli-odd-paths", sc_cli_odd_paths),
    ("cli-long-paths", sc_cli_long_paths),
    ("cli-hostile-data", sc_cli_hostile_data),
    ("cli-unwritable-data", sc_cli_unwritable_data),
    ("cli-second-instance", sc_cli_second_instance),
    ("cli-terminal-sizes", sc_cli_terminal_sizes),
    ("cli-input-flood", sc_cli_input_flood),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-k", default="", help="only scenarios whose name contains this text")
    args = parser.parse_args()
    for binary in ("tests.exe", "ChatClaudeAgent-CLI.exe", "pty_run.exe", "takeover_test.exe"):
        require((BUILD / binary).is_file(), f"build {binary} first")
    base = Path(tempfile.mkdtemp(prefix="cca-extreme-"))
    failed = 0
    ran = 0
    try:
        for name, body in SCENARIOS:
            if args.k not in name:
                continue
            ran += 1
            work = base / name
            work.mkdir()
            started = time.time()
            try:
                note = body(work)
                print(f"PASS {name} ({time.time() - started:.0f}s): {note}", flush=True)
            except Failure as error:
                failed += 1
                print(f"FAIL {name}: {error}", flush=True)
            except Exception as error:  # a harness bug must not look like a pass
                failed += 1
                print(f"FAIL {name}: {type(error).__name__}: {error}", flush=True)
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print(f"{ran - failed} of {ran} scenarios passed")
    return 1 if failed or not ran else 0


if __name__ == "__main__":
    sys.exit(main())
