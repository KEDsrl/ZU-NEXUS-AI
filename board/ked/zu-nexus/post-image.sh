#!/bin/sh
set -e
BOARD_DIR=$(dirname "$0")

# extlinux.conf nella partizione di boot (U-Boot distro boot)
cp -f "$BOARD_DIR/extlinux.conf" "$BINARIES_DIR/extlinux.conf"

# BOOT.BIN: generato automaticamente se bootgen e gli ingredienti ci sono
if ! "$BOARD_DIR/mk-bootbin.sh" "$BINARIES_DIR"; then
    echo "ATTENZIONE: BOOT.BIN non generato automaticamente."
    echo "  Generarlo con: ./build.sh bootbin   (o manualmente, vedi README)"
    [ -f "$BINARIES_DIR/BOOT.BIN" ] || touch "$BINARIES_DIR/BOOT.BIN"
fi

support/scripts/genimage.sh -c "$BOARD_DIR/genimage.cfg"
