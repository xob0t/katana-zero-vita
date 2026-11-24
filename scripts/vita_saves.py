"""Backs up or restores Katana ZERO save files on the Vita over FTP.

Usage: python scripts/vita_saves.py backup <dir> | restore <dir>   (console address: VITA_HOST, see vita_common.py)
Files named netflix_<name> map to the netflix/ cloud-save folder. backup needs a new or empty directory.
restore prunes extra saves only with a verified completion manifest. Older backups and bench.py test saves
overlay their files. If a restore disconnects during a rename, rerun it to finish replacing the files.
"""
import hashlib
import json
import sys
from contextlib import closing
from pathlib import Path
from uuid import uuid4

from vita_common import FTP_PORT as PORT, HOST, ftp_connect

BASE = "ux0:/data/gms/katanazero/assets"
MANIFEST = ".vita-saves.json"
OLD_MARKER = ".__vita_saves_old_"
UPLOAD_PREFIX = ".__vita_saves_upload_"
BLOCK_SIZE = 1 << 16


def connect():
    return ftp_connect(HOST, PORT, timeout=30)


def listing(f, path):
    return [n.rsplit("/", 1)[-1] for n in f.nlst(path)]


def remote_files(f):
    """Return live and parked saves, keyed by local backup name."""
    live, parked = {}, {}
    for folder, prefix in ((BASE, ""), (f"{BASE}/netflix", "netflix_")):
        for name in listing(f, folder):
            if name in (".", "..", "") or (not prefix and name == "netflix") or name.startswith(UPLOAD_PREFIX):
                continue
            if not prefix and name.startswith("netflix_"):
                raise ValueError(f"Cannot back up ambiguous save name {name!r}")
            original, marker, token = name.rpartition(OLD_MARKER)
            if marker and len(token) == 32:
                parked.setdefault(prefix + original, []).append(f"{folder}/{name}")
            else:
                live[prefix + name] = f"{folder}/{name}"
    return live, parked


def remote_path(name):
    if (not name or name in (".", "..") or name.casefold() == MANIFEST or name.endswith(".part") or OLD_MARKER in name
            or name.startswith(UPLOAD_PREFIX) or any(c in name for c in '/\\:<>"|?*')
            or any(ord(c) < 32 for c in name)):
        raise ValueError(f"Invalid save filename {name!r}")
    if name.startswith("netflix_"):
        cloud_name = name.removeprefix("netflix_")
        if cloud_name in ("", ".", "..") or cloud_name.startswith(UPLOAD_PREFIX):
            raise ValueError(f"Invalid cloud save filename {name!r}")
        return f"{BASE}/netflix/{cloud_name}"
    if name.casefold() == "netflix":
        raise ValueError(f"Invalid save filename {name!r}")
    return f"{BASE}/{name}"


def retrieve(f, remote, write=None):
    size = 0
    digest = hashlib.sha256()

    def receive(chunk):
        nonlocal size
        size += len(chunk)
        digest.update(chunk)
        if write is not None:
            write(chunk)

    f.retrbinary(f"RETR {remote}", receive, BLOCK_SIZE)
    return {"size": size, "sha256": digest.hexdigest()}


