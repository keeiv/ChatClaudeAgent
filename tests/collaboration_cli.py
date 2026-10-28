"""Exercise collaboration commands in the real CLI with isolated data and local protocol fixtures."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = root / "build/cli-collaboration"
    output.mkdir(exist_ok=True)
    case = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    data, project = case / "data", case / "project"
    data.mkdir()
    project.mkdir()
    for command in (["git", "init", "-b", "main"], ["git", "config", "user.name", "CCA Test"],
                    ["git", "config", "user.email", "cca-test@example.invalid"]):
        subprocess.run(command, cwd=project, check=True, capture_output=True)
    (project / "fixture.txt").write_text("CLI fixture\n", encoding="utf-8")
    subprocess.run(["git", "add", "fixture.txt"], cwd=project, check=True, capture_output=True)
    subprocess.run(["git", "commit", "-m", "Initial fixture"], cwd=project, check=True, capture_output=True)
    backend = str(root / "build/takeover_test.exe")
    (data / "settings.json").write_text(json.dumps({
        "claudePath": backend, "codexPath": backend, "guard": {"enabled": False},
        "profiles": {"user": {"name": "指揮官"}}
    }), encoding="utf-8")
    plan_file = case / "plan.json"
    plan_file.write_text(json.dumps({"title": "CLI-PLAN-FIXTURE", "steps": [
        {"text": "增加測試", "acceptance": "通過邊界測試"}]}), encoding="utf-8")
    commands = [f"/plan import {plan_file}", "/plan", "/plan-comment CLI-COMMENT-FIXTURE", "/plan",
                "/plan-approve", "/plan-accept step-1", "/focus codex security", "/inbox",
                "/workspace create"]
    lines = ["until /help 15000", "wait 1500"]
    for command in commands:
        lines += ["type " + command, "key enter", "wait 400"]
    # The JSON reply is wrapped at the terminal width, so wait for a key near its start ("origin"), which stays whole
    # whatever the temp path's length; a later key such as targetBranch can be split in two by the wrap.
    lines += ['until "origin" 15000', "type /workspace check", "key enter", "until clean 15000",
              "wait 400", "type /workspace merge", "key enter", "until merged 15000", "wait 400",
              "type /opinions claude,codex 如何檢查輸入？", "key enter", "until 效能仍需量測 20000",
              "wait 400", "type /opinions", "key enter", "wait 200", "type /opinion-plan", "key enter",
              "wait 400", "type /plan", "key enter", "wait 200", "type /exit", "key enter"]
    script = case / "input.txt"
    script.write_text("\n".join(lines) + "\n", encoding="utf-8")
    env = dict(os.environ, CCA_DATA_DIR=str(data), CCA_TEST_OPINION="1")
    for key in ("CCA_MOCK", "CCA_TEST_FAILURE", "CCA_TEST_WRITE", "CCA_TEST_FLOW", "CCA_TEST_CLI_FIXTURE"):
        env.pop(key, None)
    result = subprocess.run([str(root / "build/pty_run.exe"), "120", "38", str(case / "screen.vt"),
                             str(script), str(args.exe.resolve()), "gpt"],
                            cwd=project, env=env, capture_output=True, timeout=100)
    (case / "driver.log").write_bytes(result.stdout + result.stderr)
    assert result.returncode == 0, result.stdout.decode("utf-8", errors="replace")
    saved = json.loads((data / "collaboration/dm-codex.json").read_text(encoding="utf-8"))
    assert saved["plan"]["status"] == "draft", "opinion proposal started execution"
    assert saved["plan"]["revision"] == 3, "plan revision or comment was lost"
    assert saved["plan"]["comments"][0]["text"] == "CLI-COMMENT-FIXTURE", "comment not retained"
    assert saved["focus"]["codex"] == "security", "focus assignment not saved"
    assert saved["opinion"]["status"] == "done", "independent opinions did not finish"
    assert len(saved["opinion"]["summary"]["consensus"]) == 1, "synthesis missing"
    assert saved["workspace"]["check"]["merged"], "explicit worktree merge failed"
    assert Path(saved["workspace"]["path"]).is_dir(), "worktree was removed"
    messages = json.loads((data / "messages/dm-codex.json").read_text(encoding="utf-8"))
    assert sum(m.get("author") == "user" for m in messages) == 1, "plan unexpectedly ran a model"
    screen = (case / "screen.vt").read_text(encoding="utf-8", errors="replace")
    assert "收件匣" in screen, "inbox command was not displayed"
    assert "未知指令" not in screen, "a collaboration command was not routed"
    print("12 collaboration CLI checks passed")
    print("Artifacts: " + str(case))


if __name__ == "__main__":
    main()
