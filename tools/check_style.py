"""Checks CCA's code style. Every AI that edits CCA runs this before handing work over.

    python tools/check_style.py            whitespace and encoding rules for every text file
    python tools/check_style.py --fix      rewrites what can be fixed mechanically
    python tools/check_style.py --format   also runs clang-format on the C++ lines changed since HEAD
    python tools/check_style.py --js       also runs `node --check` on the JavaScript files

The rules are the ones in AGENTS.md ("程式碼風格"): UTF-8 without a BOM (PowerShell scripts
keep theirs), LF line endings (CRLF for .bat/.cmd/.ps1), no tabs, no trailing whitespace,
a final newline. clang-format is only applied to changed lines, never to whole files.
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEXT = {".cpp", ".hpp", ".h", ".rc", ".manifest", ".js", ".cjs", ".mjs", ".css", ".html", ".json", ".py",
        ".md", ".svg", ".bat", ".cmd", ".ps1", ".yml", ".yaml", ".txt"}
NAMED = {".clang-format", ".editorconfig", ".gitattributes", ".gitignore"}
SKIP_DIRS = {".git", "build", "third_party", "node_modules", ".playwright-mcp", "__pycache__", ".vs"}
CRLF = {".bat", ".cmd", ".ps1"}
CPP = {".cpp", ".hpp", ".h"}


def git(*args, required=False):
    try:
        result = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    except OSError as error:
        if required:
            raise RuntimeError("無法執行 Git：%s" % error) from error
        return ""
    if result.returncode:
        if required:
            raise RuntimeError("Git 無法取得改動：%s" % result.stderr.strip())
        return ""
    return result.stdout


def files():
    """Tracked and new (not ignored) files when this is a git repository, else a walk."""
    listed = git("ls-files", "--cached", "--others", "--exclude-standard", "-z")
    if listed:
        names = [n for n in listed.split("\0") if n]
    else:
        names = []
        for top, dirs, fs in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            names += [os.path.relpath(os.path.join(top, f), ROOT) for f in fs]
    out = []
    for n in names:
        n = n.replace("\\", "/")
        if n.split("/")[0] in SKIP_DIRS or not os.path.isfile(os.path.join(ROOT, n)):
            continue
        base = os.path.basename(n)
        if os.path.splitext(n)[1].lower() in TEXT or base in NAMED:
            out.append(n)
    return sorted(out)


def check(path, fix):
    """Returns a list of problems; with fix=True the file is rewritten first."""
    full = os.path.join(ROOT, path)
    with open(full, "rb") as source:
        data = source.read()
    ext = os.path.splitext(path)[1].lower()
    problems = []
    bom = data.startswith(b"\xef\xbb\xbf")
    body = data[3:] if bom else data
    try:
        text = body.decode("utf-8")
    except UnicodeDecodeError as e:
        return ["不是 UTF-8（位置 %d）" % e.start]
    want_bom = ext == ".ps1" and any(ord(c) > 127 for c in text)
    eol = "\r\n" if ext in CRLF else "\n"
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    clean = [l.rstrip(" \t") for l in lines]
    # Only indentation is rewritten; a tab inside a line may be meant (it is reported).
    clean = [" " * 4 * (len(l) - len(l.lstrip("\t"))) + l.lstrip("\t") for l in clean]
    fixed = (eol.join(clean) + eol) if clean else ""
    if bom and not want_bom:
        problems.append("有 BOM（只有含中文的 .ps1 需要）")
    if want_bom and not bom:
        problems.append("含中文的 .ps1 需要 BOM，否則 Windows PowerShell 5.1 會讀錯")
    if eol == "\n" and "\r\n" in text:
        problems.append("用了 CRLF，請改成 LF")
    if eol == "\r\n" and text.replace("\r\n", "").find("\n") >= 0:
        problems.append("要用 CRLF")
    if "\r" in text.replace("\r\n", ""):
        problems.append("有單獨的 CR 換行")
    trailing = [i + 1 for i, l in enumerate(lines) if l != l.rstrip(" \t")]
    if trailing:
        problems.append("行尾空白：第 %s 行" % ", ".join(map(str, trailing[:8])) + (" 等" if len(trailing) > 8 else ""))
    tabs = [i + 1 for i, l in enumerate(lines) if "\t" in l]
    if tabs:
        problems.append("有 Tab：第 %s 行" % ", ".join(map(str, tabs[:8])))
    if text and not text.endswith("\n"):
        problems.append("最後一行沒有換行")
    markers = [i + 1 for i, l in enumerate(lines) if l.startswith(("<<<<<<< ", ">>>>>>> ")) or l == "======="]
    if markers:
        problems.append("合併衝突標記：第 %s 行" % ", ".join(map(str, markers[:8])))
    if fix and problems and not markers:
        out = (b"\xef\xbb\xbf" if want_bom else b"") + fixed.encode("utf-8")
        if out != data:
            with open(full, "wb") as target:
                target.write(out)
        problems = check(path, False)
    return problems


def find_clang_format():
    exe = shutil.which("clang-format")
    if exe:
        return exe
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if os.path.exists(vswhere):
        out = subprocess.run([vswhere, "-latest", "-products", "*", "-property", "installationPath"],
                             capture_output=True, text=True).stdout.strip()
        exe = os.path.join(out, "VC", "Tools", "Llvm", "bin", "clang-format.exe")
        if out and os.path.exists(exe):
            return exe
    return None


def changed_ranges(base="HEAD"):
    """{path: [(first, last), ...]} for C++ lines added or changed since `base` (HEAD by default; new files count whole)."""
    ranges = {}
    patterns = ("--", "*.cpp", "*.hpp", "*.h")
    # NUL-separated names avoid Git's quoted Unicode paths. Parse each file's hunks,
    # not the quoted +++ headers, and disable external diff tools for predictable output.
    tracked = git("diff", "--name-only", "--no-renames", "--diff-filter=ACMT", "-z", base, *patterns, required=True)
    for path in tracked.split("\0"):
        if not path or path.split("/")[0] in SKIP_DIRS or not os.path.isfile(os.path.join(ROOT, path)):
            continue
        diff = git("diff", "-U0", "--no-color", "--no-ext-diff", "--no-textconv", "--no-renames", base, "--", path,
                   required=True)
        for line in diff.splitlines():
            if not line.startswith("@@ "):
                continue
            new = line.split("+", 1)[1].split(" ")[0]
            start, _, count = new.partition(",")
            count = int(count) if count else 1
            if count:
                ranges.setdefault(path, []).append((int(start), int(start) + count - 1))
    for path in git("ls-files", "--others", "--exclude-standard", "-z", *patterns, required=True).split("\0"):
        if not path or path.split("/")[0] in SKIP_DIRS or not os.path.isfile(os.path.join(ROOT, path)):
            continue
        # Encoding errors are reported by check(); they must not crash range collection.
        with open(os.path.join(ROOT, path), "rb") as source:
            count = sum(1 for _ in source)
        if count:
            ranges[path] = [(1, count)]
    return ranges


def main():
    # The messages are Chinese; a console or pipe in a legacy code page would fail on them.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--fix", action="store_true")
    ap.add_argument("--format", action="store_true")
    ap.add_argument("--js", action="store_true")
    ap.add_argument("--base", default="HEAD", help="git revision the changed C++ lines are measured against (CI passes the pull request base)")
    args = ap.parse_args()

    bad = 0
    names = files()
    for n in names:
        for p in check(n, args.fix):
            print("%s: %s" % (n, p))
            bad += 1

    if args.format:
        exe = find_clang_format()
        if not exe:
            bad += 1
            print("無法檢查 C++ 格式：找不到 clang-format，請安裝 Visual Studio Build Tools 的 C++ Clang 工具")
        elif not git("rev-parse", "HEAD"):
            bad += 1
            print("無法檢查 C++ 格式：需要 Git 倉庫與至少一個提交作為比對基準")
        else:
            try:
                changed = changed_ranges(args.base)
            except RuntimeError as error:
                bad += 1
                print(error)
                changed = {}
            for path, spans in changed.items():
                cmd = [exe, "--dry-run", "--Werror", "--style=file"] + ["--lines=%d:%d" % s for s in spans] + [path]
                r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8")
                if r.returncode:
                    bad += 1
                    print("%s: 改動的行不符合 .clang-format（clang-format -i %s 修正）" % (
                        path, " ".join("--lines=%d:%d" % s for s in spans)))

    if args.js:
        node = shutil.which("node")
        js = [n for n in names if n.endswith((".js", ".cjs", ".mjs"))]
        if js and not node:
            bad += 1
            print("無法檢查 JavaScript 語法：找不到 node，請安裝 Node.js")
        for n in js if node else []:
            r = subprocess.run([node, "--check", n], cwd=ROOT, capture_output=True, text=True, encoding="utf-8")
            if r.returncode:
                bad += 1
                print("%s: JavaScript 語法錯誤\n%s" % (n, r.stderr.strip()))

    print("%d 個檔案，%s" % (len(names), "沒有問題" if not bad else "%d 個問題" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
