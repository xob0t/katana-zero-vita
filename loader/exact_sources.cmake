# Compile flags of the exact natives, included by CMakeLists.txt. One file per line, sorted, so two pull requests that
# each add a native only conflict when their file names sort next to each other.

# Native replacements must round exactly like the runner (no FMA contraction)
set(EXACT_SOURCES
  loader/bike_draw.c
  loader/clubber_native.c
  loader/collision_native.c
  loader/cone_shadow.c
  loader/dossier_native.c
  loader/ember_draw.c
  loader/explosion_lite.c
  loader/far_rocket.c
  loader/flash_mask.c
  loader/held_native.c
  loader/input_native.c
  loader/laser_native.c
  loader/letter_glyph.c
  loader/letter_native.c
  loader/light_array_native.c
  loader/lightback_native.c
  loader/minecart_native.c
  loader/move_contact_native.c
  loader/particle_lights.c
  loader/particle_native.c
  loader/recolor_lights.c
  loader/region_native.c
  loader/rope_fast.c
  loader/shadow_native.c
  loader/smoke_create.c
  loader/trail_lights.c
  loader/virtual_capture.c
  loader/visibility_native.c
  loader/wavy_native.c
)
set_source_files_properties(${EXACT_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

# Verified against the original ARM code with exactly these flags
set_source_files_properties(loader/dustspawner_native.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-marm;-O2")
# Unicorn-verified with exactly these flags (smokedraw_verify.py, an untracked harness; host build: tools/verify)
set_source_files_properties(loader/smokedraw_native.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-O2")
# Kill-test natives (exp_lightfast, exp_lightfast_inline): Unicorn-verified with exactly these flags. The sibling
# call keeps the fallback into the original event at the caller's stack depth, as in the verified build.
set_source_files_properties(loader/light_fast.c loader/light_fast_inline.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-O2;-foptimize-sibling-calls")
