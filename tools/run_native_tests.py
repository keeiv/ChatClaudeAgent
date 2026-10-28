"""Build and run every native test target one after another and report all of them, not just the first failure.

python tools/run_native_tests.py                 all targets
python tools/run_native_tests.py test desk-test  only these build.bat targets
python tools/run_native_tests.py --skip git-test leave one out
Each target is a `build.bat <target>` that compiles its test program and runs it. Needs the Visual Studio Build Tools.
Standard library only.
"""
import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGETS = ["test", "takeover-test", "appearance-test", "desk-test", "edition-test", "profile-test", "pipeline-test",
           "collaboration-test", "quality-test", "gemini-test", "git-test", "haiku-sim-test", "memory-test"]


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser()
    parser.add_argument("targets", nargs="*")
    parser.add_argument("--skip", action="append", default=[])
    parser.add_argument("--timeout", type=int, default=900, help="seconds per target")
    args = parser.parse_args()
    targets = [t for t in (args.targets or TARGETS) if t not in args.skip]
    unknown = [t for t in targets if t not in TARGETS]
    if unknown:
        print("未知的測試目標：" + ", ".join(unknown))
        return 2
    failed = []
    for target in targets:
        started = time.time()
        try:
            result = subprocess.run(["cmd", "/c", os.path.join(ROOT, "build.bat"), target], cwd=ROOT, capture_output=True,
                                    timeout=args.timeout)
            shown = result.stdout.decode("utf-8", errors="replace")
            output = shown + result.stderr.decode("utf-8", errors="replace")
            code = result.returncode
        except subprocess.TimeoutExpired as error:
            shown = (error.stdout or b"").decode("utf-8", errors="replace")
            output = shown + f"\n逾時（{args.timeout} 秒）"
            code = -1
        # The test program's own summary line is the last thing on stdout; stderr only carries Visual Studio's noise.
        tail = [line for line in shown.strip().split("\n") if line.strip()][-1:] or [""]
        if code == 0:
            print(f"PASS {target} ({time.time() - started:.0f}s): {tail[0].strip()}", flush=True)
        else:
            failed.append(target)
            print(f"FAIL {target} (exit {code}, {time.time() - started:.0f}s)", flush=True)
            print("::group::" + target + " output" if os.environ.get("GITHUB_ACTIONS") else "---- output ----")
            print(output[-6000:])
            print("::endgroup::" if os.environ.get("GITHUB_ACTIONS") else "----------------")
    print(f"{len(targets) - len(failed)} of {len(targets)} targets passed" + (f"; failed: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
