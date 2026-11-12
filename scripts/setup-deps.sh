#!/usr/bin/env bash
# Builds the softfp dependencies the loader needs, mirroring loader/.github/workflows/compilation.yml.
# Assumes scripts/setup-sdk.sh already installed the toolchain into $VITASDK.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/env.sh"
BUILD="${BUILD_DIR:-/c/vitasdk-build}"
JOBS="${JOBS:-$(nproc)}"
LIB="$VITASDK/arm-vita-eabi/lib"
mkdir -p "$BUILD"

cp "$ROOT"/loader/stubs/libkubridge_stub.a "$ROOT"/loader/stubs/libkubridge_stub_weak.a "$LIB"/
cp "$ROOT"/loader/libs/libsndfile.a "$ROOT"/loader/libs/libcurl.a "$ROOT"/loader/libs/libssl.a "$ROOT"/loader/libs/libcrypto.a "$LIB"/

fetch() { # fetch <url> <dir> [branch]
  if [ ! -d "$BUILD/$2" ]; then
    git clone -q --depth 1 ${3:+-b "$3"} "$1" "$BUILD/$2"
  fi
}

VITA_CMAKE=(-DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" -DCMAKE_BUILD_TYPE=Release)

fetch https://github.com/Rinnegatamante/openal-soft.git openal-soft vgl
cmake -S "$BUILD/openal-soft" -B "$BUILD/openal-soft/build" "${VITA_CMAKE[@]}" -DCMAKE_C_FLAGS=-mfloat-abi=softfp
cmake --build "$BUILD/openal-soft/build" -j "$JOBS" --target install

fetch https://github.com/bythos14/SceShaccCgExt.git SceShaccCgExt
cmake -S "$BUILD/SceShaccCgExt" -B "$BUILD/SceShaccCgExt/build"
cmake --build "$BUILD/SceShaccCgExt/build" -j "$JOBS" --target install

fetch https://github.com/Rinnegatamante/vitaShaRK.git vitaShaRK
make -C "$BUILD/vitaShaRK" -j "$JOBS" install

fetch https://github.com/Rinnegatamante/vitaGL.git vitaGL
# Our vitaGL changes (patches/vitagl.patch) are made against this commit
VITAGL_COMMIT=464876a79cdd00650bb0eb9629b85d81f9908fd8
if [ "$(git -C "$BUILD/vitaGL" rev-parse HEAD)" != "$VITAGL_COMMIT" ]; then
  git -C "$BUILD/vitaGL" fetch -q --depth 1 origin "$VITAGL_COMMIT"
  git -C "$BUILD/vitaGL" checkout -q "$VITAGL_COMMIT"
fi
if git -C "$BUILD/vitaGL" apply --check "$ROOT/patches/vitagl.patch" 2>/dev/null; then
  git -C "$BUILD/vitaGL" apply "$ROOT/patches/vitagl.patch"
fi
make -C "$BUILD/vitaGL" DEPTH_STENCIL_HACK=1 SOFTFP_ABI=1 NO_DEBUG=1 SHARED_RENDERTARGETS=1 NO_SPLASHSCREEN=1 ENABLE_LEGACY_PIPELINE=1 HAVE_SHADER_CACHE=1 -j "$JOBS" install

fetch https://github.com/frangarcj/opensles.git opensles
# Larger output grain and a high-priority playback thread: the stock 128-frame port crackles under load
if git -C "$BUILD/opensles" apply --check "$ROOT/patches/opensles.patch" 2>/dev/null; then
  git -C "$BUILD/opensles" apply "$ROOT/patches/opensles.patch"
fi
make -C "$BUILD/opensles/libopensles" -j "$JOBS" install

echo "deps OK"
