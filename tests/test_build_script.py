"""Tests of build.py's own logic (patch handling, SDK archives, dependency stamps). Run with
uv run python -m unittest discover -s tests"""
import io
import json
import subprocess
import sys
import tarfile
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import build  # noqa: E402  (build.py, not the build/ output folder)


def quiet(fn, *args):
    with redirect_stdout(io.StringIO()):
        return fn(*args)


class PatchTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.repo = Path(directory.name) / "repo"
        self.repo.mkdir()
        (self.repo / "a.c").write_text("one\ntwo\n")
        git = ["git", "-C", str(self.repo), "-c", "user.name=t", "-c", "user.email=t@t", "-c", "core.autocrlf=false"]
        subprocess.run(git[:3] + ["init", "-q"], check=True)
        subprocess.run(git + ["add", "."], check=True)
        subprocess.run(git + ["commit", "-qm", "base"], check=True)
        (self.repo / "a.c").write_text("one\nTWO\n")
        self.patch = Path(directory.name) / "change.patch"
        self.patch.write_bytes(subprocess.run(git + ["diff"], capture_output=True, check=True).stdout)
        subprocess.run(git + ["checkout", "--", "."], check=True)

    def test_apply_then_already_applied(self):
        self.assertEqual(quiet(build.vitagl_patch_state, self.repo, self.patch), "apply")
        quiet(build.apply_patch, self.repo, self.patch)
        self.assertEqual((self.repo / "a.c").read_text(), "one\nTWO\n")
        self.assertEqual(quiet(build.vitagl_patch_state, self.repo, self.patch), "applied")
        quiet(build.apply_patch, self.repo, self.patch)  # no-op
        self.assertEqual((self.repo / "a.c").read_text(), "one\nTWO\n")

    def test_tree_with_other_changes_fails(self):
        (self.repo / "a.c").write_text("one\nold version of the change\n")
        self.assertEqual(quiet(build.vitagl_patch_state, self.repo, self.patch), "stale")
        with self.assertRaisesRegex(build.BuildError, "does not apply and is not already applied"), \
                redirect_stdout(io.StringIO()):
            build.apply_patch(self.repo, self.patch)
        self.assertIn("checkout -- .", str(build.stale_vitagl(self.repo)))


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.dir = Path(directory.name)
        self.archive = self.dir / "sdk.tar.bz2"
        with tarfile.open(self.archive, "w:bz2") as tar:
            for name in ("vitasdk/arm-vita-eabi/include/psp2/kernel.h", "vitasdk/arm-vita-eabi/include/other.h",
                         "vitasdk/arm-vita-eabi/lib/libSceFoo_stub.a", "vitasdk/arm-vita-eabi/lib/libc.a"):
                data = name.encode()
                info = tarfile.TarInfo(name)
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))

    def test_strip_and_wildcards(self):
        out = self.dir / "out"
        build.extract(self.archive, out, 1, build.NEW_SDK_MEMBERS)
        files = sorted(p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file())
        self.assertEqual(files, ["arm-vita-eabi/include/psp2/kernel.h", "arm-vita-eabi/lib/libSceFoo_stub.a"])
        build.extract(self.archive, out, 2)
        self.assertTrue((out / "lib" / "libc.a").is_file())

    def test_links_are_refused(self):
        with tarfile.open(self.archive, "w:bz2") as tar:
            info = tarfile.TarInfo("vitasdk/bin/link")
            info.type, info.linkname = tarfile.SYMTYPE, "/etc/passwd"
            tar.addfile(info)
        with self.assertRaisesRegex(build.BuildError, "link"):
            build.extract(self.archive, self.dir / "out", 1)

    def test_download_checks_the_hash(self):
        url = self.archive.as_uri()
        digest = build.sha256(self.archive)
        cache = self.dir / "cache"
        with redirect_stdout(io.StringIO()):
            self.assertEqual(build.download((url, digest), cache).read_bytes(), self.archive.read_bytes())
            with self.assertRaisesRegex(build.BuildError, "update the pin"):
                build.download((url, "0" * 64), self.dir / "other")
        self.assertEqual(list((self.dir / "other").iterdir()), [])


class DepsStampTests(unittest.TestCase):
    def test_stamp(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk, deps = Path(directory) / "sdk", Path(directory) / "deps"
            deps.mkdir()
            for f in build.DEPS_INSTALLED:
                (sdk / "arm-vita-eabi" / f).parent.mkdir(parents=True, exist_ok=True)
                (sdk / "arm-vita-eabi" / f).write_text(f)
            self.assertIn("no record", build.deps_stale_reason(sdk, deps))
            stamp = {"inputs": build.deps_inputs(sdk), "sources": build.deps_sources(deps),
                     "outputs": build.deps_outputs(sdk)}
            (deps / build.DEPS_STAMP).write_text(json.dumps(stamp))
            self.assertIsNone(build.deps_stale_reason(sdk, deps))
            (sdk / "arm-vita-eabi/lib/libvitaGL.a").write_text("another vitaGL build")
            self.assertIn("replaced", build.deps_stale_reason(sdk, deps))
            (sdk / "arm-vita-eabi/lib/libvitaGL.a").unlink()
            self.assertIn("missing", build.deps_stale_reason(sdk, deps))
            (deps / "vitaGL").mkdir()
            self.assertIn("sources changed", build.deps_stale_reason(sdk, deps))
            stamp["inputs"]["vitagl_commit"] = "0" * 40
            (deps / build.DEPS_STAMP).write_text(json.dumps(stamp))
            self.assertIn("changed", build.deps_stale_reason(sdk, deps))


if __name__ == "__main__":
    unittest.main()
