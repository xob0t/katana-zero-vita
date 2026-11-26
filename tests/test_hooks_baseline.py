"""scripts/hooks_baseline.py: a boot log's [HOOKS] lines against tests/expected_hooks.txt (no console needed)."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import hooks_baseline

BASE = ("[HOOKS] 42 entry hooks, 2 stubs, rx arena 20512 of 65536 bytes; not installed: none\n"
        "[HOOKS] also 272 raw hooks, 2 GOT slots, 17 registry swaps (4 chained); conflicts: none\n")


def log(*lines):
    return "\n".join(f"[   12.{i:03d}] {l}" for i, l in enumerate(lines)) + "\n"


class HooksBaselineTest(unittest.TestCase):
    def test_committed_baseline_has_both_lines(self):
        lines = [l for l in hooks_baseline.BASELINE.read_text(encoding="utf-8").splitlines() if l.startswith("[HOOKS]")]
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[0].endswith("not installed: none"))
        self.assertTrue(lines[1].endswith("conflicts: none"))

    def test_same(self):
        self.assertEqual(hooks_baseline.compare(log("boot", *BASE.splitlines(), "Entered Room: room_title"), BASE),
                         (True, []))

    def test_last_boot_counts(self):
        old = BASE.replace("not installed: none", "not installed: x (prologue)")
        ok, _ = hooks_baseline.compare(log(*old.splitlines(), "boot 2", *BASE.splitlines()), BASE)
        self.assertTrue(ok)

    def test_lost_install_fails(self):
        got = BASE.replace("not installed: none", 'not installed: "virtual enemy captures: NOT installed" (logged)')
        ok, msgs = hooks_baseline.compare(log(*got.splitlines()), BASE)
        self.assertFalse(ok)
        self.assertIn("virtual enemy captures", msgs[0])

    def test_conflict_fails(self):
        got = BASE.replace("conflicts: none", "conflicts: builtin_swap on surface_copy from 0x81086161")
        ok, msgs = hooks_baseline.compare(log(*got.splitlines()), BASE)
        self.assertFalse(ok)

    def test_counts_only_warn(self):
        got = BASE.replace("42 entry hooks", "43 entry hooks")
        ok, msgs = hooks_baseline.compare(log(*got.splitlines()), BASE)
        self.assertTrue(ok)
        self.assertEqual(len(msgs), 1)
        self.assertIn("counts differ", msgs[0])

    def test_missing_lines_fail(self):
        self.assertFalse(hooks_baseline.compare(log("no hooks here"), BASE)[0])
        self.assertFalse(hooks_baseline.compare(log(BASE.splitlines()[0]), BASE)[0])  # build without the registry line

    def test_check_log_non_default_doesnt_fail(self):
        got = BASE.replace("not installed: none", "not installed: x (prologue)")
        import io
        import contextlib
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(hooks_baseline.check_log(log(*got.splitlines()), default_config=False))
            self.assertFalse(hooks_baseline.check_log(log(*got.splitlines()), default_config=True))


if __name__ == "__main__":
    unittest.main()
