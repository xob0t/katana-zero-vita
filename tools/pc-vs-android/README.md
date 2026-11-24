# timingaudit tools

These tools are static and read-only. Run them from `work/timingaudit` (python 3.14 with capstone, pefile and
pyelftools).

Inputs:
- PC: `C:/Program Files (x86)/Steam/steamapps/common/Katana ZERO/Katana ZERO.exe` and `data.win`.
- Android: `work/apk110/lib/armeabi-v7a/libyoyo.so` and `work/apk110/assets/assets/game.droid`.

The scripts expect to run from a folder laid out like `work/timingaudit` (inputs and `out/` relative to it).

Pipeline (about 15 minutes; the Android extraction takes ~10 of them):

    python -I tools/pc_extract.py      # out/pc_features.json, out/pc_names.json
    python tools/and_extract.py        # out/android_features.json, out/android_names.json (multiprocess)
    python -I tools/match_diff.py      # out/match.json (name matching, aliases)
    python -I tools/linediff.py        # out/linediff.json/.txt (per-line aligned diff, noisy)
    python -I tools/funcdiff.py        # out/funcdiff.json/.txt (numbers / gml calls / assets on one side only)
    python -I tools/review_list.py     # out/const_review.json/.txt (after noise classes)
    python -I tools/vardiff.py         # out/vardiff.txt (variables on one side only)
    python -I tools/roomcc.py          # out/roomcc.txt (room creation code matched by content)
    python -I tools/rooms_diff.py      # out/rooms_diff.txt (room instance placement)
    python tools/varrefs.py @<abs path to name list>   # out/varrefs.txt (Android readers/writers of variables)
    python -I tools/show.py Object_obj_enemy_Step_0 60 70   # side-by-side tokens of one function

What each file does:
- `pcbin.py`: PE loader. Finds the `{char* name, void* func}` GML function table by walking out from
  `gml_Object_obj_bullet_Draw_0`.
- `pc_extract.py`: x86 token extraction.
  - The YYVAR table `{name, id}` is found around the `first_attack_timer` entry.
  - The builtin function-id slots `{name, int slot}` are found around `round`, and `Function_Add` registrations at
    0x19a6080 are read too.
  - Variable accessor helpers (0x18fae90, 0x18fae20, 0x18fbca0, 0x18fbaa0, 0x18fba20, 0x18fb7b0) were identified by
    the `0x80000000` no-array-index argument they share.
  - The line slot is the stack slot that receives the function's name string, plus 4.
- `and_extract.py`: ARM token extraction on top of `work/decomp/annotate.py` (symbols, GOT, PLT and veneers).
- `tokens.py`: the shared event stream. Turns literal stores into doubles (hi/lo word pairing) and kind-15 asset refs
  into tokens, and groups tokens per GML line.
- `match_diff.py`: `ALIAS` maps PC master Step events to their Android user events, and PC GUID-named collision events
  to Android `Collision_<obj>` names.
- `linediff.py`: aligns lines with difflib on variable/call signatures. Cancels code motion within 3 rows (15 rows for
  constants) and maps asset indices by name through both data files (`work/decomp/assets.py`).
- `funcdiff.py` and `review_list.py`: function-level differences and the noise classes. The noise classes are
  documented in `REPORT.md` §3.
