"""Keep packaging annotations in agreement with the native update comparator."""
import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("cca_release", ROOT / "tools/make_release.py")
RELEASE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RELEASE)


class VersionTests(unittest.TestCase):
    def test_ordinary_sequence(self):
        versions = ["1.0.0-", "1.0.0-oss", "1.0.0-luna", "1.0.0-terra", "1.0.0-sol", "1.0.1-"]
        for before, after in zip(versions, versions[1:]):
            self.assertEqual(RELEASE.next_version(before), after)
            self.assertGreater(RELEASE.parse_version(after), RELEASE.parse_version(before))

    def test_major_annotation_and_legacy(self):
        self.assertEqual(RELEASE.next_version("1.0.0-oss", True), "1.0.0-astra")
        self.assertEqual(RELEASE.next_version("1.0.0-astra"), "1.0.1-")
        self.assertEqual(RELEASE.parse_version("1.0.0"), RELEASE.parse_version("1.0.0-"))

    def test_invalid_versions(self):
        for value in ("1.0", "1.0.0-Luna", "01.0.0-", "1.0.0-beta", "1.0.0--", "1000000.0.0-"):
            with self.assertRaises(ValueError):
                RELEASE.parse_version(value)


if __name__ == "__main__":
    unittest.main()
