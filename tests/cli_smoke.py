"""Exercise the real CLI in ConPTY with isolated data and simulated agent backends.

Build cli, pty and takeover-test first. No network calls or model quota are used.
Recordings, persisted messages and memory samples stay under build/cli-smoke/.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.stdout.reconfigure(encoding="utf-8")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run_case(root, exe, name, script, conversation="claude", flow=False, usage=None, badges=None, setup=None):
    case = root / name
    project = case / "project"
    data = case / "data"
    project.mkdir(parents=True)
    data.mkdir()
    if setup:
        setup(project)
    settings = {
        "claudePath": str(ROOT / "tests/fake_claude.cmd"),
        "codexPath": str(ROOT / "build/takeover_test.exe"),
        "profiles": {"user": {"name": "指揮官"}},
        "humanize": {"enabled": True},
        # The fixture backends answer tersely and never read files; the compliance checks would send
        # every reply back. They are covered by build.bat test and takeover-test instead.
        "guard": {"enabled": False, "retries": 2},
    }
    (data / "settings.json").write_text(json.dumps(settings), encoding="utf-8")
    if usage is not None:
        (data / "usage.json").write_text(json.dumps(usage), encoding="utf-8")
    if badges is not None:
        (data / "badges.json").write_text(json.dumps(badges), encoding="utf-8")
    script_path = case / "input.txt"
    script_path.write_text(script, encoding="utf-8")
    env = dict(os.environ, CCA_DATA_DIR=str(data), CCA_TEST_CLI_FIXTURE="1")
    env["GEMINI_CLI_HOME"] = str(data / "gemini-home")
    # Do not inherit a desktop demo flag or a quota-fixture scenario from another run.
    for key in ("CCA_MOCK", "CCA_TEST_FAILURE", "CCA_TEST_WRITE"):
        env.pop(key, None)
    env["CCA_TEST_FLOW"] = "1" if flow else "0"
    command = [str(ROOT / "build/pty_run.exe"), "100", "34", str(case / "screen.vt"),
               str(script_path), str(exe), conversation]
    result = subprocess.run(command, cwd=project, env=env, capture_output=True, timeout=90)
    (case / "driver.log").write_bytes(result.stdout + result.stderr)
    require(result.returncode == 0, f"{name}: PTY failed: {result.stdout.decode('utf-8', errors='replace')}")
    messages = []
    for path in (data / "messages").glob("*.json"):
        messages.extend(json.loads(path.read_text(encoding="utf-8")))
    saved = json.loads((data / "settings.json").read_text(encoding="utf-8"))
    require(b"ChatClaudeAgent" in (case / "screen.vt").read_bytes(), f"{name}: missing brand")
    print(f"PASS {name}")
    return case, saved, messages


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, default=ROOT / "build/ChatClaudeAgent-CLI.exe")
    args = parser.parse_args()
    exe = args.exe.resolve()
    for binary in (exe, ROOT / "build/pty_run.exe", ROOT / "build/takeover_test.exe"):
        require(binary.is_file(), f"Build required binary first: {binary}")
    out = ROOT / "build/cli-smoke"
    out.mkdir(exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="run-", dir=out))
    print(f"Artifacts: {root}")

    case, saved, messages = run_case(root, exe, "allow", """until /help 15000
wait 1200
snap start
memory idle
type 幫我寫
key shift-enter
type is_prime
wait 250
snap multiline
key enter
until 想要執行指令 15000
wait 500
snap approval
memory approval
key enter
until 完成： 10000
wait 600
snap done
memory done
type /show
key enter
wait 250
snap details
type /humanize invalid
key enter
wait 250
snap invalid
type /exit
key enter
"""
    )
    user = next(m for m in messages if m["author"] == "user")
    require(user["text"] == "幫我寫\nis_prime", "Shift+Enter must preserve a newline without submitting")
    blocks = [b for m in messages for b in m.get("blocks", [])]
    require(any(b.get("type") == "approval" and b.get("state") == "allow" for b in blocks), "approval not sent")
    require(any(b.get("type") == "tool" and b.get("status") == "ok" for b in blocks), "command not completed")
    require((case / "project/prime.py").is_file(), "fake backend did not run in the project")
    require(saved["humanize"]["enabled"], "invalid /humanize argument changed a setting")

    _, _, messages = run_case(root, exe, "deny", """until /help 15000
