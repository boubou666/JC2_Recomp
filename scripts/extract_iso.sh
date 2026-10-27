#!/usr/bin/env bash
# Usage: scripts/extract_iso.sh <path-to-Jade-Cocoon-2-USA.iso>
# Extracts the disc into game/ (the runtime mounts cdrom0: on the ELF's directory).
source "$(dirname "$0")/common.sh"
ISO="${1:-}"
[ -f "$ISO" ] || { echo "usage: $0 <iso>"; exit 1; }
mkdir -p "$GAME_DIR"
bsdtar -xf "$ISO" -C "$GAME_DIR"
chmod -R u+w "$GAME_DIR"
[ -f "$ELF" ] || { echo "ELF not found after extraction: $ELF (is this the NTSC-U SLUS-20309 disc?)"; exit 1; }
echo "Extracted to $GAME_DIR"
