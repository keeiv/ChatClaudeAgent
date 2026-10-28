"""Run the DOM and browser tests against one local UI server, so CI and developers use the same command.

Needs Node.js, the `playwright` package (see docs/ci.md for the pinned install), Microsoft Edge, and
`linkedom` in build/workflow-qa. The UI server (tools/serve_ui.js) is started here on a free port and every test gets it
through CCA_UI_URL. The browsers run headless; no model quota and no network beyond loopback.
python tests/run_browser_tests.py            all
python tests/run_browser_tests.py guard desk only tests whose file name contains one of these words
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Pure DOM or VM tests first (no browser), then the Edge ones.
TESTS = ["markdown_links.cjs", "profile_media.cjs", "desktop_smoke.cjs", "workflow_ui.cjs", "guard_browser.cjs",
         "desk_browser.cjs", "appearance_browser.cjs", "message_bounds_browser.cjs", "platform_browser.cjs",
         "collaboration_browser.cjs", "mcp_pin_browser.cjs", "activity_browser.cjs", "badges_browser.cjs",
         "group_navigation_browser.cjs", "pipeline_models_browser.cjs", "git_browser.cjs", "gemini_browser.cjs",
         "visual_identity_browser.cjs", "editions_browser.cjs"]


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    words = sys.argv[1:]
    tests = [t for t in TESTS if not words or any(w in t for w in words)]
    tests = [t for t in tests if os.path.isfile(os.path.join(ROOT, "tests", t))]
    port = free_port()
    env = dict(os.environ, PORT=str(port), CCA_UI_URL=f"http://127.0.0.1:{port}/")
    server = subprocess.Popen(["node", os.path.join(ROOT, "tools", "serve_ui.js")], env=env, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    failed = []
    try:
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.1)
        else:
            print("UI 伺服器沒有啟動")
            return 1
        for test in tests:
            started = time.time()
            try:
                result = subprocess.run(["node", os.path.join("tests", test)], cwd=ROOT, env=env, capture_output=True,
                                        timeout=300)
                code = result.returncode
                output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
            except subprocess.TimeoutExpired:
                code, output = -1, "逾時（300 秒）"
            if code == 0:
                last = [l for l in output.strip().split("\n") if l.strip()][-1:] or [""]
                print(f"PASS {test} ({time.time() - started:.0f}s): {last[0].strip()[:100]}", flush=True)
            else:
                failed.append(test)
                print(f"FAIL {test} (exit {code})", flush=True)
                print(output[-4000:])
    finally:
        server.kill()
    print(f"{len(tests) - len(failed)} of {len(tests)} browser tests passed" + (f"; failed: {', '.join(failed)}" if failed else ""))
    return 1 if failed or not tests else 0


if __name__ == "__main__":
    sys.exit(main())