wait 1200
type 幫我寫 is_prime
key enter
until 想要執行指令 15000
wait 300
key esc
wait 200
snap feedback
type 保持唯讀
key enter
until 完成： 10000
wait 400
snap denied
type /exit
key enter
"""
    )
    blocks = [b for m in messages for b in m.get("blocks", [])]
    require(any(b.get("type") == "approval" and b.get("state") == "deny" and b.get("feedback") == "保持唯讀" for b in blocks), "denial or feedback not sent")
    require(any(b.get("type") == "tool" and b.get("status") == "declined" for b in blocks), "denied command must remain declined")

    settings_case, saved, _ = run_case(root, exe, "settings", """until /help 15000
wait 1200
type /chat
key enter
wait 200
snap chats
key esc
type /name
key enter
wait 200
key enter
wait 200
key ctrl-u
type CLI 指揮官
key enter
wait 200
type /persona
key enter
wait 200
key enter
wait 200
snap personas
key enter
type /permission
key enter
wait 200
snap permissions
key enter
type /model
key enter
wait 200
key enter
wait 200
snap models
key esc
type /
wait 200
snap commands
key ctrl-c
resize 48 18
wait 250
type /permission
key enter
wait 250
snap narrow
key esc
resize 100 34
wait 250
type /status
key enter
wait 300
snap restored
memory idle
type /motion off
key enter
wait 200
type /flow compact
key enter
wait 200
snap compact
type /exit
key enter
""", "group")
    require(saved["profiles"]["user"]["name"] == "CLI 指揮官", "name setting did not persist")
    require(saved["persona"]["claude"] == "none", "persona selection did not persist")
    require(saved["visual"] == {"mode": "compact", "motion": False}, "visual settings did not persist")
    conversations = json.loads((settings_case / "data/conversations.json").read_text(encoding="utf-8"))
    require(any(c["kind"] == "group" and c["permission"] == "plan" for c in conversations), "permission selection did not persist")

    _, saved, _ = run_case(root, exe, "gemini-profile", """until /help 15000
wait 1200
type /name
key enter
until 要改誰的名稱 3000
key down
key down
key down
key enter
until 新的名稱 3000
key ctrl-u
type Gemini CLI examiner
key enter
wait 300
type /persona
key enter
until 要設定誰的性格 3000
key down
key down
key enter
until Gemini 3000
wait 250
key enter
wait 250
type /exit
key enter
""", "group")
    require(saved["profiles"]["gemini"]["name"] == "Gemini CLI examiner", "Gemini CLI name did not persist")
    require(saved["persona"]["gemini"] == "none", "Gemini CLI persona choice did not persist")

    _, _, messages = run_case(root, exe, "group", """until /help 15000
wait 1200
type 幫我寫 is_prime
key enter
until 想要執行指令 15000
wait 300
key enter
until 協作完成 15000
wait 300
snap consensus
memory done
type /exit
key enter
""", "group")
    require(any(m.get("kind") == "handoff" for m in messages), "group never handed off")
    require(any(m.get("author") == "codex" and m.get("phase") == "review" for m in messages), "GPT reviewer did not run")
    require(any(m.get("kind") == "summary" and m.get("outcome") == "consensus" for m in messages), "group never reached consensus")

    flow_case, _, _ = run_case(root, exe, "flow", """until /help 15000
