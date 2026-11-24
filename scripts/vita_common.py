"""Shared helpers for the scripts that talk to the Vita through vitacompanion (FTP on 1337, commands on 1338).

The console's address comes from the VITA_HOST environment variable (default 192.168.1.151).

Sessions that share one console take the lock file ~/.t3/vita-device.lock for every console step: a holder line
written with O_EXCL, removed when done. Keep each hold short (about 5 minutes: install, launch, capture, fetch).

    with device_lock("katana: bench club_4_2"):
        ...

or from a shell, holding the lock around one command:

    python scripts/vita_common.py hold "katana: bench club_4_2" -- python scripts/bench.py --label x
"""
import contextlib
import ftplib
import io
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_HOST = "192.168.1.151"
HOST = os.environ.get("VITA_HOST") or DEFAULT_HOST
FTP_PORT = 1337
COMMAND_PORT = 1338
SHARED = "ux0:/data/gms/shared"
LOCK_PATH = Path.home() / ".t3" / "vita-device.lock"


def ftp_connect(host=None, port=FTP_PORT, timeout=30):
    f = ftplib.FTP()
    f.connect(host or HOST, port, timeout=timeout)
    f.login()
    return f


@contextlib.contextmanager
def ftp_session(host=None):
    """One FTP connection, closed with QUIT."""
    f = ftp_connect(host)
    try:
        yield f
    finally:
        try:
            f.quit()
        except (OSError, EOFError, ftplib.Error):
            f.close()


def ftp_get(remote, host=None):
    """Returns the remote file's bytes, None if it doesn't exist."""
    with ftp_session(host) as f:
        buf = io.BytesIO()
        try:
            f.retrbinary(f"RETR {remote}", buf.write)
        except ftplib.error_perm:
            return None
        return buf.getvalue()


def ftp_put(data_or_path, remote, host=None):
    """Uploads bytes or a local file."""
    data = data_or_path if isinstance(data_or_path, bytes) else Path(data_or_path).read_bytes()
    with ftp_session(host) as f:
        f.storbinary(f"STOR {remote}", io.BytesIO(data))


def ftp_delete(remote, host=None):
    """Deletes a remote file; a missing one is not an error."""
    with ftp_session(host) as f:
        try:
            f.delete(remote)
        except ftplib.error_perm:
            pass


def vc(cmd, timeout=120, host=None):
    """Runs a vitacompanion command chain and returns its output."""
    with socket.create_connection((host or HOST, COMMAND_PORT), timeout=10) as s:
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


@contextlib.contextmanager
def device_lock(holder, wait=1800, poll=10, path=None):
    """Holds the shared console lock. Waits up to `wait` seconds for another holder to release it; never removes a
    lock this process didn't write."""
    path = Path(path) if path else LOCK_PATH
    path.parent.mkdir(parents=True, exist_ok=True)
    line = f"{holder} {time.strftime('%H:%M')}"
    deadline = time.monotonic() + wait
    while True:
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            if time.monotonic() >= deadline:
                try:
                    current = path.read_text(encoding="utf-8", errors="replace").splitlines()[0]
                except (OSError, IndexError):
                    current = "?"
                raise TimeoutError(f"{path} still held after {wait} s by: {current}")
            time.sleep(poll)
            continue
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(f"{line}\npid {os.getpid()}\n")
        break
    try:
        yield
    finally:
        try:
            if path.read_text(encoding="utf-8", errors="replace").splitlines()[:1] == [line]:
                path.unlink()
        except OSError:
            pass


def main(argv):
    if len(argv) >= 3 and argv[0] == "hold" and argv[2] == "--" and len(argv) > 3:
        with device_lock(argv[1]):
            return subprocess.call(argv[3:])
    raise SystemExit(__doc__)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
