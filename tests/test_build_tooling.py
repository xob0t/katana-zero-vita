"""Run with python -m unittest discover -s tests, or pass --benchmark for QOI timings."""
import bz2
import hashlib
import io
import json
import struct
import sys
import tempfile
import timeit
import unittest
import zipfile
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import MagicMock, Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "scripts"), str(ROOT / "web")]
import builder
import extract_art
import patch_plt_calls
import prepare_apk
import vita_saves


def qoi(width, height, opcodes):
    return struct.pack("<4sHHI", b"fioq", width, height, len(opcodes)) + opcodes


def compressed_qoi(width, height, payload):
    return struct.pack("<4sHHI", b"2zoq", width, height, len(payload)) + bz2.compress(payload)


def game_data(*blobs):
    table_size = 4 + 4 * len(blobs)
    txtr = bytearray(struct.pack("<I", len(blobs)) + bytes((4 + 28) * len(blobs)))
    for i, blob in enumerate(blobs):
        entry = table_size + 28 * i
        struct.pack_into("<I", txtr, 4 + 4 * i, 16 + entry)
        struct.pack_into("<I", txtr, entry + 8, len(blob))
        struct.pack_into("<I", txtr, entry + 24, 16 + len(txtr))
        txtr += blob
    chunks = struct.pack("<4sI", b"TXTR", len(txtr)) + txtr + b"AUDO\x04\0\0\0\0\0\0\0"
    return struct.pack("<4sI", b"FORM", len(chunks)) + chunks


