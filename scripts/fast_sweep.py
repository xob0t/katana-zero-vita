"""Fast room sweep: boots the game once, then jumps between rooms with the loader's debug room jump
(ux0:data/gms/shared/goto_room) and records fps + top events for each room.

Usage: python scripts/fast_sweep.py [--rooms file] [--seconds 6] [--settle 2] [--no-profile]
Results: work/fast_sweep/<room>.txt and a ranking printed at the end.
Needs debugMode=1; the game is left running.

Triage only: the jump bypasses the game's own room transition, so some rooms load without all of their
content (seen as missing geometry) and read too fast (room_club_4_2: 35.6 fps jumped vs 31.3 fresh boot).
Confirm any before/after comparison with fresh boots (scripts/bench.py --save).
"""
import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def prof_lines(log):
    for line in log.splitlines():
        t = bench.log_time(line)
        m = re.search(r"\[PROF\] fps ([\d.]+) \| process ([\d.]+)ms", line)
        if t and m:
            yield t, float(m.group(1)), float(m.group(2))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rooms", default=os.path.join(ROOT, "work", "sweep_rooms.txt"))
    ap.add_argument("--seconds", type=float, default=6)
    ap.add_argument("--settle", type=float, default=2, help="seconds to skip after entering a room")
    ap.add_argument("--no-profile", action="store_true")
    ap.add_argument("--boot-save", default=os.path.join(ROOT, "work", "testsaves", "bunker2_3"))
    args = ap.parse_args()
    rooms = open(args.rooms).read().split()
    out_dir = os.path.join(ROOT, "work", "fast_sweep")
    os.makedirs(out_dir, exist_ok=True)

    boot = [sys.executable, os.path.join(ROOT, "scripts", "bench.py"), "--label", "fast_sweep_boot", "--no-install",
            "--save", args.boot_save, "--room", "room_bunker2_3", "--seconds", "2"]
    if not args.no_profile:
        boot.append("--profile")
    subprocess.run(boot, check=False)

    results = []
    for room in rooms:
        name = room if room.startswith("room_") else "room_" + room
        bench.put(name.encode(), bench.SHARED + "/goto_room")
        try:
            log = bench.wait_for(rf"Entered Room: {name}(?![0-9_])(?![\s\S]*Entered Room)", 90)
        except (TimeoutError, RuntimeError) as e:
            print(f"{name}: {e}")
            results.append((0.0, name))
            continue
        start = [bench.log_time(l) for l in log.splitlines() if re.search(rf"Entered Room: {name}(?![0-9_])", l)][-1]
        begin, end = start + args.settle, start + args.settle + args.seconds
        while True:
            time.sleep(2)
            log = bench.read_log()
            times = [t for t, _, _ in prof_lines(log)]
            if times and times[-1] >= end:
                break
        window = [(f, p) for t, f, p in prof_lines(log) if begin <= t <= end]
        fps = sum(f for f, _ in window) / len(window) if window else 0.0
        proc = sum(p for _, p in window) / len(window) if window else 0.0
        lines = log.splitlines()
        idx = [i for i, l in enumerate(lines) if "[PROF]" in l and bench.log_time(l) and begin <= bench.log_time(l) <= end]
        detail = []
        if idx:
            block = lines[idx[-1]:idx[-1] + 70]
            detail = [l for l in block if "[PROF]" in l or "STAGE" in l or "EVENT" in l or "VGLMEM" in l][:20]
        with open(os.path.join(out_dir, name[5:] + ".txt"), "w", encoding="utf-8") as f:
            f.write(f"{name} fps {fps:.1f} process {proc:.2f}ms samples {len(window)}\n" + "\n".join(detail) + "\n")
        print(f"{name}: fps {fps:.1f} process {proc:.2f}ms ({len(window)} samples)")
        results.append((fps, name))

    print("\nslowest first:")
    for fps, name in sorted(results):
        print(f"  {fps:5.1f}  {name}")


if __name__ == "__main__":
    main()