wait 1200
snap idle
memory idle
type 幫我寫 is_prime
key enter
until 思考中 15000
wait 100
snap thinking
wait 240
snap thinking-next
memory thinking
until 想要執行指令 15000
wait 250
snap approval
memory approval
key enter
until 送交審查 15000
wait 120
snap transfer
memory transfer
wait 360
snap transfer-next
resize 24 18
wait 200
snap narrow-flow
resize 100 34
wait 100
until 協作完成 15000
wait 300
snap done
type /motion off
key enter
wait 250
snap still
type /flow off
key enter
wait 250
snap hidden
type /exit
key enter
""", "group", flow=True)
    require("transfer-next=" in (flow_case / "screen.vt.snaps").read_text(), "handoff animation was not captured")

    activity_case, _, _ = run_case(root, exe, "activity", """until /help 15000
wait 1200
type /activity daily
key enter
until 每日輸入與輸出用量 10000
snap daily
type /activity weekly
key enter
until 截至該日期的近 7 日用量 10000
snap weekly
type /activity total
key enter
until 已保存紀錄截至該日期的累計用量 10000
snap total
type /about
key enter
until 技術規範與工具權限保持一致 10000
snap about
type /exit
key enter
""", usage={"2000-01-01": {"claude": {"input": 500}},
            time.strftime("%Y-%m-%d"): {"api": {"input": 123, "output": 45}}})
    require(b"668" in (activity_case / "screen.vt").read_bytes(), "activity lost a historical date or API usage")

    badge_case, saved, _ = run_case(root, exe, "badges", """until /help 15000
wait 1200
type /badges
key enter
until CCA 原初 10000
snap badges
type /badges hide origin
key enter
wait 400
type /badges first member
key enter
wait 400
type /badges show origin
key enter
wait 400
type /badges
key enter
until [展示] 成員 10000
snap reordered
type /exit
key enter
""", badges={"version": 1, "profile": "isolated-fixture",
             "grants": ["developer", "community-developer", "origin", "familiar", "present", "pioneer"]})
    require(saved["badgeDisplay"]["order"][0] == "member", "CLI did not persist badge order")
    require(saved["badgeDisplay"]["hidden"] == [], "CLI did not restore hidden badge visibility")

    appearance_case, saved, _ = run_case(root, exe, "appearance", """until /help 15000
wait 1200
type /theme claude
key enter
wait 200
type /auto on
key enter
wait 200
type /auto effort off
key enter
wait 200
type /auto preview 修復資料遺失
key enter
wait 250
snap appearance
type /exit
key enter
""")
    require(saved["theme"] == "claude", "CLI theme preference was not saved")
    conversations = json.loads((appearance_case / "data/conversations.json").read_text(encoding="utf-8"))
    dm = next(c for c in conversations if c["id"] == "dm-claude")
    require(dm["adaptive"]["claude"] == {"model": True, "effort": False}, "CLI auto dimensions were not independent")
    recording = (appearance_case / "screen.vt").read_bytes()
    require(b"48;2;250;249;245" in recording, "light CLI did not paint paper background")
    require(not list((appearance_case / "data/messages").glob("*.json")), "routing preview invoked a model")

    queue_case, _, queued_messages = run_case(root, exe, "instruction-queue", """until /help 15000
wait 1200
type /queue add 回報設定 QUEUED_FIRST
key enter
wait 200
type /queue add 回報設定 QUEUED_SECOND
key enter
wait 200
type /queue resume
key enter
wait 3500
type /context
key enter
wait 300
type /update
key enter
wait 300
snap queue-complete
type /exit
key enter
""")
    queue_data = json.loads((queue_case / "data/desk/dm-claude.json").read_text(encoding="utf-8"))
    require([q["status"] for q in queue_data["queue"]] == ["delivered", "delivered"],
            "queue did not deliver both instructions exactly once")
    requests = [m["text"] for m in queued_messages if m["author"] == "user"]
    require(requests == ["回報設定 QUEUED_FIRST", "回報設定 QUEUED_SECOND"],
            "queued instructions lost ordering or were delivered more than once")
    require(len(queue_data["contexts"]) == 2, "missing context receipts for actual queued turns")

    navigation_case, saved, _ = run_case(root, exe, "navigation", """until /help 15000
