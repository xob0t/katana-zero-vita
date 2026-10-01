#!/usr/bin/env bash
# Installs a softfp VitaSDK natively on Windows (Git Bash), no WSL needed.
#
# The loader must be built softfp to call into the Android .so. The last softfp
# Windows toolchain is from 2022, so its Vita system headers/stubs are refreshed
# from the current VitaSDK (these are float-ABI independent).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK=/c/vitasdk-softfp
DL=/c/vitasdk-dl
SOFTFP_TAG=softfp-win-v2.228
mkdir -p "$SDK" "$DL/pkgs" "$DL/new"

# 1. softfp toolchain
gh release download "$SOFTFP_TAG" -R vitasdk/buildscripts -p "*.tar.bz2" -D "$DL" --clobber
tar -xjf "$DL"/vitasdk-x86_64-w64-mingw32-2022-*.tar.bz2 -C "$SDK" --strip-components=1

# 2. softfp builds of the usual vdpm packages
gh release download Nightly -R Rinnegatamante/vitasdk-packages-softfp -D "$DL/pkgs" --clobber
tar -xzf "$DL/pkgs/include.tar.gz" -C "$SDK" --strip-components=3
tar -xzf "$DL/pkgs/lib.tar.gz" -C "$SDK" --strip-components=3

# 3. current Vita headers and syscall stubs
LATEST=$(gh release list -R vitasdk/autobuilds --limit 20 --json tagName -q '[.[] | select(.tagName | startswith("master-win"))][0].tagName')
gh release download "$LATEST" -R vitasdk/autobuilds -p "*.tar.bz2" -D "$DL" --clobber
NEW_TAR=$(ls -t "$DL"/vitasdk-x86_64-w64-mingw32-*.tar.bz2 | head -1)
tar -xjf "$NEW_TAR" -C "$DL/new" --strip-components=1 --wildcards \
  "*/arm-vita-eabi/include/psp2*" "*/arm-vita-eabi/include/vitasdk*" \
  "*/arm-vita-eabi/lib/*_stub.a" "*/arm-vita-eabi/lib/*_stub_weak.a"
cp -r "$DL/new/arm-vita-eabi/include/." "$SDK/arm-vita-eabi/include/"
cp "$DL"/new/arm-vita-eabi/lib/*.a "$SDK/arm-vita-eabi/lib/"

echo "SDK installed to $SDK. Next: bash scripts/setup-deps.sh"
