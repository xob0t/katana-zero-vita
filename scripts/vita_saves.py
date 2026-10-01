"""Backs up or restores Katana ZERO save files on the Vita over FTP.

Usage: python scripts/vita_saves.py backup <dir> | restore <dir>
Files named netflix_<name> map to the netflix/ cloud-save folder.
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


def backup(dst):
    os.makedirs(dst, exist_ok=True)
    f = connect()
    for name in listing(f, BASE):
        if name == "netflix":
            continue
        with open(os.path.join(dst, name), "wb") as out:
            f.retrbinary(f"RETR {BASE}/{name}", out.write)
    for name in listing(f, f"{BASE}/netflix"):
        with open(os.path.join(dst, "netflix_" + name), "wb") as out:
            f.retrbinary(f"RETR {BASE}/netflix/{name}", out.write)
    f.quit()


def restore(src):
    f = connect()
    for name in os.listdir(src):
        remote = f"{BASE}/netflix/{name[len('netflix_'):]}" if name.startswith("netflix_") else f"{BASE}/{name}"
        with open(os.path.join(src, name), "rb") as data:
            f.storbinary(f"STOR {remote}", io.BytesIO(data.read()))
    f.quit()


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("backup", "restore"):
        raise SystemExit(__doc__)
    (backup if sys.argv[1] == "backup" else restore)(sys.argv[2])
    print(f"{sys.argv[1]} done: {sys.argv[2]}")
