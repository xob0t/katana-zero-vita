#!/usr/bin/env python3
"""The install lines of a yyl.log boot, in order, for comparing two builds (or two boots of one build).

    python scripts/boot_lines.py yyl.log                 # print them
    python scripts/boot_lines.py old.log new.log         # unified diff, exit 1 if they differ

A boot's install lines are the log lines from the start up to and including the `[HOOKS] N entry hooks` line (printed
at the end of katana_late_install, before the main loop; the registry line after it is left out), without their `[seconds.ms]` timestamps and without lines that
differ from boot to boot of the same build: timings, addresses, memory figures (VOLATILE below). If a log holds
several boots, the last one is used.
"""
import difflib
import re
import sys

TIMESTAMP = re.compile(r"^\[\s*\d+\.\d+\]\s?")
# Lines whose content changes between two boots of one build: they are left out
VOLATILE = re.compile(
    r"^\[(BOOT|BOOTFN|BOOTIO|BOOTSAMPLE|PREFETCH|READBENCH|MEM|SPIKE)\]"
    r"|\b\d+(\.\d+)? ?(ms|us|MB/s|s)\b|0x[0-9a-fA-F]{6,}|\bfree\b.*\b(KB|MB)\b|\bsbrk\b|\bheap\b"
    r"|^vitaGL pools: free vram|time\(microsecs\)=",
)
HOOKS = re.compile(r"^\[HOOKS\] \d+ entry hooks")


def boot_lines(text):
    lines = [TIMESTAMP.sub("", l.rstrip("\r")) for l in text.split("\n")]
    end = max((i for i, l in enumerate(lines) if HOOKS.match(l)), default=None)
    if end is None:
        raise ValueError("no [HOOKS] line: the boot didn't reach the end of katana_late_install")
    # The boot starts after the previous boot's [HOOKS] line, if the log holds several
    start = 0
    for i in range(end - 1, -1, -1):
        if HOOKS.match(lines[i]):
            start = i + 1
            break
    return [l for l in lines[start:end + 1] if l and not VOLATILE.search(l)]


def main(argv):
    if len(argv) == 2:
        print("\n".join(boot_lines(open(argv[1], encoding="utf-8", errors="replace").read())))
        return 0
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    a, b = (boot_lines(open(p, encoding="utf-8", errors="replace").read()) for p in argv[1:])
    diff = list(difflib.unified_diff(a, b, argv[1], argv[2], lineterm="", n=1))
    print("\n".join(diff) if diff else f"same {len(a)} install lines")
    return 1 if diff else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
