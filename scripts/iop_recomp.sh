#!/usr/bin/env bash
# Recompiles JC2's IOP modules to native C++ (generated/iop/*_iop.cpp) with the fork's tools/iop_recomp.py, from the
# images a run saved with PS2X_IOP_DUMP_MODULES=build/iop_modules (each relocated at its base; the native code is
# used only for a module with the same file hash at the same base). PS2X_IOP_NATIVE=1 runs them.
#   scripts/iop_recomp.sh [MODULE...]    default: all 13 modules JC2 loads (none leaves code to the interpreter)
# The tool also takes the functions a module only reaches through a pointer (M2SOUND: 9 functions -> 89).
source "$(dirname "$0")/common.sh"
DUMPS="$ROOT/build/iop_modules"
OUT="$GENERATED/iop"
modules=("$@")
[ ${#modules[@]} -gt 0 ] || modules=(M2IOPSYS M2SOUND M2IMODE MODMIDI MODHSYN LIBSD SIO2MAN MTAPMAN MCMAN MCSERV PADMAN UAMAN IMODEMAN)
mkdir -p "$OUT"
for m in "${modules[@]}"; do
    json=$(ls "$DUMPS/$m".IRX*.json 2>/dev/null | head -1)
    [ -n "$json" ] || { echo "no dump for $m in $DUMPS (run with PS2X_IOP_DUMP_MODULES=$DUMPS)"; exit 1; }
    # Windows Python takes D:/ paths, not MSYS /d/ ones.
    native() { command -v cygpath > /dev/null && cygpath -m "$1" || echo "$1"; }
    python "$(native "$ROOT/PS2Recomp/tools/iop_recomp.py")" "$(native "$json")" --out "$(native "$OUT")" |
        grep -E '^[^ ]|functions,'
done
