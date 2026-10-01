#!/usr/bin/env bash
# Profiles a list of rooms (test saves in work/testsaves/<room>): fps + top events per room -> work/sweep/<room>.txt
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/work/sweep"
for r in $(cat "${1:-$ROOT/work/sweep_rooms.txt}"); do
	out="$ROOT/work/sweep/$r.txt"
	python "$ROOT/scripts/bench.py" --label "sweep_$r" --no-install --save "$ROOT/work/testsaves/$r" --room "room_$r" --seconds 8 --profile > "$out" 2>&1
	python - "$out" <<'PY'
import sys; sys.path.insert(0, 'scripts'); import bench
log = bench.read_log().splitlines()
idx = [i for i, l in enumerate(log) if '[PROF]' in l]
block = log[idx[-2]:idx[-2] + 70] if len(idx) >= 2 else []
keep = [l for l in block if '[PROF]' in l or 'STAGE' in l or 'EVENT' in l or 'VGLMEM' in l][:18]
open(sys.argv[1], 'a').write('\n'.join(keep) + '\n')
PY
	tail -n +1 "$out" | grep -m1 "fps mean"
done
