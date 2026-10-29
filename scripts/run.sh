#!/usr/bin/env bash
# Launches the recompiled game. Extra args are passed to ps2EntryRunner.
# The SPU2 runs on the wall clock (native path, as TMB): on the default path JC2's output was white noise
# (boot 20 A/B, docs/RECOMP_NOTES.md). Set PS2X_SPU2_WALLCLOCK=0 to compare.
# Settings that must reach the game go through the environment of this script: export them (a bare VAR=x in front
# of a command inside another bash -c did not reach the runner in boot 31).
source "$(dirname "$0")/common.sh"
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner$EXE"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/build_runtime.sh first"; exit 1; }
export PS2X_SPU2_WALLCLOCK="${PS2X_SPU2_WALLCLOCK:-1}"
# The IOP modules recompiled by scripts/iop_recomp.sh (generated/iop) run native. Boot 33: same SPU2 counters and
# sound as interpreted (PS2X_IOP_NATIVE_CHECK only checks TMB code, so it proves nothing here). =0 to compare.
export PS2X_IOP_NATIVE="${PS2X_IOP_NATIVE:-1}"
# The IOP's time follows the PC clock, not the EE cycles the game ran (native layer 2, TMB's run_native.sh). JC2's
# "AutoDMA underruns" (iop_spu2) stay at 230 per 75 s with or without it and leave no silent gap in the output
# (boots 33 and 37): they count the stream idle, not audible dropouts.
export PS2X_IOP_WALLCLOCK="${PS2X_IOP_WALLCLOCK:-1}"
# The native GS renderer (GSNativeBackend, OpenGL): no GS emulation. Boot 38: intro movie, title and the 3D demo
# draw. PS2X_GS_RENDERER=cpu for the CPU GS reference.
export PS2X_GS_RENDERER="${PS2X_GS_RENDERER:-native}"
exec "$BIN" "$ELF" "$@"
