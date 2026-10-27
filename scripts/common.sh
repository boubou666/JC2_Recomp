# Sourced by the other scripts. Resolves the repo root and tool paths.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$HOME/.local/share/mise/shims:$PATH"

GAME_DIR="$ROOT/game"
ELF="$GAME_DIR/SLUS_203.09"
CONFIG="$ROOT/recomp/jc2_slus20309.toml"
GHIDRA_CSV="$ROOT/recomp/ghidra/jc2_functions.csv"
GENERATED="$ROOT/generated"
TOOLS_BUILD="$ROOT/build/tools"
GAME_BUILD="$ROOT/build/game"
JOBS="${JOBS:-$(nproc)}"

# Native executables carry .exe on Windows (MSYS2).
EXE=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; esac
