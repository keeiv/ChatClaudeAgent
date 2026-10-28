"""Strictness check for GitHub Actions workflows (standard library only; no YAML parser needed).

Fails when a workflow: lacks top-level `permissions`, grants write access at the top level, uses an action that is not
pinned to a full commit SHA, has a job without `timeout-minutes`, uses `pull_request_target`, or puts attacker-controlled
event data (pull request title, branch name, body...) straight into a `run:` script where it could inject commands.
python tools/check_workflows.py
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UNTRUSTED = re.compile(r"\$\{\{\s*(github\.event\.(pull_request\.(title|body|head\.ref|head\.label)|issue\.(title|body)|comment\.body|head_commit\.message|commits)|github\.head_ref)")


def check(path):
    problems = []
    lines = open(path, encoding="utf-8").read().split("\n")
    name = os.path.relpath(path, ROOT).replace("\\", "/")
    top_permissions = [i for i, l in enumerate(lines) if re.match(r"^permissions:", l)]
    if not top_permissions:
        problems.append(f"{name}: 缺少頂層 permissions（預設權限過大）")
    else:
        i = top_permissions[0]
        inline = lines[i].split(":", 1)[1].strip()
        if inline in ("write-all", "read-all") or (inline and inline != "{}"):
            problems.append(f"{name}:{i + 1}: permissions 要逐項列出且預設唯讀")
        for j in range(i + 1, len(lines)):
            if lines[j] and not lines[j].startswith((" ", "#")):
                break
            if re.match(r"^\s+[a-z-]+:\s*write\s*$", lines[j]) and "security-events" not in lines[j]:
                problems.append(f"{name}:{j + 1}: 頂層不得授予寫入權限，請放在需要它的工作裡")
    if re.search(r"^\s*pull_request_target\s*:", "\n".join(lines), re.M):
        problems.append(f"{name}: 不使用 pull_request_target（會讓外部貢獻者的程式碼取得密鑰）")
    in_jobs, job_name, job_start, job_has_timeout, jobs = False, None, 0, False, []
    for i, line in enumerate(lines):
        if re.match(r"^jobs:\s*$", line):
            in_jobs = True
            continue
        if in_jobs and re.match(r"^  [A-Za-z0-9_-]+:\s*$", line):
            if job_name:
                jobs.append((job_name, job_start, job_has_timeout))
            job_name, job_start, job_has_timeout = line.strip().rstrip(":"), i + 1, False
        elif in_jobs and re.match(r"^    timeout-minutes:\s*\d+", line):
            job_has_timeout = True
        m = re.match(r"^\s*-?\s*uses:\s*([^\s#]+)", line)
        if m and not m.group(1).startswith("./"):
            if not re.search(r"@[0-9a-f]{40}$", m.group(1)):
                problems.append(f"{name}:{i + 1}: {m.group(1)} 沒有固定到完整的提交 SHA")
    if job_name:
        jobs.append((job_name, job_start, job_has_timeout))
    for job, start, ok in jobs:
        if not ok:
            problems.append(f"{name}:{start}: 工作 {job} 沒有 timeout-minutes")
    # untrusted event data inside a `run:` block
    in_run, run_indent = False, 0
    for i, line in enumerate(lines):
        stripped = line.lstrip()
        indent = len(line) - len(stripped)
        if re.match(r"^-?\s*run:\s*[|>]?-?\s*$", stripped) or re.match(r"^run:\s*[|>]", stripped):
            in_run, run_indent = True, indent
            continue
        if in_run and stripped and indent <= run_indent:
            in_run = False
        text = stripped if in_run or re.match(r"^-?\s*run:\s+\S", stripped) else ""
        if text and UNTRUSTED.search(text):
            problems.append(f"{name}:{i + 1}: 事件資料直接放進 run 腳本，請改用 env 傳入")
    return problems


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    files = sorted(glob.glob(os.path.join(ROOT, ".github", "workflows", "*.yml")) + glob.glob(os.path.join(ROOT, ".github", "workflows", "*.yaml")))
    if not files:
        print("找不到任何工作流程")
        return 1
    problems = [p for f in files for p in check(f)]
    for p in problems:
        print(p)
    print(f"{len(files)} 個工作流程，{len(problems)} 個問題")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
