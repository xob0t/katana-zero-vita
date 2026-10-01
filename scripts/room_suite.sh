#!/usr/bin/env bash
# Benchmarks a set of heavy rooms by installing test saves (work/testsaves/<room>) before each run.
# Usage: scripts/room_suite.sh <label> [bench.py args...]; ROOMS env overrides the room list.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LABEL=$1; shift
ROOMS=${ROOMS:-"bunker2_3 haunted_alien2 dragon_3 factory2_4 mansion_bikechase mansion_3"}
for r in $ROOMS; do
	python "$ROOT/scripts/bench.py" --label "${LABEL}_$r" --no-install --save "$ROOT/work/testsaves/$r" --room "room_$r" "$@" 2>&1 | tail -1
done