wait 1200
type /new Navigation A
key enter
until Navigation A 10000
wait 300
type /new Navigation B
key enter
until Navigation B 10000
wait 300
type /groups up
key enter
wait 300
type /groups up
key enter
wait 300
type /groups down
key enter
wait 300
type /groups
key enter
until 協作群組 · 固定順序 10000
snap groups
type /chat
key enter
wait 250
snap picker
key esc
type /exit
key enter
""", "group")
    stored_conversations = json.loads((navigation_case / "data/conversations.json").read_text(encoding="utf-8"))
    group_names = {c["id"]: c["name"] for c in stored_conversations if c["kind"] == "group"}
    require([group_names[c] for c in saved["groupOrder"]] == ["協作群組", "Navigation B", "Navigation A"],
            "CLI group order did not persist its up/down operations")

    # Gemini joins the ordinary group rotation, and updates come only from the local folder.
    member_case, saved, _ = run_case(root, exe, "gemini-member-update", """until /help 15000
wait 1200
type /gemini on
key enter
until 第三位審查者 10000
wait 300
type /update
key enter
until 資料夾中沒有安裝包 10000
snap update
type /exit
key enter
""", "group")
    stored_conversations = json.loads((member_case / "data/conversations.json").read_text(encoding="utf-8"))
    require(any(c["kind"] == "group" and c.get("geminiMember") for c in stored_conversations),
            "CLI /gemini on did not persist Gemini group membership")
    require(any(c["kind"] == "gemini" for c in stored_conversations), "missing Gemini direct message")
    require((member_case / "data/installers").is_dir(), "local update folder was not created automatically")

    # The reviewer test guard is configured per group; the host stores the command and refuses unusable ones.
    guard_case, _, _ = run_case(root, exe, "test-guard", """until /help 15000
wait 1200
type /test-guard exit /b 0
key enter
wait 800
type /test-guard
key enter
until 測試守門：開啟 10000
snap guard
type /exit
key enter
""", "group")
    stored_conversations = json.loads((guard_case / "data/conversations.json").read_text(encoding="utf-8"))
    require(any(c["kind"] == "group" and c.get("regressionGuard") and c.get("testCommand") == "exit /b 0"
                for c in stored_conversations), "CLI /test-guard did not persist the guard and its command")

    # CCA plugins: the example plugin is listed, enabled only after ALLOW, and its group mode is applied.
    def plugin_folder(project):
        target = project.parent / "data" / "plugins" / "night-shift"
        target.mkdir(parents=True)
        source = ROOT / "examples" / "plugins" / "night-shift" / "plugin.json"
        (target / "plugin.json").write_bytes(source.read_bytes())

    plugin_case, saved, _ = run_case(root, exe, "plugins", """until /help 15000
wait 1200
type /plugins
key enter
until 夜班 10000
type /plugins enable night-shift
key enter
until ALLOW 10000
type ALLOW
key enter
until 已啟用插件 10000
type /plugin-mode night-shift/strict
key enter
wait 1200
snap plugins
type /exit
key enter
""", "group", setup=plugin_folder)
    require(saved.get("extensions", {}).get("enabled", {}).get("night-shift", "").__len__() == 64,
            "CLI /plugins enable did not pin the plugin's SHA-256")
    stored_conversations = json.loads((plugin_case / "data/conversations.json").read_text(encoding="utf-8"))
    require(any(c["kind"] == "group" and c.get("mode") == "night-shift/strict" and c.get("permission") == "ask"
                and c.get("maxRounds") == 10 for c in stored_conversations),
            "CLI /plugin-mode did not apply the plugin mode's settings")

    # Version control on a real repository: commit the green and yellow files, keep the locked one out.
    def git_project(project):
        def run(*args):
            subprocess.run(["git", *args], cwd=project, check=True, capture_output=True, timeout=30)
        run("init", "-b", "main")
        run("config", "user.name", "CCA Test")
        run("config", "user.email", "test@example.invalid")
        run("config", "commit.gpgsign", "false")
        (project / "app.txt").write_text("one\n", encoding="utf-8")
        run("add", "app.txt")
        run("commit", "-m", "Initial")
        (project / "app.txt").write_text("one\ntwo\n", encoding="utf-8")
        (project / "new.txt").write_text("new\n", encoding="utf-8")
        (project / ".env").write_text("TOKEN=x\n", encoding="utf-8")

    git_case, saved, _ = run_case(root, exe, "git-tools", """until /help 15000
