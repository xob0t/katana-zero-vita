# Notes for Claude

## Editing C sources from scripts

- When patching files with a Python heredoc, put inserted C code in **raw strings** (`r'''...'''`) and write escapes
  once (`\n`). A `\\n` in a normal Python string arrives as a real line break and splits C string literals.
- For small edits to lines containing escapes (`debugPrintf("...\n")`), prefer the Edit tool.

## Building

- When checking a build, grep case-insensitively for `error` and require the `Built target` line; a failed build
  silently leaves the previous `eboot.bin` in place and every following test runs old code.

## Vita safety

- A texture page that fails to decode crashes the GPU and the whole Vita. The loader refuses to start if its texture
  self-test fails; never bypass it, and upload runner builds only from `scripts/patch_plt_calls.py`.
- Back up the user's saves (`scripts/vita_saves.py backup`) before installing test saves; restore them afterwards.
