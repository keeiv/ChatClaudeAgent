"""Exercise style failures in temporary files and Git repositories, without changing CCA's index."""
import contextlib
import importlib.util
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location("check_style", Path(__file__).resolve().parents[1] / "tools/check_style.py")
style = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(style)


class FileFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cca-style-")
        self.root = Path(self.temp.name)
        self.patched = mock.patch.object(style, "ROOT", str(self.root))
        self.patched.start()

    def tearDown(self):
        self.patched.stop()
        self.temp.cleanup()

    def write(self, name, data):
        (self.root / name).write_bytes(data)

    def invoke(self, *args):
        with mock.patch.object(sys, "argv", ["check_style.py", *args]), contextlib.redirect_stdout(io.StringIO()):
            return style.main()


class StyleChecks(FileFixture):
    def test_markdown_tabs_are_reported(self):
        self.write("notes.md", b"inline\ttab\n")
        self.assertTrue(style.check("notes.md", False))
        self.assertTrue(style.check("notes.md", True))  # An inline tab must not be silently rewritten.
        self.assertEqual((self.root / "notes.md").read_bytes(), b"inline\ttab\n")

    def test_lone_cr_is_reported_and_fixed(self):
        self.write("sample.py", b"first\rsecond\n")
        self.assertTrue(style.check("sample.py", False))
        self.assertEqual(style.check("sample.py", True), [])
        self.assertEqual((self.root / "sample.py").read_bytes(), b"first\nsecond\n")

    def test_powershell_unicode_keeps_bom_and_crlf(self):
        self.write("sample.ps1", "# \u4e2d\u6587\n".encode("utf-8"))
        self.assertEqual(style.check("sample.ps1", True), [])
        self.assertEqual((self.root / "sample.ps1").read_bytes(), b"\xef\xbb\xbf" + "# \u4e2d\u6587\r\n".encode("utf-8"))

    def test_requested_js_check_fails_without_node(self):
        self.write("sample.js", b"const value = 1;\n")
        with mock.patch.object(style.shutil, "which", return_value=None):
            self.assertEqual(self.invoke("--js"), 1)

    def test_requested_format_check_fails_without_formatter(self):
        self.write("sample.cpp", b"int value = 1;\n")
        with mock.patch.object(style, "find_clang_format", return_value=None):
            self.assertEqual(self.invoke("--format"), 1)

    @unittest.skipUnless(style.find_clang_format(), "clang-format is required for format integration checks")
    def test_format_check_fails_without_git_baseline(self):
        self.assertEqual(self.invoke("--format"), 1)


@unittest.skipUnless(shutil.which("git"), "Git is required for changed-line tests")
class ChangedLines(FileFixture):
    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.root, check=True, capture_output=True)

    def setUp(self):
        super().setUp()
        self.git("init", "-q")
        self.git("config", "core.quotepath", "true")
        self.name = "\u898f\u7bc4 file.cpp"
        self.write(self.name, b"int first = 1;\n")
        self.git("add", "--", self.name)
        self.git("-c", "user.name=CCA test", "-c", "user.email=cca-test@localhost", "-c", "commit.gpgsign=false",
                 "commit", "-qm", "Create test fixture", "--no-verify")

    def test_tracked_unicode_filename_keeps_changed_range(self):
        self.write(self.name, b"int first = 1;\nint second=2;\n")
        self.assertEqual(style.changed_ranges(), {self.name: [(2, 2)]})

    def test_base_revision_selects_the_compared_lines(self):
        # CI compares a pull request against its base commit, not against its own HEAD.
        self.write(self.name, b"int first = 1;\nint second=2;\n")
        self.git("add", "--", self.name)
        self.git("-c", "user.name=CCA test", "-c", "user.email=cca-test@localhost", "-c", "commit.gpgsign=false",
                 "commit", "-qm", "Add a second line", "--no-verify")
        self.assertEqual(style.changed_ranges(), {})
        self.assertEqual(style.changed_ranges("HEAD~1"), {self.name: [(2, 2)]})

    def test_untracked_unicode_filename_is_checked(self):
        name = "\u65b0\u6a94.cpp"
        self.write(name, b"int value=1;\n")
        self.assertEqual(style.changed_ranges(), {name: [(1, 1)]})

    def test_staged_changes_are_checked(self):
        self.write(self.name, b"int first=1;\n")
        self.git("add", "--", self.name)
        self.assertEqual(style.changed_ranges(), {self.name: [(1, 1)]})

    def test_empty_new_cpp_does_not_make_an_invalid_range(self):
        self.write("empty.cpp", b"")
        self.assertEqual(style.changed_ranges(), {})

    def test_non_utf8_new_cpp_can_still_report_format_ranges(self):
        self.write("invalid.cpp", b"// \xff\n")
        self.assertEqual(style.changed_ranges(), {"invalid.cpp": [(1, 1)]})

    @unittest.skipUnless(style.find_clang_format(), "clang-format is required for format integration checks")
    def test_bad_unicode_cpp_fails_the_real_formatter(self):
        self.write(".clang-format", b"BasedOnStyle: Google\nIndentWidth: 4\nColumnLimit: 120\n")
        self.write(self.name, b"int first = 1;\nint second=2;\n")
        self.assertEqual(self.invoke("--format"), 1)

    @unittest.skipUnless(style.find_clang_format(), "clang-format is required for format integration checks")
    def test_legacy_lines_are_not_reformatted(self):
        self.write(".clang-format", b"BasedOnStyle: Google\nIndentWidth: 4\nColumnLimit: 120\n")
        self.write(self.name, b"int legacy=1;\n\nint current = 2;\n")
        self.git("add", "--", self.name)
        self.git("-c", "user.name=CCA test", "-c", "user.email=cca-test@localhost", "-c", "commit.gpgsign=false",
                 "commit", "-qm", "Create legacy fixture", "--no-verify")
        content = b"int legacy=1;\n\nint current = 3;\n"
        self.write(self.name, content)
        self.assertEqual(self.invoke("--format"), 0)
        self.assertEqual((self.root / self.name).read_bytes(), content)


if __name__ == "__main__":
    unittest.main()