wait 1200
type /cd .
key enter
wait 400
type /git
key enter
until 分支 main 10000
snap status
type /git commit CLI 選檔提交
key enter
until 已提交 10000
wait 300
type /git identity --repo CLI Tester cli@example.invalid
key enter
until 已設定這個倉庫的 Git 提交身分 10000
wait 300
type /git never secret.txt
key enter
wait 300
type /resume-auto on
key enter
until 自動接續：開啟 10000
snap done
type /exit
key enter
""", "group", setup=git_project)
    log = subprocess.run(["git", "log", "-1", "--format=%s%n%b", "--name-only"], cwd=git_case / "project",
                         capture_output=True, text=True, encoding="utf-8", timeout=30).stdout
    require(log.startswith("CLI 選檔提交") and "app.txt" in log and "new.txt" in log and ".env" not in log,
            "CLI commit did not contain exactly the recommended files")
    author = subprocess.run(["git", "config", "--local", "user.name"], cwd=git_case / "project",
                            capture_output=True, text=True, encoding="utf-8", timeout=30).stdout.strip()
    require(author == "CLI Tester", "CLI /git identity --repo did not set the repository author")
    require("secret.txt" in saved["git"]["neverCommit"] and saved["autoResume"]["enabled"] is True,
            "CLI never-commit rule or automatic resume did not persist")

    # A missing expectation must fail the driver, even if the program exits cleanly.
    negative = root / "negative.txt"
    negative.write_text("until THIS_MARKER_MUST_NOT_EXIST 10\n", encoding="utf-8")
    result = subprocess.run([str(ROOT / "build/pty_run.exe"), "80", "24", str(root / "negative.vt"),
                             str(negative), str(exe), "--version"], capture_output=True, timeout=15)
    require(result.returncode != 0, "PTY driver silently accepted a timed-out expectation")
    print("PASS timeout is a failure")

    # Separate processes must also reject spelling aliases of the same data folder.
    locked = root / "locked"
    locked.mkdir()
    holder_script = root / "holder.txt"
    holder_script.write_text("until /help 15000\nsnap locked\nwait 5000\ntype /exit\nkey enter\n", encoding="utf-8")
    contender_script = root / "contender.txt"
    contender_script.write_text("until 另一個 3000\n", encoding="utf-8")
    env = dict(os.environ, CCA_DATA_DIR=str(case / "data"))
    holder_log = (locked / "holder.log").open("wb")
    holder = subprocess.Popen([str(ROOT / "build/pty_run.exe"), "80", "24", str(locked / "holder.vt"),
                               str(holder_script), str(exe), "claude"], cwd=case / "project", env=env,
                              stdout=holder_log, stderr=holder_log)
    try:
        deadline = time.monotonic() + 15
        ready = locked / "holder.vt.snaps"
        while time.monotonic() < deadline and not (ready.exists() and "locked=" in ready.read_text()):
            time.sleep(0.05)
        require(ready.exists() and "locked=" in ready.read_text(), "lock holder did not start")
        env["CCA_DATA_DIR"] += "\\."
        contender = subprocess.run([str(ROOT / "build/pty_run.exe"), "80", "24", str(locked / "contender.vt"),
                                    str(contender_script), str(exe), "claude"], cwd=case / "project", env=env,
                                   capture_output=True, timeout=10)
        (locked / "contender.log").write_bytes(contender.stdout + contender.stderr)
        require(contender.returncode != 0 and b"found" in contender.stdout,
                "second process did not refuse the already-open data folder")
    finally:
        holder.wait(timeout=25)
        holder_log.close()
    require(holder.returncode == 0, "lock holder did not close cleanly")
    print("PASS shared data lock across processes")
    print("17 CLI smoke scenarios passed")


if __name__ == "__main__":
    main()
