#!/usr/bin/env bash
# Builds the Katana ZERO app (build/app/KatanaZero.vpk, build/app/eboot.bin)
# and the YoYo Loader drop-in loader (build/loader/loader2.bin).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/env.sh"
cmake -S "$ROOT/loader" -B "$ROOT/build/app" -DKATANA_APP=1
cmake --build "$ROOT/build/app" -j "$(nproc)"
cmake -S "$ROOT/loader" -B "$ROOT/build/loader" -DLOADER=1
cmake --build "$ROOT/build/loader" -j "$(nproc)"
