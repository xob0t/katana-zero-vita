"""Backs up or restores Katana ZERO save files on the Vita over FTP.

Usage: python scripts/vita_saves.py backup <dir> | restore <dir>
Files named netflix_<name> map to the netflix/ cloud-save folder. backup needs a new or empty directory; restore
makes the Vita's saves exactly the backup's (save files not in it are deleted).
"""
import ftplib
import io
import os
import sys

HOST, PORT = "192.168.1.151", 1337
BASE = "ux0:/data/gms/katanazero/assets"


def connect():
    f = ftplib.FTP()
    f.connect(HOST, PORT, timeout=30)
    f.login()
    return f


def listing(f, path):
    return [n.rsplit("/", 1)[-1] for n in f.nlst(path)]


def remote_files(f):
    """Save files on the Vita: local backup name -> remote path."""
    files = {name: f"{BASE}/{name}" for name in listing(f, BASE) if name != "netflix"}
    files.update({"netflix_" + name: f"{BASE}/netflix/{name}" for name in listing(f, f"{BASE}/netflix")})
    return files


def backup(dst):
    if os.path.isdir(dst) and os.listdir(dst):
        raise SystemExit(f"{dst} is not empty: back up into a new directory")
    os.makedirs(dst, exist_ok=True)
    f = connect()
    for name, remote in remote_files(f).items():
        # Download next to the target first, so a failed transfer never leaves a truncated save in the backup
        part = os.path.join(dst, name + ".part")
        with open(part, "wb") as out:
            f.retrbinary(f"RETR {remote}", out.write)
        os.replace(part, os.path.join(dst, name))
    f.quit()


def restore(src):
    names = [n for n in os.listdir(src) if not n.endswith(".part")]
    f = connect()
    for name, remote in remote_files(f).items():
        if name not in names:
            f.delete(remote)
    for name in names:
        remote = f"{BASE}/netflix/{name[len('netflix_'):]}" if name.startswith("netflix_") else f"{BASE}/{name}"
        with open(os.path.join(src, name), "rb") as data:
            f.storbinary(f"STOR {remote}", io.BytesIO(data.read()))
    f.quit()


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("backup", "restore"):
        raise SystemExit(__doc__)
    (backup if sys.argv[1] == "backup" else restore)(sys.argv[2])
    print(f"{sys.argv[1]} done: {sys.argv[2]}")
