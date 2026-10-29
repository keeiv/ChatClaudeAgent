"""Build and run every native test target one after another and report all of them, not just the first failure.

python tools/run_native_tests.py                 all targets
python tools/run_native_tests.py test desk-test  only these build.bat targets
python tools/run_native_tests.py --skip git-test leave one out
python tools/run_native_tests.py --asan          every target built with AddressSanitizer (leaves ASan objects in build/obj)
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
    parser.add_argument("--asan", action="store_true", help="build every target with MSVC AddressSanitizer (CCA_SANITIZE=1)")
    args = parser.parse_args()
    skip = list(args.skip)
    if args.asan:
        # gemini-test points PATH at an empty folder and copies the tested program there as node.exe; the AddressSanitizer
        # build cannot find its runtime DLLs from there, so the probe never starts. Its protocol code is covered by the
        # other targets' sanitizer runs.
        skip.append("gemini-test")
        os.environ["CCA_SANITIZE"] = "1"
        # Objects from a normal build were compiled with /MT; the sanitizer needs /MD.
        obj = os.path.join(ROOT, "build", "obj")
        for name in os.listdir(obj) if os.path.isdir(obj) else []:
            if name.endswith(".obj"):
                os.remove(os.path.join(obj, name))
    targets = [t for t in (args.targets or TARGETS) if t not in skip]
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
            # An AddressSanitizer report starts well before the end of the output; show it from its first line.
            report = output.find("ERROR: AddressSanitizer")
            print(output[max(0, report - 200):][:9000] if report != -1 else output[-6000:])
            print("::endgroup::" if os.environ.get("GITHUB_ACTIONS") else "----------------")
    print(f"{len(targets) - len(failed)} of {len(targets)} targets passed" + (f"; failed: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
