"""Exercise real executable replacement and rollback inside disposable directories."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
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
        # the broker always explains itself: a log next to the report, and findings when something went wrong
        self.assertTrue(Path(state["log"]).exists(), state)
        self.assertEqual(bool(state["diagnosis"]), fail_start, state)
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

    def staged(self, setup, wait=40):
        """Run a real update with `setup(installed, digest)` having prepared the disk; returns the report and files."""
        root = Path(tempfile.mkdtemp(prefix="cca-update-broker-"))
        installed = root / "CCA-fixture.exe"
        shutil.copy2(ROOT / "build/desk_test.exe", installed)
        source = root / "release"
        source.mkdir()
        payload = ROOT / "build/update_probe.exe"
        shutil.copy2(payload, source / "release.exe")
        data = payload.read_bytes()
        file = {"name": "release.exe", "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        manifest_path = source / "manifest.json"
        manifest_path.write_text(json.dumps({"schema": 1, "version": "1.0.0-oss", "files": {"desktop": file}}), encoding="utf-8")
        probe = root / "probe.txt"
        env = {**os.environ, "CCA_DATA_DIR": str(root / "data"), "CCA_UPDATE_PROBE_FILE": str(probe),
               "CCA_TEST_UPDATE_SILENT": "1"}
        env.pop("CCA_UPDATE_FAIL_START", None)
        original = installed.read_bytes()
        setup(installed, hashlib.sha256(original).hexdigest())
        subprocess.run([str(installed), "--apply", str(manifest_path)], env=env, timeout=25, capture_output=True,
                       creationflags=subprocess.CREATE_NO_WINDOW)
        plan = json.loads(Path(str(probe) + ".plan").read_text(encoding="utf-8"))
        report = Path(plan["plan"]).with_name("result.json")
        for _ in range(wait * 10):
            if report.exists():
                break
            time.sleep(0.1)
        self.assertTrue(report.exists(), (root, plan))
        return json.loads(report.read_text(encoding="utf-8")), installed, original, data

    @staticmethod
    def hold_open(path, seconds):
        """Keep an exclusive handle on a file, as a virus scanner or the shell briefly does."""
        kernel = ctypes.windll.kernel32
        kernel.CreateFileW.restype = ctypes.c_void_p
        handle = kernel.CreateFileW(str(path), 0xC0000000, 0, None, 3, 0, None)
        assert handle not in (None, ctypes.c_void_p(-1).value), "could not lock the fixture"

        def release():
            time.sleep(seconds)
            kernel.CloseHandle(ctypes.c_void_p(handle))
        threading.Thread(target=release, daemon=True).start()

    def test_leftovers_of_an_interrupted_update_are_cleared(self):
        def setup(installed, digest):
            Path(str(installed) + ".cca-next").write_bytes(b"half a download")
            shutil.copy2(installed, Path(str(installed) + ".cca-previous-" + digest[:12]))
        state, installed, original, data = self.staged(setup)
        self.assertTrue(state["ok"], state)
        self.assertEqual(state["diagnosis"], [])
        self.assertEqual(installed.read_bytes(), data)
        log = Path(state["log"]).read_text(encoding="utf-8")
        self.assertIn("已清除上次留下的暫存檔", log)
        self.assertIn("清除重複的舊版備份", log) if "重試" in log else None

    def test_a_briefly_locked_file_is_retried(self):
        def setup(installed, digest):
            backup = Path(str(installed) + ".cca-previous-" + digest[:12])
            shutil.copy2(installed, backup)
            self.hold_open(backup, 1.3)
        state, installed, original, data = self.staged(setup)
        self.assertTrue(state["ok"], state)
        self.assertEqual(installed.read_bytes(), data)
        log = Path(state["log"]).read_text(encoding="utf-8")
        self.assertIn("檔案正被其他程式使用", log)
        self.assertIn("次嘗試成功", log)

    def test_a_file_that_stays_locked_is_diagnosed_and_the_old_version_kept(self):
        def setup(installed, digest):
            backup = Path(str(installed) + ".cca-previous-" + digest[:12])
            shutil.copy2(installed, backup)
            self.hold_open(backup, 12)
        state, installed, original, data = self.staged(setup)
        self.assertFalse(state["ok"], state)
        self.assertTrue(any("備份檔無法處理" in item and "正被其他程式使用" in item for item in state["diagnosis"]), state)
        self.assertEqual(installed.read_bytes(), original)
        self.assertTrue(Path(state["log"]).exists())

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
