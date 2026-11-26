"""exp_* switches: the generated index is current, each switch has one owning file, and every switch is documented.

The scan itself is scripts/exp_index.py (its docstring lists the patterns it relies on).
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import exp_index

DOCS = [ROOT / "docs" / name for name in ("DEVELOPING.md", "settings.md", "changes.md")]


def documented_flags(text):
    """Every exp_<name> the docs name. `exp_x1` to `exp_x8` names the whole range."""
    names = set(re.findall(r"\bexp_([A-Za-z0-9_]+)", text))
    for stem, lo, hi in re.findall(r"\bexp_([A-Za-z_]+?)(\d+)`? to `?exp_\1(\d+)", text):
        names.update(f"{stem}{n}" for n in range(int(lo), int(hi) + 1))
    return names


class ExpFlagsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.readers = exp_index.scan()

    def test_scan_finds_flags(self):
        # A broken regex would make every other test here pass vacuously
        self.assertGreater(len(self.readers), 200)
        self.assertIn("smokedraw0", self.readers)
        self.assertEqual(self.readers["rainkeep5"], {"rain_cheap.c"})

    def test_index_is_current(self):
        current = exp_index.OUT.read_text(encoding="utf-8").replace("\r\n", "\n")
        self.assertEqual(current, exp_index.render(self.readers),
                         "docs/exp-flags.md is out of date: run python scripts/exp_index.py")

    def test_one_owner_per_flag(self):
        self.assertEqual(exp_index.owner_problems(self.readers), [])

    def test_every_flag_documented(self):
        documented = documented_flags("\n".join(p.read_text(encoding="utf-8") for p in DOCS))
        missing = sorted(f"exp_{f} ({', '.join(sorted(files))})" for f, files in self.readers.items()
                         if f not in documented)
        self.assertEqual(missing, [], "switches not named in docs/DEVELOPING.md (feature map), settings.md or changes.md")

    def test_unlisted_dynamic_name_fails(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            Path(d, "x.c").write_text('int f(char *n) { return exp_flag(n); }\n', encoding="utf-8")
            with self.assertRaises(ValueError):
                exp_index.scan(Path(d))

    def test_shared_flag_needs_listing(self):
        self.assertTrue(exp_index.owner_problems({"newflag0": {"a.c", "b.c"}}))
        self.assertEqual(exp_index.owner_problems({"newflag0": {"a.c"}} | {f: files for f, (_, files) in
                                                                           exp_index.SHARED.items()}), [])

    def test_range_in_docs(self):
        self.assertIn("rainkeep5", documented_flags("`exp_rainkeep2` to `exp_rainkeep8`"))


if __name__ == "__main__":
    unittest.main()