def backup(dst):
    dst = Path(dst)
    dst.mkdir(parents=True, exist_ok=True)
    if any(dst.iterdir()):
        raise SystemExit(f"{dst} is not empty: back up into a new directory")
    # Reserve the directory and mark it incomplete before any transfer can fail.
    manifest_part = dst / (MANIFEST + ".part")
    manifest_part.touch(exist_ok=False)
    files = {}
    with closing(connect()) as f:
        saves, parked = remote_files(f)
        live_names = {name.casefold() for name in saves}
        for name, originals in parked.items():
            if name.casefold() not in live_names:  # a previous restore lost its connection mid-rename
                if len(originals) != 1:
                    raise ValueError(f"Multiple parked originals for {name}. Finish the interrupted restore first.")
                saves[name] = originals[0]
        if len({name.casefold() for name in saves}) != len(saves):
            raise ValueError("Save filenames collide on case-insensitive filesystems")
        f.voidcmd("TYPE I")
        for name, remote in saves.items():
            remote_path(name)
            expected_size = f.size(remote)
            part = dst / (name + ".part")
            with part.open("xb") as out:
                files[name] = retrieve(f, remote, out.write)
            if files[name]["size"] != expected_size:
                raise ValueError(f"Incomplete download of {name}")
            part.replace(dst / name)
    manifest_part.write_text(json.dumps({"version": 1, "files": files}, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    manifest_part.replace(dst / MANIFEST)


def restore(src, prune=True):
    src = Path(src)
    entries = {path.name: path for path in src.iterdir()}
    if any(name.endswith(".part") for name in entries):
        raise ValueError(f"{src} contains partial files. Restore requires a completed backup.")
    if any(path.is_symlink() or not path.is_file() for path in entries.values()):
        raise ValueError(f"{src} must contain only regular save and manifest files")
    manifest = entries.pop(MANIFEST, None)
    paths = {name: remote_path(name) for name in entries}
    if len({path.casefold() for path in paths.values()}) != len(paths):
        raise ValueError("Save filenames collide on the Vita")
    files = {}
    for name, path in entries.items():
        with path.open("rb") as data:
            files[name] = {"size": path.stat().st_size, "sha256": hashlib.file_digest(data, "sha256").hexdigest()}
    if manifest is None:
        print(f"{src} has no completion manifest. Restoring its files and keeping extra saves on the Vita.")
        prune = False
    else:
        try:
            saved_files = json.loads(manifest.read_text(encoding="utf-8"))
        except (ValueError, OSError) as exc:
            raise ValueError(f"Cannot read the completion manifest in {src}. Restore cancelled.") from exc
        if saved_files != {"version": 1, "files": files}:
            raise ValueError(f"{src} does not match its completion manifest. Restore cancelled.")

    with closing(connect()) as f:
        live, old = remote_files(f)
        # The filesystem is case-insensitive. Use the listed path when parking an existing save.
        live = {path.casefold(): path for path in live.values()}
        parked = {}
        for name, originals in old.items():
            parked.setdefault(remote_path(name).casefold(), []).extend(originals)
        uploaded = {}
        token = uuid4().hex
        try:
            for i, (name, remote) in enumerate(paths.items()):
                folder = remote.rsplit("/", 1)[0]
                temporary = f"{folder}/{UPLOAD_PREFIX}{token}_{i}"
                uploaded[remote] = temporary  # before STOR, so a failed upload is cleaned up too
                with entries[name].open("rb") as data:
                    f.storbinary(f"STOR {temporary}", data, BLOCK_SIZE)
                if retrieve(f, temporary) != files[name]:
                    raise ValueError(f"Upload verification failed for {name}. Restore cancelled.")
        except BaseException:
            # Nothing has been replaced yet: remove this attempt's uploads and keep the live saves
            for temporary in uploaded.values():
                try:
                    f.delete(temporary)
                except Exception:
                    pass
            raise

        # VitaShell cannot rename over an existing file. Keep the original under a recoverable
        # name until every verified upload is in place. A retry can also finish a half-completed swap.
        replaced = set()
        for remote, temporary in uploaded.items():
            key = remote.casefold()
            original = live.get(key)
            if original is not None:
                previous = original + OLD_MARKER + token
                f.rename(original, previous)
                parked.setdefault(key, []).append(previous)
            f.rename(temporary, remote)
            replaced.add(key)

        for key, originals in parked.items():
            if prune or key in replaced:
                for remote in originals:
                    f.delete(remote)
        for key, remote in live.items():
            if prune and key not in replaced:
                f.delete(remote)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("backup", "restore"):
        raise SystemExit(__doc__)
    (backup if sys.argv[1] == "backup" else restore)(sys.argv[2])
    print(f"{sys.argv[1]} done: {sys.argv[2]}")
