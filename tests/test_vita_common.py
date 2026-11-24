"""scripts/vita_common.py: console address and the shared device lock (no console needed)."""
import importlib
import os
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import vita_common


class HostTest(unittest.TestCase):
    def reload(self, env):
        with patch.dict(os.environ, env, clear=False):
            if "VITA_HOST" not in env:
                os.environ.pop("VITA_HOST", None)
            return importlib.reload(vita_common)

    def tearDown(self):
        importlib.reload(vita_common)

    def test_default_host(self):
        self.assertEqual(self.reload({}).HOST, "192.168.1.151")

    def test_host_from_environment(self):
        self.assertEqual(self.reload({"VITA_HOST": "192.168.1.139"}).HOST, "192.168.1.139")


class LockTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / "vita-device.lock"

    def tearDown(self):
        self.dir.cleanup()

    def test_holds_and_releases(self):
        with vita_common.device_lock("katana: test", path=self.path):
            text = self.path.read_text(encoding="utf-8")
            self.assertTrue(text.startswith("katana: test "))
            self.assertIn(f"pid {os.getpid()}", text)
        self.assertFalse(self.path.exists())

    def test_waits_for_another_holder(self):
        self.path.write_text("other session\npid 1\n", encoding="utf-8")
        with self.assertRaisesRegex(TimeoutError, "other session"):
            with vita_common.device_lock("katana: test", wait=0.2, poll=0.05, path=self.path):
                pass
        self.assertEqual(self.path.read_text(encoding="utf-8"), "other session\npid 1\n")

        threading.Timer(0.2, self.path.unlink).start()
        with vita_common.device_lock("katana: test", wait=5, poll=0.05, path=self.path):
            self.assertTrue(self.path.read_text(encoding="utf-8").startswith("katana: test "))
        self.assertFalse(self.path.exists())

    def test_never_removes_someone_elses_lock(self):
        with vita_common.device_lock("katana: test", path=self.path):
            self.path.unlink()
            self.path.write_text("other session\npid 1\n", encoding="utf-8")
        self.assertEqual(self.path.read_text(encoding="utf-8"), "other session\npid 1\n")


if __name__ == "__main__":
    unittest.main()
