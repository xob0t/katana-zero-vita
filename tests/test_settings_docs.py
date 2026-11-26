"""yyl.cfg keys: what loadConfig reads matches docs/settings.md (keys, defaults, defining file) and docs/changes.md.

Regex patterns this relies on (plain text, no C parser):
- loadConfig (`void loadConfig(` in loader/loader/main.c up to the first line that is only `}`): every
  `strcmp("<key>", buffer)` is a key. The key's variable is the first `<name> = ` after the line's last
  `buffer) == 0`, except where VARIABLE says otherwise.
- A variable's default is its file-scope definition in loader/loader/*.c (or an added `+` line of patches/*.patch):
  a line starting at column 0 with `int`, `double`, `float`, `bool` (optionally `volatile`/`unsigned`), with one or
  more comma-separated declarators. No initialiser means 0. Lines inside `#ifdef X_TEST` / the `#else` of
  `#ifndef X_TEST` (any macro ending in _TEST or _STUB) are skipped: those are host test builds.
- docs/settings.md: rows "| `key` | ..." in the tables under "## Port settings" (default in the second column, the
  defining files in the last), "## Upstream YoYo Loader settings" (default 0, as the section says) and
  "## Ignored keys" (no default; the first cell may name several keys).
- docs/changes.md: every `key=<number>` in backticks must be a key loadConfig reads.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "loader" / "loader"
DOCS = ROOT / "docs"

# Keys whose variable isn't the first assignment on their line
VARIABLE = {"renderRes": "screen_w"}
# Documented value = code value * factor (stickDeadzone is documented in percent, stored as a fraction)
SCALE = {"stickDeadzone": 100}
# Documented keys that loadConfig doesn't read, with the file that reads them and the text it looks for
RAW_READERS = {"maximizeNewlib": ("sbrk.c", '"maximizeNewlib="')}


def read(path):
    return path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")


def load_config_keys(main_c):
    """{key: variable} for every key loadConfig compares."""
    m = re.search(r"^void loadConfig\(.*?^}$", main_c, re.S | re.M)
    if not m:
        raise AssertionError("loadConfig not found in main.c")
    keys = {}
    for line in m.group(0).split("\n"):
        names = re.findall(r'strcmp\("(\w+)", buffer\)', line)
        if not names:
            continue
        tail = line[line.rfind("buffer) == 0"):]
        var = re.search(r"\b([A-Za-z_]\w*)\s*=(?!=)", tail)
        for key in names:
            keys[key] = VARIABLE.get(key, var.group(1) if var else None)
    return keys


TYPE = r"(?:volatile\s+)?(?:unsigned\s+)?(?:int|double|float|bool)\s+"


def definitions(text, filename):
    """{name: [(value, filename)]} for the file-scope variable definitions in a C file (see the module docstring)."""
    out = {}
    stack = []  # one entry per #if: (is_test_macro, active)
    for raw in text.split("\n"):
        line = raw
        d = re.match(r"\s*#\s*(ifdef|ifndef|if|else|elif|endif)\b\s*(\w*)", line)
        if d:
            kw, macro = d.groups()
            test = bool(re.search(r"_(TEST|STUB)$", macro))
            if kw in ("ifdef", "ifndef", "if"):
                stack.append((test and kw != "if", not (test and kw == "ifdef")))
            elif kw in ("else", "elif") and stack:
                is_test, active = stack[-1]
                stack[-1] = (is_test, not active if is_test else True)
            elif kw == "endif" and stack:
                stack.pop()
            continue
        if not all(active for _, active in stack):
            continue
        m = re.match(rf"^{TYPE}([^;(]+);", line)
        if not m:
            continue
        for decl in m.group(1).split(","):
            dm = re.match(r"\s*\**\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=\s*(.+?))?\s*$", decl)
            if not dm:
                continue
            value = dm.group(2)
            try:
                value = float(value) if value is not None else 0.0
            except ValueError:
                value = None  # not a plain number
            out.setdefault(dm.group(1), []).append((value, filename))
    return out


def all_definitions():
    defs = {}
    for path in sorted(SRC.glob("*.c")):
        for name, found in definitions(read(path), path.name).items():
            defs.setdefault(name, []).extend(found)
    for path in sorted((ROOT / "patches").glob("*.patch")):
        added = "\n".join(l[1:] for l in read(path).split("\n") if l.startswith("+") and not l.startswith("+++"))
        for name, found in definitions(added, f"patches/{path.name}").items():
            defs.setdefault(name, []).extend(found)
    return defs


def section(text, heading):
    m = re.search(rf"^## {re.escape(heading)}\n(.*?)(?=^## |\Z)", text, re.S | re.M)
    if not m:
        raise AssertionError(f"settings.md has no '## {heading}' section")
    return m.group(1)


def table_rows(text):
    """The cells of each table row whose first cell starts with a backticked key."""
    rows = []
    for line in text.split("\n"):
        if line.startswith("| `"):
            rows.append([c.strip() for c in line.strip().strip("|").split("|")])
    return rows


def documented_settings(settings_md):
    """{key: (default or None, defined-in cell or None)}"""
    docs = {}
    for cells in table_rows(section(settings_md, "Port settings")):
        number = re.match(r"-?\d+(?:\.\d+)?", cells[1])
        docs[re.match(r"`(\w+)`", cells[0]).group(1)] = (float(number.group(0)) if number else None, cells[-1])
    for cells in table_rows(section(settings_md, "Upstream YoYo Loader settings")):
        docs[re.match(r"`(\w+)`", cells[0]).group(1)] = (0.0, None)
    for cells in table_rows(section(settings_md, "Ignored keys")):
        for key in re.findall(r"`(\w+)`", cells[0]):
            docs[key] = (None, None)
    return docs


class SettingsDocsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.keys = load_config_keys(read(SRC / "main.c"))
        cls.docs = documented_settings(read(DOCS / "settings.md"))
        cls.defs = all_definitions()

    def test_parsers_find_things(self):
        # A broken regex would make the comparisons below pass vacuously
        self.assertGreater(len(self.keys), 60)
        self.assertEqual(self.keys["smokeClouds"], "smoke_clouds")
        self.assertEqual(self.keys["platTarget"], "platTarget")
        self.assertGreater(len(self.docs), 60)
        self.assertEqual(self.defs["smoke_clouds"], [(2.0, "smokedraw_native.c")])  # the SMOKEDRAW_TEST copy skipped
        self.assertIn((0.0, "option_defaults.c"), self.defs["shaders_default"])  # a comma list

    def test_every_read_key_is_documented(self):
        missing = sorted(set(self.keys) - set(self.docs))
        self.assertEqual(missing, [], "yyl.cfg keys read by loadConfig but missing from docs/settings.md")

    def test_every_documented_key_is_read(self):
        extra = sorted(set(self.docs) - set(self.keys) - set(RAW_READERS))
        self.assertEqual(extra, [], "keys in docs/settings.md that loadConfig doesn't read")
        for key, (filename, text) in RAW_READERS.items():
            self.assertTrue(text in read(SRC / filename), f"{key}: {filename} no longer looks for {text}")

    def test_documented_defaults_match_code(self):
        wrong = []
        for key, (doc_default, defined_in) in sorted(self.docs.items()):
            if doc_default is None or key in RAW_READERS:
                continue
            var = self.keys.get(key)
            found = self.defs.get(var, [])
            values = {v for v, _ in found}
            if len(values) != 1 or None in values:
                wrong.append(f"{key}: variable {var!r} has no single numeric file-scope definition ({found})")
                continue
            code = values.pop() * SCALE.get(key, 1)
            if abs(code - doc_default) > 1e-9:
                wrong.append(f"{key}: settings.md says {doc_default:g}, {found[0][1]} defines {var} = {code:g}")
            if defined_in is not None and not any(f in defined_in for _, f in found):
                wrong.append(f"{key}: {var} is defined in {found[0][1]}, not in the row's files ({defined_in})")
        self.assertEqual(wrong, [])

    def test_changes_md_keys_exist(self):
        keys = set(re.findall(r"`(\w+)=-?\d+`", read(DOCS / "changes.md")))
        self.assertTrue(keys)
        self.assertEqual(sorted(keys - set(self.keys)), [], "docs/changes.md names keys loadConfig doesn't read")

    def test_definitions_parser(self):
        text = ("int a = 3, b, *c = 0;\n#ifndef X_TEST\nint d = 1;\n#else\nint d = 7;\n#endif\n"
                "#ifdef Y_TEST\nint e = 5;\n#endif\n\tint local = 9;\ndouble f = 0.25;\nint g(void);\n")
        defs = definitions(text, "t.c")
        self.assertEqual({k: v[0][0] for k, v in defs.items()}, {"a": 3, "b": 0, "c": 0, "d": 1, "f": 0.25})


if __name__ == "__main__":
    unittest.main()
