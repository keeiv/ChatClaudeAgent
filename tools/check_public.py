"""Hygiene check for the open-source tree: nothing private, nothing binary, nothing generated may be committed.

python tools/check_public.py            checks every tracked file (git ls-files)
Run it in the open-source tree (CI does). The private source tree keeps working notes and tool settings on purpose, so
it fails there by design; private/export_repos.py is what turns that tree into the public one.
Fails on: private keys and tokens, personal machine paths, real-looking e-mail addresses, build output and binaries,
signing keys, tool-specific local configuration, internal working notes, oversized files, and missing project files.
Standard library only.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Paths that never belong in the repository.
FORBIDDEN_PATHS = [
    (re.compile(r"(^|/)CLAUDE\.md$"), "個別 AI 工具的本機設定；通用的開發規則在 AGENTS.md"),
    (re.compile(r"(^|/)\.claude/"), "本機工具設定"),
    (re.compile(r"(^|/)(build|node_modules|__pycache__)/"), "建置輸出或相依套件"),
    (re.compile(r"\.(jks|keystore|pfx|p12|pem|key|der)$", re.I), "金鑰或憑證"),
    (re.compile(r"(^|/)\.env"), "環境變數檔"),
    (re.compile(r"\.(log|pyc|pdb|obj|ilk|exp|tmp|bak|swp|orig|rej)$", re.I), "暫存或建置產物"),
    (re.compile(r"\.(exe|dll|lib|apk|aab|zip|7z|rar|msi|dmp|mp4|wav|mp3)$", re.I), "二進位檔：請放進發行版（Release），不要提交"),
    (re.compile(r"(^|/)docs/[^/]*-(validation|review)\.md$"), "內部驗證與審查紀錄，不屬於開源文件"),
    (re.compile(r"(^|/)(Thumbs\.db|\.DS_Store|desktop\.ini)$", re.I), "作業系統雜檔"),
]
# Text that must not appear. Each entry: (pattern, why).
FORBIDDEN_TEXT = [
    (re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----"), "私鑰"),
    (re.compile(r"\bgh[pousr]_[A-Za-z0-9]{30,}\b"), "GitHub 權杖"),
    (re.compile(r"\bgithub_pat_[A-Za-z0-9_]{30,}\b"), "GitHub 權杖"),
    (re.compile(r"\bAKIA[0-9A-Z]{16}\b"), "AWS 存取金鑰"),
    (re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{10,}\b"), "Slack 權杖"),
    (re.compile(r"\bsk-(ant-)?[A-Za-z0-9_-]{40,}\b"), "看起來像真的 API 金鑰"),
    (re.compile(r"[A-Za-z]:\\Users\\(?!dev\\|user\\|Public\\|Default\\|name\\|you\\|<|%|\*)[A-Za-z0-9._-]+\\", re.I), "個人電腦的使用者路徑"),
    (re.compile(r"[A-Za-z]:/Users/(?!dev/|user/|Public/|Default/|name/|you/)[A-Za-z0-9._-]+/", re.I), "個人電腦的使用者路徑"),
    (re.compile(r"OneDrive[\\/]"), "個人雲端資料夾路徑"),
    (re.compile(r"S-1-5-21-\d+-\d+-\d+-\d+"), "Windows 安全識別碼（SID）"),
]
EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")
EMAIL_OK = re.compile(r"(@example\.(com|org|net|invalid)|@[a-z0-9.-]*\.invalid$|@users\.noreply\.github\.com$|@github\.com$|^git@|^token@|^you@|^user@|^dev@|^password@|^pass@|^tester@|^test@|^fixture@|^cca-test@)", re.I)
# The few places that may legitimately talk about the things above (this script and the guides that explain it).
SKIP_TEXT = {"tools/check_public.py", "CONTRIBUTING.md", "SECURITY.md", ".github/pull_request_template.md"}
# Fixture files: fake keys and fake users are the point of these.
FIXTURE_TEXT = re.compile(r"^(tests/|ui/mock\.js$|docs/)")
ALLOWED_BINARY = re.compile(r"\.(png|ico|jpg|jpeg|gif|webp|woff2)$", re.I)
MAX_BYTES = 1024 * 1024
MAX_BINARY_BYTES = 512 * 1024
REQUIRED = ["README.md", "LICENSE", "CONTRIBUTING.md", "SECURITY.md", "AGENTS.md", ".github/workflows/ci.yml",
            ".github/CODEOWNERS", ".github/pull_request_template.md"]


def tracked():
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8").split("\0") if p]


def is_text(data):
    return b"\0" not in data[:4096]


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    problems = []
    files = tracked()
    for path in files:
        full = os.path.join(ROOT, path)
        if not os.path.isfile(full):
            continue
        for pattern, why in FORBIDDEN_PATHS:
            if pattern.search(path):
                problems.append(f"{path}: {why}")
        size = os.path.getsize(full)
        with open(full, "rb") as handle:
            data = handle.read()
        if is_text(data):
            if size > MAX_BYTES:
                problems.append(f"{path}: 文字檔超過 {MAX_BYTES // 1024} KiB")
            if path in SKIP_TEXT:
                continue
            text = data.decode("utf-8", "replace")
            for pattern, why in FORBIDDEN_TEXT:
                if FIXTURE_TEXT.match(path) and why in ("看起來像真的 API 金鑰",):
                    continue
                hit = pattern.search(text)
                if hit:
                    line = text.count("\n", 0, hit.start()) + 1
                    problems.append(f"{path}:{line}: {why}")
            for hit in EMAIL.finditer(text):
                address = hit.group(0)
                if not EMAIL_OK.search(address):
                    problems.append(f"{path}:{text.count(chr(10), 0, hit.start()) + 1}: 疑似真實的電子郵件位址 {address}")
        else:
            if not ALLOWED_BINARY.search(path):
                problems.append(f"{path}: 不允許的二進位檔")
            elif size > MAX_BINARY_BYTES:
                problems.append(f"{path}: 圖片超過 {MAX_BINARY_BYTES // 1024} KiB")
    for needed in REQUIRED:
        if not os.path.isfile(os.path.join(ROOT, needed)):
            problems.append(f"{needed}: 開源倉庫必備的檔案不存在")
    license_path = os.path.join(ROOT, "LICENSE")
    if os.path.isfile(license_path) and "GNU GENERAL PUBLIC LICENSE" not in open(license_path, encoding="utf-8", errors="replace").read():
        problems.append("LICENSE: 不是 GPL 授權文字")
    for item in problems:
        print(item)
    print(f"{len(files)} 個追蹤檔案，{len(problems)} 個問題")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
