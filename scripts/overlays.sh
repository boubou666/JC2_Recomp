#!/usr/bin/env bash
# Recompiles JC2's code overlays (CodeWarrior 'MWo3' modules in CDDATA.000, loaded at run time into fixed slots:
# 0x940000, 0x963000, ...) as PS2Recomp overlay modules, into generated/overlays/<module>/. At run time the module
# whose first 64 bytes (its header, with its file name) match guest memory at its load address is the one called.
#   scripts/overlays.sh [MODULE...]     default: every module on the disc (1383; ~6.5M instructions)
# Stages per module: tools/find_overlays.py (once) -> VP2S_Recomp's tools/wrap_elf.py (ELF at the load address)
# -> tools/overlay_functions.py (function map, no Ghidra) -> ps2_recomp in module mode.
source "$(dirname "$0")/common.sh"
export PATH="/c/msys64/ucrt64/bin:$PATH"
native() { command -v cygpath > /dev/null && cygpath -m "$1" || echo "$1"; }
DISC="$ROOT/build/overlays_disc"
WORK="$ROOT/build/overlay_work"
OUT="$GENERATED/overlays"
WRAP="${JC2_WRAP_ELF:-D:/VP2S_Recomp/tools/wrap_elf.py}"
mkdir -p "$WORK" "$OUT"
[ -n "$(ls "$DISC"/*.bin 2>/dev/null | head -1)" ] ||
    python -I "$(native "$ROOT/tools/find_overlays.py")" "$(native "$GAME_DIR/CDDATA.000")" "$(native "$DISC")" | tail -1

modules=("$@")
if [ ${#modules[@]} -eq 0 ]; then
    # Every module, $JOBS at a time (each run is one process per module: wrap, map, ps2_recomp).
    for f in "$DISC"/*.bin; do basename "$f" .bin; done | xargs -P "$JOBS" -n 16 bash "$0" ||
        { echo "some overlay modules failed (see $WORK/*.log)"; exit 1; }
    echo "$(ls -d "$OUT"/*/ | wc -l) overlay modules in $OUT"
    exit 0
fi
for stem in "${modules[@]}"; do
    bin=$(ls "$DISC/$stem"*.bin 2>/dev/null | head -1)
    [ -n "$bin" ] || { echo "no overlay $stem in $DISC"; exit 1; }
    stem=$(basename "$bin" .bin)
    name="ovl_$(echo "$stem" | sed 's/_[0-9a-f]\{6\}$//; s/[^A-Za-z0-9_]/_/g')"
    load="0x${stem##*_}"
    # A module that cannot be wrapped or mapped is reported and skipped; the others go on.
    python -I "$WRAP" "$(native "$bin")" "$(native "$WORK/$name.elf")" --flags-from "$(native "$ELF")"         > "$WORK/$name.log" 2>&1 || { echo "wrap failed for $name (see $WORK/$name.log)"; continue; }
    python -I "$(native "$ROOT/tools/overlay_functions.py")" "$(native "$bin")" "$(native "$WORK/$name.csv")"         >> "$WORK/$name.log" 2>&1 || { echo "function map failed for $name (see $WORK/$name.log)"; continue; }
    cat > "$WORK/$name.toml" <<TOML
[general]
input = "$(native "$WORK/$name.elf")"
ghidra_output = "$(native "$WORK/$name.csv")"
output = "$(native "$OUT/$name")/"
single_file_output = false
patch_syscalls = false
patch_cop0 = true
patch_cache = true
module_name = "$name"
module_load = "$load"
module_guard_size = 64
stubs = []
skip = []
TOML
    mkdir -p "$OUT/$name"
    "$TOOLS_BUILD/ps2xRecomp/ps2_recomp$EXE" "$(native "$WORK/$name.toml")" >> "$WORK/$name.log" 2>&1 ||
        { echo "ps2_recomp failed for $name (see $WORK/$name.log)"; continue; }
    # One include folder for every module's headers (names carry the module prefix): an include directory per
    # module made the compiler's command line too long for Windows.
    mkdir -p "$OUT/include"
    cp "$OUT/$name/${name}_stubs.h" "$OUT/$name/${name}_functions.h" "$OUT/include/" 2>/dev/null
done
echo "$(ls -d "$OUT"/*/ | wc -l) overlay modules in $OUT"
