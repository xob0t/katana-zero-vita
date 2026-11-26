# Host test builds of the exact natives

Several natives in `loader/loader/` have a test build behind a define. It swaps the Vita headers (`kubridge.h`,
`so_util.h`, ...) for the small stubs in this folder, so the file compiles outside the loader, as the offline
differential and Unicorn harnesses need it. The harnesses themselves, and the game-derived data they run on
(libyoyo.so, decompiled events, fixtures), are not in the repository.

| Header | Define | File |
|---|---|---|
| `collision_stub.h` | `COLLISION_STUB` | `collision_native.c` |
| | `DUSTSPAWNER_TEST` | `dustspawner_native.c` |
| | `FLASH_MASK_STUB` | `flash_mask.c` |
| | `LASER_TEST` | `laser_native.c` |
| | `LETTER_TEST` | `letter_native.c` |
| | `LIGHT_FAST_STUB` | `light_fast_inline.c` |
| | `PARTICLE_NATIVE_TEST` | `particle_native.c` |
| | `RAIL_TEST` | `minecart_native.c` |
| | `RECOLOR_LIGHTS_TEST` | `recolor_lights.c` (the harness defines `BUG(n)`, its fault injection; `-D'BUG(n)=0'` for none) |
| | `ROPE_STUB` | `rope_fast.c` |
| | `VIRTUAL_CAPTURE_TEST` | `virtual_capture.c` |
| `smokedraw_test.h` | `SMOKEDRAW_TEST` | `smokedraw_native.c` |

`dossier_native.c` has a `DOSSIER_TEST` build that includes `dossier_stub.h`. That header is lost, so that build
doesn't compile.

`scripts/check_loader.py --only testbuilds` compiles all of them (CI runs it). One by hand, with the Vita toolchain
(`. scripts/env.sh` first in Git Bash):

```
arm-vita-eabi-gcc -mfloat-abi=softfp -fsyntax-only -DLASER_TEST -Itools/verify loader/loader/laser_native.c
```

A harness has to provide the stubbed functions itself, and should compile with the file's flags from
`loader/exact_sources.cmake`.
