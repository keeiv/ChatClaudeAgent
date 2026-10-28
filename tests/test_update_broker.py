"""Exercise real executable replacement and rollback inside disposable directories."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parent.parent


class BrokerTests(unittest.TestCase):
    def scenario(self, fail_start=False, corrupt=False, same=False, wrong_metadata=False):
        root = Path(tempfile.mkdtemp(prefix="cca-update-broker-"))
        installed = root / "CCA-fixture.exe"
        shutil.copy2(ROOT / "build/desk_test.exe", installed)
        source = root / "release"
        source.mkdir()
        payload = ROOT / ("build/desk_test.exe" if same else "build/update_probe.exe")
        shutil.copy2(payload, source / "release.exe")
        data = payload.read_bytes()
        file = {"name": "release.exe", "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        if corrupt:
            file["sha256"] = "f" * 64
        manifest = {"schema": 1, "version": "1.0.0-luna" if wrong_metadata else "1.0.0-oss", "files": {"desktop": file}}
        manifest_path = source / "manifest.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        probe = root / "probe.txt"
        env = {**os.environ, "CCA_DATA_DIR": str(root / "data"), "CCA_UPDATE_PROBE_FILE": str(probe)}
        env.pop("CCA_UPDATE_FAIL_START", None)
        if fail_start:
            env["CCA_UPDATE_FAIL_START"] = "1"
        original = installed.read_bytes()
        result = subprocess.run([str(installed), "--apply", str(manifest_path)], env=env, timeout=25,
                                capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
        plan = json.loads(Path(str(probe) + ".plan").read_text(encoding="utf-8"))
        if corrupt or wrong_metadata:
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(plan["ok"])
            self.assertEqual(installed.read_bytes(), original)
            return
        if same:
            self.assertFalse(plan["available"])
            self.assertEqual(installed.read_bytes(), original)
            return
        self.assertEqual(result.returncode, 0, (result.stdout, result.stderr, plan))
        report = Path(plan["plan"]).with_name("result.json")
        for _ in range(400):
            if report.exists():
                break
            time.sleep(0.1)
        self.assertTrue(report.exists(), (root, plan))
        state = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(state["ok"], not fail_start, (root, state))
        self.assertEqual(installed.read_bytes(), original if fail_start else data)
        self.assertTrue(probe.exists())
        self.assertEqual(probe.read_text(), "restored" if fail_start else "new-started")
        self.assertTrue((root / "data").exists())

    def test_single_executable_in_local_folder(self):
        root = Path(tempfile.mkdtemp(prefix="cca-update-folder-"))
        installed = root / "CCA-fixture.exe"
        shutil.copy2(ROOT / "build/desk_test.exe", installed)
        inbox = root / "data" / "installers"
        inbox.mkdir(parents=True)
        payload = ROOT / "build/update_probe.exe"
        shutil.copy2(payload, inbox / "ChatClaudeAgent-1.0.0-oss.exe")
        probe = root / "probe.txt"
        env = {**os.environ, "CCA_DATA_DIR": str(root / "data"), "CCA_UPDATE_PROBE_FILE": str(probe)}
        env.pop("CCA_UPDATE_FAIL_START", None)
        result = subprocess.run([str(installed), "--apply-folder", str(inbox)], env=env, timeout=25,
                                capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
        plan = json.loads(Path(str(probe) + ".plan").read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, (result.stdout, result.stderr, plan))
        self.assertEqual(plan["newHash"], hashlib.sha256(payload.read_bytes()).hexdigest())
        report = Path(plan["plan"]).with_name("result.json")
        for _ in range(400):
            if report.exists():
                break
            time.sleep(0.1)
        self.assertTrue(json.loads(report.read_text(encoding="utf-8"))["ok"])
        self.assertEqual(installed.read_bytes(), payload.read_bytes())
        self.assertEqual(probe.read_text(), "new-started")

    def test_real_replace_and_restart(self):
        self.scenario()

    def test_start_failure_restores_original(self):
        self.scenario(fail_start=True)

    def test_hash_mismatch_keeps_original(self):
        self.scenario(corrupt=True)

    def test_payload_metadata_must_match_manifest(self):
        self.scenario(wrong_metadata=True)

    def test_new_version_with_identical_binary_does_not_update(self):
        self.scenario(same=True)


if __name__ == "__main__":
    unittest.main()
