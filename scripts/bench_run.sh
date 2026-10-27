#!/usr/bin/env bash
# Runs one command under the shared bench lock (the fork's PS2Recomp/tools/bench_lock.sh): waits for it, takes it,
# runs, and releases it on any exit (end, error, Ctrl-C, kill), so a stopped run never leaves the lock behind.
#   scripts/bench_run.sh <minutes> "<purpose>" <command> [args...]
source "$(dirname "$0")/common.sh"
source "$ROOT/PS2Recomp/tools/bench_lock.sh"
[ -x "$ROOT/build/announce.sh" ] && export PS2X_BENCH_LOCK_ANNOUNCE="$ROOT/build/announce.sh"
minutes="${1:?minutes}"; purpose="${2:?purpose}"; shift 2
[ $# -gt 0 ] || { echo "usage: $0 <minutes> <purpose> <command> [args...]"; exit 1; }

bench_lock_take "JC2-Recomp" "$minutes" "$purpose"
trap bench_lock_release EXIT
trap 'exit 130' INT TERM HUP
"$@"
