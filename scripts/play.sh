#!/usr/bin/env bash
# A play session for a person at the keyboard or pad: the game as run.sh starts it, a screenshot every
# PLAY_SHOT seconds (default 10) in build/play/<time>/ and the log beside them, under the bench lock (one slot).
#   scripts/play.sh [minutes, default 90]
# Keyboard: arrows/WASD move, X or Space = Cross, C or Escape = Circle, Z = Square, V = Triangle, Q/E = L1/R1,
# Enter = Start, Tab = Select. A plugged-in pad works too.
source "$(dirname "$0")/common.sh"
export PATH="/c/msys64/ucrt64/bin:$PATH"
minutes="${1:-90}"
dir="$ROOT/build/play/$(date +%Y%m%d-%H%M)"
mkdir -p "$dir"
echo "screenshots and log: $dir"
exec bash "$ROOT/scripts/bench_run.sh" "$minutes" "JC2 play session (the owner plays, screenshots every ${PLAY_SHOT:-10} s)" \
    bash -c "export JC2_FPS=1 PS2X_CAPTURE_DIR='$dir' PS2X_CAPTURE_INTERVAL='${PLAY_SHOT:-10}'; bash '$ROOT/scripts/run.sh' > '$dir/game.log' 2>&1"
