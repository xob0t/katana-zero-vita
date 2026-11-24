# Host test builds of the exact natives

Several natives in `loader/loader/` have a test build behind a define. It swaps the Vita headers (`kubridge.h`,
`so_util.h`, ...) for the small stubs in this folder, so the file compiles outside the loader, as the offline
differential and Unicorn harnesses need it. The harnesses themselves, and the game-derived data they run on
(libyoyo.so, decompiled events, fixtures), are not in the repository.

| Header | Define | File |
|---|---|---|
| `collision_stub.h` | `CLUBBER_TEST` | `clubber_native.c` |
| | `COLLISION_STUB` | `collision_native.c` |
| | `DUSTSPAWNER_TEST` | `dustspawner_native.c` |
| | `FLASH_MASK_STUB` | `flash_mask.c` |
| | `LASER_TEST` | `laser_native.c` |
| | `LETTER_TEST` | `letter_native.c` |
| | `LIGHT_FAST_STUB` | `light_fast.c`, `light_fast_inline.c` |
| | `RAIL_TEST` | `minecart_native.c` |
| | `ROPE_STUB` | `rope_fast.c` |
| | `VIRTUAL_CAPTURE_TEST` | `virtual_capture.c` |
| `smokedraw_test.h` | `SMOKEDRAW_TEST` | `smokedraw_native.c` |

`dossier_native.c` has a `DOSSIER_TEST` build that includes `dossier_stub.h`. That header is lost, so that build
doesn't compile.

To check that a test build still compiles, with the Vita toolchain (`. scripts/env.sh` first in Git Bash):

```
arm-vita-eabi-gcc -mfloat-abi=softfp -fsyntax-only -DLASER_TEST -Itools/verify loader/loader/laser_native.c
```

A harness has to provide the stubbed functions itself, and should compile with the file's flags from
`loader/exact_sources.cmake`.
