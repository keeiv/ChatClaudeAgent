"""Check that relative links and images in the Markdown files point at files that exist.

python tools/check_links.py
Skips http(s), mailto and in-page (#...) links; a link may carry a #fragment, which is not checked. Files listed in git
(git ls-files) are the ones that count, so a link to something that is only present on one machine fails here.
Standard library only.
"""
import os
import re
import subprocess
import sys
from urllib.parse import unquote

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
HTML = re.compile(r"""(?:src|href)=["']([^"']+)["']""")
FENCE = re.compile(r"^\s*(```|~~~)")


def tracked():
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
    return {p for p in out.decode("utf-8").split("\0") if p}


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    files = tracked()
    lowered = {f.lower() for f in files}
    dirs = {os.path.dirname(f) for f in files}
    problems = []
    markdown = [f for f in sorted(files) if f.endswith(".md")]
    for path in markdown:
        text = open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read()
        inside = False
        for number, line in enumerate(text.split("\n"), 1):
            if FENCE.match(line):
                inside = not inside
                continue
            if inside:
                continue
            line = re.sub(r"`[^`]*`", "", line)
            for target in LINK.findall(line) + HTML.findall(line):
                if re.match(r"^([a-z][a-z0-9+.-]*:|#|//)", target, re.I):
                    continue
                target = unquote(target.split("#", 1)[0].split("?", 1)[0])
                if not target:
                    continue
                resolved = os.path.normpath(os.path.join(os.path.dirname(path), target)).replace("\\", "/")
                if resolved in files or resolved.rstrip("/") in dirs or resolved == ".":
                    continue
                note = "（大小寫不同）" if resolved.lower() in lowered else ""
                problems.append(f"{path}:{number}: 連結的目標不存在 {target}{note}")
    for item in problems:
        print(item)
    print(f"{len(markdown)} 個 Markdown 檔，{len(problems)} 個壞連結")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
