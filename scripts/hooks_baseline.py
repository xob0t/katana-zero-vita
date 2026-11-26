#!/usr/bin/env python3
"""Compares a boot's [HOOKS] lines with the committed baseline, tests/expected_hooks.txt.

    python scripts/hooks_baseline.py yyl.log             # check; exit 1 if what installed differs
    python scripts/hooks_baseline.py --non-default yyl.log   # a boot with exp_* files or CFG: print, exit 0
    python scripts/hooks_baseline.py --update yyl.log    # write the baseline from a default-config boot

hooks_report() (loader/loader/hooks.c) logs two lines at the end of katana_late_install:

    [HOOKS] 42 entry hooks, 2 stubs, rx arena 20512 of 65536 bytes; not installed: none
    [HOOKS] also 24 raw hooks, 2 GOT slots, 14 registry swaps (3 chained); conflicts: none

The baseline holds both lines of a boot with the default yyl.cfg and no exp_* files. A check fails when the
"not installed" or "conflicts" part differs: a feature stopped installing (the vcapture loss from #115 to #120 would
have shown on the first bench run), or two modules now patch the same target. Counts that differ are only reported:
a PR that adds a hook changes them, and then updates the baseline with --update.

A boot with exp_* files or the profiler on may install a different set on purpose; scripts/bench.py and
work/vita3k/kz_verify.sh still print the comparison then, labelled, and fail only for a default boot.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASELINE = ROOT / "tests" / "expected_hooks.txt"
TIMESTAMP = re.compile(r"^\[\s*\d+\.\d+\]\s?")


def hooks_lines(text):
    """The last boot's two [HOOKS] summary lines (without timestamps), or [] if the log has none."""
    lines = [TIMESTAMP.sub("", l.rstrip("\r")) for l in text.splitlines()]
    first = [i for i, l in enumerate(lines) if re.match(r"\[HOOKS\] \d+ entry hooks", l)]
    if not first:
        return []
    i = first[-1]
    out = [lines[i]]
    for l in lines[i + 1:i + 40]:
        if l.startswith("[HOOKS] also "):
            out.append(l)
            break
    return out


def parts(line):
    """(counts text, list text) of one summary line: what's before and after 'not installed:' / 'conflicts:'."""
    m = re.match(r"(.*?); (?:not installed|conflicts): (.*)$", line)
    return (m.group(1), m.group(2)) if m else (line, "")


def compare(log_text, baseline_text):
    """(ok, messages): ok is False when the install or conflict lists differ from the baseline."""
    got = hooks_lines(log_text)
    want = [l for l in baseline_text.splitlines() if l.startswith("[HOOKS]")]
    if not got:
        return False, ["no [HOOKS] line in the log: the boot didn't reach the end of katana_late_install"]
    ok, msgs = True, []
    for label, w, g in zip(("hooks", "registry"), want, got + [""] * (len(want) - len(got))):
        (wc, wl), (gc, gl) = parts(w), parts(g)
        if not g:
            ok = False
            msgs.append(f"{label}: no line in the log (expected: {w})")
            continue
        if wl != gl:
            ok = False
            msgs.append(f"{label}: DIFFERENT FROM THE BASELINE\n  expected: {wl}\n  got:      {gl}")
        if wc != gc:
            msgs.append(f"{label} counts differ (update tests/expected_hooks.txt if this build adds or drops a hook)"
                        f"\n  expected: {wc}\n  got:      {gc}")
    return ok, msgs


def check_log(log_text, default_config=True, label="[HOOKS]"):
    """Prints the comparison; returns False only when a default-config boot differs (for the bench scripts)."""
    ok, msgs = compare(log_text, BASELINE.read_text(encoding="utf-8"))
    if ok and not msgs:
        print(f"{label} matches tests/expected_hooks.txt")
        return True
    banner = "" if default_config else " (exp flags or profiler on: differences may be expected)"
    print(f"{label} {'differs from' if not ok else 'counts differ from'} tests/expected_hooks.txt{banner}")
    for m in msgs:
        print("  " + m.replace("\n", "\n  "))
    return ok or not default_config


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log")
    ap.add_argument("--update", action="store_true", help="write tests/expected_hooks.txt from this log")
    ap.add_argument("--non-default", action="store_true",
                    help="the boot had exp_* files or a changed yyl.cfg: print the comparison, don't fail")
    args = ap.parse_args()
    text = Path(args.log).read_text(encoding="utf-8", errors="replace")
    if args.update:
        got = hooks_lines(text)
        if len(got) != 2:
            print("the log has no complete pair of [HOOKS] lines", file=sys.stderr)
            return 1
        head = [l for l in BASELINE.read_text(encoding="utf-8").splitlines() if l.startswith("#")] \
            if BASELINE.exists() else []
        BASELINE.write_text("\n".join(head + got) + "\n", encoding="utf-8", newline="\n")
        print(f"wrote {BASELINE.relative_to(ROOT)}")
        return 0
    return 0 if check_log(text, default_config=not args.non_default) else 1


if __name__ == "__main__":
    sys.exit(main())
