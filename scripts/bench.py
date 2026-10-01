"""Automated Katana ZERO benchmark on a Vita running vitacompanion (FTP 1337, commands 1338).

Installs build/app/eboot.bin (unless --no-install), launches the app, starts a new game,
skips the intro cutscene and records the loader's [PROF] lines during the scripted opening
of room_factory_0. Requires debugMode=1 in ux0:data/gms/katanazero/yyl.cfg.

Usage: python scripts/bench.py [--label name] [--no-install] [--seconds 20] [--profile]
"""
import argparse
import ftplib
import io
import os
import re
import socket
import statistics
import sys
import time

HOST = "192.168.1.151"
SHARED = "ux0:/data/gms/shared"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def vc(cmd, timeout=120):
    """Runs a vitacompanion command chain and returns its output."""
    with socket.create_connection((HOST, 1338), timeout=10) as s:
        s.sendall((cmd + "\n").encode())
        s.settimeout(timeout)
        out = b""
        try:
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                out += chunk
        except socket.timeout:
            pass
    return out.decode(errors="replace")


def ftp():
    f = ftplib.FTP()
    f.connect(HOST, 1337, timeout=30)
    f.login()
    return f


def read_log():
    f = ftp()
    buf = io.BytesIO()
    try:
        f.retrbinary(f"RETR {SHARED}/yyl.log", buf.write)
    except ftplib.error_perm:
        pass
    f.quit()
    return buf.getvalue().decode(errors="replace")


def delete(path):
    f = ftp()
    try:
        f.delete(path)
    except ftplib.error_perm:
        pass
    f.quit()


def put(local_or_bytes, remote):
    f = ftp()
    data = local_or_bytes if isinstance(local_or_bytes, bytes) else open(local_or_bytes, "rb").read()
    f.storbinary(f"STOR {remote}", io.BytesIO(data))
    f.quit()


def wait_for(pattern, timeout, poke=None, interval=2.0):
    """Polls the log until pattern appears; optionally sends a command chain each poll."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if poke:
            vc(poke, timeout=10)
        log = read_log()
        # Broken texture pages end in a GPU crash that takes the whole Vita down: bail out first
        fatal = re.search(r"QOIF error|LZ4 texture page failed", log)
        if fatal:
            vc("quit all", timeout=10)
            raise RuntimeError(f"aborted: {fatal.group(0)!r} in log")
        if re.search(pattern, log):
            return log
        time.sleep(interval)
    raise TimeoutError(f"timed out waiting for {pattern!r}")


def press(button):
    return f"press {button}; wait 150ms; release {button}"


def log_time(line):
    m = re.match(r"\[\s*([\d.]+)\]", line)
    return float(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", default="run")
    ap.add_argument("--no-install", action="store_true")
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--profile", action="store_true", help="enable heavy profilers for this run")
    ap.add_argument("--exp", action="append", default=[], help="experiment flag files to create (exp_<name>)")
    ap.add_argument("--room", default="room_factory_0", help="room to wait for after Continue")
    ap.add_argument("--save", help="save backup dir (scripts/vita_saves.py layout) to install after quitting the game")
    ap.add_argument("--window", action="store_true",
                    help="also wait for the loader's [WINDOW] line (frames 300-899 after entering the room) and print it")
    ap.add_argument("--gameplay", action="store_true",
                    help="play through the opening tutorial (touch attack prompt), walk right, then measure free play")
    args = ap.parse_args()

    vc("nosleep on; screen on; quit all; wait 2s", timeout=10)
    if args.save:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import vita_saves
        vita_saves.restore(args.save)
    if not args.no_install:
        put(os.path.join(ROOT, "build", "app", "eboot.bin"), "ux0:/app/KTNZ00001/eboot.bin")
    delete(f"{SHARED}/yyl.log")
    if args.profile:
        put(b"1", f"{SHARED}/profile")
    else:
        delete(f"{SHARED}/profile")
    f = ftp()
    for name in f.nlst(SHARED):
        base = name.rsplit("/", 1)[-1]
        if base.startswith("exp_") and base[4:] not in args.exp:
            f.delete(f"{SHARED}/{base}")
    f.quit()
    for name in args.exp:
        put(b"1", f"{SHARED}/exp_{name}")

    t0 = time.time()
    vc("launch KTNZ00001; wait 3s; " + press("cross"), timeout=15)
    log = wait_for(r"Entered Room: room_title", 240)
    load_line = [l for l in log.splitlines() if "Entered Room: room_title" in l][0]
    time.sleep(4)
    vc(press("cross") + "; wait 4s", timeout=10)
    log = wait_for(rf"Entered Room: {args.room}(?![0-9_])", 120, poke=press("cross"))
    start = [log_time(l) for l in log.splitlines() if re.search(rf"Entered Room: {args.room}(?![0-9_])", l)][-1]

    if args.gameplay:
        # The opening ends in a time-stop tutorial that only accepts the on-screen attack button
        tap = "press front-touch 0 1730 870; wait 150ms; release front-touch 0; wait 100ms; press square; wait 150ms; release square"
        for _ in range(20):
            vc(tap + "; wait 3500ms", timeout=10)
        vc("press right; wait 1200ms; release right; wait 500ms", timeout=10)
        log = read_log()
        start = max(log_time(l) or 0 for l in log.splitlines() if "[PROF]" in l) - 4

    # Wait until the log covers the whole measurement window
    deadline = time.time() + args.seconds + 60
    time.sleep(args.seconds + 8)
    while True:
        log = read_log()
        times = [log_time(l) for l in log.splitlines() if "[PROF]" in l]
        if (times and times[-1] >= start + 8 + args.seconds) or time.time() > deadline:
            break
        time.sleep(3)
    prof = []
    for line in log.splitlines():
        t = log_time(line)
        m = re.search(r"\[PROF\] fps ([\d.]+) \| process ([\d.]+)ms", line)
        if m and t and start + 8 <= t <= start + 8 + args.seconds:
            prof.append((float(m.group(1)), float(m.group(2))))
    # Leave the game running: its power tick keeps the Vita awake between runs
    window = None
    if args.window:
        deadline = time.time() + 120
        while time.time() < deadline:
            wins = [l for l in log.splitlines() if "[WINDOW]" in l and (log_time(l) or 0) > start]
            if wins:
                window = wins[0]
                break
            time.sleep(3)
            log = read_log()

    fps = [p[0] for p in prof]
    proc = [p[1] for p in prof]
    clock = re.findall(r"CPU clock: (\d+) MHz", log)
    crash = "no crash"
    print(f"[{args.label}] title reached at {log_time(load_line):.1f}s (wall {time.time() - t0:.0f}s total run)")
    phy = re.findall(r"Phycont code block: (0x[0-9A-F]+)", log)
    print(f"[{args.label}] clock {clock[-1] if clock else '?'} MHz, samples {len(prof)}, exp {args.exp}, phycont {phy[-1] if phy else '?'}")
    if prof:
        print(f"[{args.label}] fps mean {statistics.mean(fps):.1f} min {min(fps):.1f} | process mean {statistics.mean(proc):.2f}ms max {max(proc):.2f}ms")
    if window:
        print(f"[{args.label}] {window.split('] ', 1)[1]}")
    with open(os.path.join(ROOT, "work", f"bench_{args.label}.log"), "w", encoding="utf-8") as f:
        f.write(log)


if __name__ == "__main__":
    sys.exit(main())
