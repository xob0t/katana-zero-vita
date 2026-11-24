# timer_sites: the constant table of loader/loader/timer_slowmo.c

These read-only scripts find every instruction pair that builds the Android time limit (`global.max_time`) and
slow-mo meter (`bullettime_max`) constants in `libyoyo.so` (APK 1.1.10), and check each against the PC exe. They were
developed in `work/pcmelee/` next to `work/timingaudit/` and expect that layout: `../decomp/annotate.py`,
`../apk110/lib/armeabi-v7a/libyoyo.so` and `../timingaudit/out/` (features from the audit pipeline,
[../README.md](../README.md)).

- `const_sites.py VAR FUNC...` (or `@names.txt`): per function, follows the pointer returned by the getVar of VAR
  through registers and stack spills to the store of a movw/movt-built high word; prints the movw and movt addresses
  and the value.
- `maxtime_sites.txt`: its output for every function that references `max_time`, plus three sites found by hand
  (club_7 Create, dragon_6 Step: register reuse the tracker loses; obj_init Create: missing from the variable index).
- `gen_table.py`: checks each site's instruction words and that the PC version of the same event assigns the PC value
  and not the Android one, then writes `sites_table.c` (the table in `timer_slowmo.c`, with obj_player Other_13's
  `bullettime_max`).
- `scan_all.py`: every movw/movt pair in the code that builds one of the Android values; lists any not in the table
  (two 6000s remain, neither a time limit).
- `scan_lit.py`: the same values as 8-byte literals anywhere in the file (two 760s in `.data`, constant arguments, not
  `bullettime_max`).