class TextureTests(unittest.TestCase):
    def test_all_opcodes(self):
        ops = bytes.fromhex("00 41 60 00 80 c0 00 e0 00 00 ff 11 22 33 44")
        image = qoi(10, 4, ops)  # 1 + 2 + 33 + 1 + 1 + 1 + 1
        self.assertEqual(extract_art.validate_gm_qoi(image), (10, 4))
        self.assertEqual(extract_art.decode_gm_qoi(image).size, (10, 4))

    def test_invalid_opcode_streams(self):
        for width, height, opcodes in (
            (0, 1, b"\x00"), (1, 4097, b"\x00"), (1, 1, b""),
            (1, 1, b"\xc0"), (1, 1, b"\xe0\x00"), (1, 1, b"\xff\x00\x00\x00"),
            (1, 1, b"\x60"), (1, 1, b"\x41"), (2, 1, b"\x00"), (1, 1, b"\x00\x00"),
        ):
            with self.subTest(width=width, height=height, opcodes=opcodes), self.assertRaises(ValueError):
                extract_art.validate_gm_qoi(qoi(width, height, opcodes))

    def test_pages_are_checked_before_recompression(self):
        valid = qoi(1, 1, b"\x40")
        bad_length = valid[:8] + b"\x02\0\0\0" + valid[12:]
        packed = compressed_qoi(1, 1, valid)
        for invalid in (b"unknown tex header", bad_length, packed[:-1], packed + b"trailing",
                        compressed_qoi(1, 2, valid), compressed_qoi(1, 1, qoi(1, 1, b"\x41"))):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, "texture page 1"):
                prepare_apk.recompress_textures(game_data(valid, invalid), False)
        rebuilt = prepare_apk.recompress_textures(game_data(valid, packed), False)
        _, audo_off, _ = prepare_apk.parse_chunks(rebuilt)[-1]
        entries = prepare_apk.texture_entries(rebuilt, 16, audo_off)
        for _, ptr, length in entries:
            self.assertEqual(extract_art.unpack_gm_texture(rebuilt[ptr:ptr + length]), (valid, (1, 1)))
        self.assertEqual(rebuilt[entries[1][1] + 12:][:4], b"LZ4H")

    def test_reencoded_glyph_page_is_also_checked(self):
        original = game_data(compressed_qoi(1, 1, qoi(1, 1, b"\x40")))
        with (patch.object(prepare_apk, "patched_pages", return_value={0: object()}),
              patch.object(prepare_apk, "encode_gm_qoi", return_value=qoi(1, 1, b"\xff")),
              self.assertRaisesRegex(ValueError, "texture page 0: truncated")):
            prepare_apk.recompress_textures(original)

    def test_bad_page_bounds(self):
        raw = game_data(qoi(1, 1, b"\x40"))
        for offset, value in ((16, 1000), (20, 0), (32, 1000), (48, len(raw) - 4), (48, 20)):
            damaged = bytearray(raw)
            struct.pack_into("<I", damaged, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaisesRegex(ValueError, "TXTR"):
                prepare_apk.recompress_textures(damaged, False)
        with self.assertRaisesRegex(ValueError, "overlapping TXTR"):
            damaged = bytearray(game_data(qoi(1, 1, b"\x40"), qoi(1, 1, b"\x40")))
            struct.pack_into("<I", damaged, 24, 28)  # both table slots point to the first entry
            prepare_apk.recompress_textures(damaged, False)


class ApkTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.apk = self.root / "original.apk"

    def make_apk(self, assets, manifest=True):
        with zipfile.ZipFile(self.apk, "w") as z:
            if manifest:
                z.writestr(prepare_apk.MANIFEST, b"manifest")
            z.writestr(prepare_apk.KEEP_LIB, b"unknown runner")
            for name, data in assets.items():
                z.writestr(name, data)

    def test_shared_apk_layout_and_warning(self):
        for assets, marker in (
            ({"assets/game.droid": b"flat", "assets/assets/stray": b"unused", "assets/music.ogg": b"flat"}, b"flat"),
            ({"assets/game.droid": b"flat", "assets/assets/game.droid": b"nested",
              "assets/music.ogg": b"flat", "assets/assets/music.ogg": b"nested"}, b"nested"),
        ):
            with self.subTest(marker=marker):
                self.make_apk(assets)
                verdict = builder.inspect(self.apk)
                self.assertTrue(verdict["ok"])
                self.assertEqual(extract_art.load_game_data(self.apk), marker)
                out, logs = io.BytesIO(), io.StringIO()
                with (patch.object(prepare_apk, "recompress_textures", lambda data, _: data),
                      patch.object(patch_plt_calls, "patch", return_value=(b"patched", ["patched"])),
                      redirect_stdout(logs)):
                    prepare_apk.prepare(self.apk, out)
                self.assertIn(verdict["warning"], logs.getvalue())
                with zipfile.ZipFile(out) as z:
                    self.assertEqual(set(z.namelist()), prepare_apk.REQUIRED_OUTPUT | {"assets/music.ogg"})
                    self.assertEqual(z.read("assets/game.droid"), marker)
                    self.assertEqual(z.read("assets/music.ogg"), marker)

    def test_missing_manifest_and_cli_failure_preserve_existing_apk(self):
        self.make_apk({"assets/game.droid": b"bad"}, manifest=False)
        self.assertIn("AndroidManifest.xml", builder.inspect(self.apk)["error"])
        self.make_apk({"assets/game.droid": game_data(qoi(1, 1, b"\x40"))})
        out = self.root / "game.apk"
        out.write_bytes(b"previous build")
        with (patch.object(sys, "argv", ["prepare", "--xbox-glyphs", str(self.apk), str(out)]),
              self.assertRaisesRegex(SystemExit, "invalid runner ELF"),
              redirect_stdout(io.StringIO())):
            prepare_apk.main()
        self.assertEqual(out.read_bytes(), b"previous build")
        self.assertEqual(set(self.root.iterdir()), {self.apk, out})

    def test_web_default_log_and_private_workspace(self):
        work = self.root / "work"
        work.mkdir()
        keep = work / "keep"
        keep.write_text("user file")
        for name, _ in builder.VPK_FILES:
            (self.root / name).touch()
        private = []

        def prepare(_, dest, glyphs, *, log):
            path = Path(dest)
            private.append(path.parent)
            path.write_bytes(b"game")
            log("game.droid: recompressed")

        def art(_, dest, *, log):
            for name, _ in builder.LIVEAREA:
                (Path(dest) / name).touch()
            log("LiveArea ready")

        out, log, progress = self.root / "out.vpk", io.StringIO(), Mock()
        with (patch.object(prepare_apk, "prepare", prepare), patch.object(builder.make_livearea, "game_art", art),
              redirect_stdout(log)):
            builder.build(self.apk, out, self.root, progress=progress)
        self.assertIn("game.droid: recompressed", log.getvalue())
        progress.assert_called_with(1.0, "Done")
        self.assertEqual(keep.read_text(), "user file")
        self.assertNotEqual(private[0], work)
        self.assertFalse(private[0].exists())
        with zipfile.ZipFile(out) as z:
            self.assertEqual(z.read("game.apk"), b"game")
        previous = out.read_bytes()
        (self.root / builder.VPK_FILES[1][0]).unlink()
        with (patch.object(prepare_apk, "prepare", prepare), patch.object(builder.make_livearea, "game_art", art),
              redirect_stdout(log), self.assertRaises(FileNotFoundError)):
            builder.build(self.apk, out, self.root, progress=progress)
        self.assertEqual(out.read_bytes(), previous)
        self.assertFalse(private[1].exists())


class Symbol(dict):
    def __init__(self, name, addr, size, defined=True):
        super().__init__(st_value=addr, st_size=size, st_info={"type": "STT_FUNC"},
                         st_shndx=1 if defined else "SHN_UNDEF")
        self.name = name


class RunnerTests(unittest.TestCase):
    def setUp(self):
        p = patch_plt_calls
        elf = MagicMock(elfclass=32, little_endian=True)
        elf.__getitem__.side_effect = {"e_type": "ET_DYN", "e_machine": "EM_ARM"}.__getitem__
        self.caller = 0x40
        self.reader = 0x50
        self.destination = 0x1a0
        self.plt = 0x200
        self.site = self.reader + 0xdc
        self.raw = bytearray(0x400)
        symbols = [Symbol("caller", self.caller, 16), Symbol(p.QOI_READER, self.reader, 0xe0),
                   Symbol(p.BZIP2, 0, 0, defined=False), Symbol("callee", self.destination, 4)]
        self.rels = Mock()
        self.rels.iter_relocations.return_value = [
            {"r_info_sym": 2 + i, "r_info_type": 22, "r_offset": 0x300 + 4 * i} for i in range(2)
        ]
        self.dynsym = Mock()
        self.dynsym.get_symbol.side_effect = symbols.__getitem__
        self.dynsym.get_symbol_by_name.return_value = [symbols[1]]
        self.dynsym.iter_symbols.side_effect = lambda: iter(symbols)
        self.sections = {
            ".text": {"sh_addr": 0x40, "sh_offset": 0x40, "sh_size": 0x1c0},
            ".plt": {"sh_addr": self.plt, "sh_offset": self.plt, "sh_size": 64},
            ".rel.plt": self.rels, ".dynsym": self.dynsym, ".note.gnu.build-id": None,
        }
        elf.get_section_by_name.side_effect = self.sections.get
        self.enterContext(patch.object(p, "ELFFile", return_value=elf))
        for i, rel in enumerate(self.rels.iter_relocations.return_value):
            entry = self.plt + 32 + i * 16
            struct.pack_into("<III", self.raw, entry, 0xe28fc000, 0xe28cc000, 0xe5bcf000 | (rel["r_offset"] - entry - 8))
        struct.pack_into("<I", self.raw, self.site, self.bl(self.site, self.plt + 32))
        struct.pack_into("<I", self.raw, self.caller, self.bl(self.caller, self.plt + 48))

    @staticmethod
    def bl(site, destination):
        return 0xeb000000 | (((destination - site - 8) // 4) & 0xffffff)

    def test_unknown_runner_with_exact_layout_can_be_patched(self):
        out, _ = patch_plt_calls.patch(bytes(self.raw))
        self.assertEqual(struct.unpack_from("<I", out, self.caller)[0], self.bl(self.caller, self.destination))

    def test_bad_plt_and_bad_decoder_call(self):
        self.raw[self.plt + 48] ^= 1
        with self.assertRaisesRegex(ValueError, "PLT instructions"):
            patch_plt_calls.patch(bytes(self.raw))
        self.raw[self.plt + 48] ^= 1
        struct.pack_into("<I", self.raw, self.site, self.bl(self.site, self.plt + 48))
        with self.assertRaisesRegex(ValueError, "unexpected bzip2 call"):
            patch_plt_calls.patch(bytes(self.raw))

    def test_known_plan_detects_missing_call_even_if_excluded(self):
        self.sections[".note.gnu.build-id"] = Mock()
        self.sections[".note.gnu.build-id"].iter_notes.return_value = [{"n_desc": "test", "n_type": "NT_GNU_BUILD_ID"}]
        hook = self.bl(self.site, self.plt + 32)
        plan = struct.pack("<6I", self.site, hook, hook, self.caller,
                           self.bl(self.caller, self.plt + 48), self.bl(self.caller, self.destination))
        with patch.dict(patch_plt_calls.EXPECTED_PATCH_SITES, {"test": hashlib.sha256(plan).hexdigest()}):
            out, _ = patch_plt_calls.patch(bytes(self.raw), exclude=["callee"])
            self.assertEqual(out, bytes(self.raw))
            struct.pack_into("<I", self.raw, self.caller, 0xe1a00000)
            with self.assertRaisesRegex(ValueError, "patch sites differ"):
                patch_plt_calls.patch(bytes(self.raw), exclude=["callee"])


class FakeFTP:
    def __init__(self, files):
        self.files = files
        self.events = []
        self.bad_upload = False
        self.short_download = False
        self.fail_publish = False

    def nlst(self, folder):
        return [path for path in self.files if path.rsplit("/", 1)[0] == folder]

    def voidcmd(self, command):
        assert command == "TYPE I"

    def size(self, path):
        return len(self.files[path])

    def retrbinary(self, command, receive, _):
        path = command.removeprefix("RETR ")
        self.events.append(("read", path))
        data = self.files[path]
        receive(data[:-1] if self.short_download else data)

    def storbinary(self, command, data, _):
        path = command.removeprefix("STOR ")
        self.events.append(("upload", path))
        payload = data.read()
        self.files[path] = bytes([payload[0] ^ 1]) + payload[1:] if self.bad_upload else payload

    def rename(self, old, new):
        self.events.append(("rename", old, new))
        if self.fail_publish and vita_saves.UPLOAD_PREFIX in old:
            raise OSError("disconnected before rename")
        if new in self.files:
            raise OSError("destination exists")
        self.files[new] = self.files.pop(old)

    def delete(self, path):
        self.events.append(("delete", path))
        del self.files[path]

    def close(self):
        pass


class SaveTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.dst = Path(self.directory.name)
        self.save = vita_saves.BASE + "/KatanaSave.zero"
        self.cloud = vita_saves.BASE + "/netflix/slot.sav"
        self.extra = vita_saves.BASE + "/extra.zero"
        self.ftp = FakeFTP({self.save: b"new", self.cloud: b"cloud"})
        self.connect = self.enterContext(patch.object(vita_saves, "connect", return_value=self.ftp))

    def test_backup_and_restore_verify_before_deleting(self):
        vita_saves.backup(self.dst)
        manifest = json.loads((self.dst / vita_saves.MANIFEST).read_text())
        self.assertEqual(manifest["files"]["KatanaSave.zero"],
                         {"size": 3, "sha256": hashlib.sha256(b"new").hexdigest()})
        self.ftp.files = {self.save: b"original", self.extra: b"keep until commit"}
        self.ftp.events.clear()
        vita_saves.restore(self.dst)
        self.assertEqual(self.ftp.files, {self.save: b"new", self.cloud: b"cloud"})
        actions = [action for action, *_ in self.ftp.events]
        self.assertEqual(actions[:4], ["upload", "read", "upload", "read"])
        self.assertEqual(actions[4:], ["rename", "rename", "rename", "delete", "delete"])

    def test_corrupt_upload_never_replaces_or_prunes(self):
        vita_saves.backup(self.dst)
        self.ftp.files = {self.save: b"original", self.extra: b"keep"}
        self.ftp.bad_upload = True
        with self.assertRaisesRegex(ValueError, "Upload verification failed"):
            vita_saves.restore(self.dst)
        self.assertEqual(self.ftp.files[self.save], b"original")
        self.assertEqual(self.ftp.files[self.extra], b"keep")
        self.assertFalse(any(event[0] in ("rename", "delete") for event in self.ftp.events))

    def test_disconnect_leaves_original_available_and_retry_finishes(self):
        vita_saves.backup(self.dst)
        self.ftp.files = {self.save: b"original", self.extra: b"keep"}
        self.ftp.fail_publish = True
        with self.assertRaisesRegex(OSError, "disconnected"):
            vita_saves.restore(self.dst)
        self.assertEqual(self.ftp.files[self.extra], b"keep")
        with tempfile.TemporaryDirectory() as rescue:
            vita_saves.backup(rescue)
            self.assertEqual((Path(rescue) / "KatanaSave.zero").read_bytes(), b"original")
        self.ftp.fail_publish = False
        vita_saves.restore(self.dst)
        self.assertEqual(self.ftp.files[self.save], b"new")
        self.assertEqual(self.ftp.files[self.cloud], b"cloud")
        self.assertNotIn(self.extra, self.ftp.files)

    def test_interrupted_backup_and_tampered_manifest_refuse_before_connect(self):
        self.ftp.short_download = True
        with self.assertRaisesRegex(ValueError, "Incomplete download"):
            vita_saves.backup(self.dst)
        self.connect.reset_mock()
        with self.assertRaisesRegex(ValueError, "partial files"):
            vita_saves.restore(self.dst)
        self.connect.assert_not_called()
        with tempfile.TemporaryDirectory() as complete:
            self.ftp.short_download = False
            vita_saves.backup(complete)
            (Path(complete) / "KatanaSave.zero").write_bytes(b"bad")
            self.connect.reset_mock()
            with self.assertRaisesRegex(ValueError, "does not match"):
                vita_saves.restore(complete)
            self.connect.assert_not_called()

    def test_legacy_overlay_and_explicit_no_prune(self):
        (self.dst / "KatanaSave.zero").write_bytes(b"legacy")
        with redirect_stdout(io.StringIO()) as log:
            vita_saves.restore(self.dst)
        self.assertIn("keeping extra saves", log.getvalue())
        self.assertEqual(self.ftp.files[self.save], b"legacy")
        self.assertEqual(self.ftp.files[self.cloud], b"cloud")
        with tempfile.TemporaryDirectory() as complete:
            vita_saves.backup(complete)
            self.ftp.files[self.extra] = b"extra"
            vita_saves.restore(complete, prune=False)
            self.assertEqual(self.ftp.files[self.extra], b"extra")


def benchmark():
    width = height = 2048
    for name, opcodes in (
        ("dense single-pixel opcodes", b"\x80" * (width * height)),
        ("mixed diffs and runs", bytes.fromhex("80 c0 00 e0 00 00 4c") * (width * height // 16)),
    ):
        data = qoi(width, height, opcodes)
        timer = timeit.Timer(lambda: extract_art.validate_gm_qoi(data))
        samples = sorted(timer.repeat(repeat=5, number=1))
        print(f"{name}, {width}x{height}, {len(opcodes):,} opcode bytes: {samples[2] * 1000:.1f} ms median")


if __name__ == "__main__":
    if sys.argv[1:] == ["--benchmark"]:
        benchmark()
    else:
        unittest.main()
