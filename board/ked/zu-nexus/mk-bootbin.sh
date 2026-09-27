#!/bin/sh
# mk-bootbin.sh <images_dir> - genera BOOT.BIN con bootgen
# Ingredienti: boot/fsbl.elf boot/pmufw.elf boot/*.bit (questa cartella),
#              bl31.elf u-boot.elf <dtb> (da images_dir).
set -eu
BOARD_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BOOT_DIR=$BOARD_DIR/boot
IMAGES=${1:?uso: mk-bootbin.sh <images_dir>}
IMAGES=$(CDPATH= cd -- "$IMAGES" && pwd)
DTB=zynqmp-ked-zcu-revA.dtb

# bootgen: preferisce il binario nella cartella boot, poi quello nel PATH
if [ -x "$BOOT_DIR/bootgen" ] && "$BOOT_DIR/bootgen" -help >/dev/null 2>&1; then
    BOOTGEN=$BOOT_DIR/bootgen
elif command -v bootgen >/dev/null 2>&1; then
    BOOTGEN=$(command -v bootgen)
else
    echo "mk-bootbin: bootgen non disponibile (ne' in $BOOT_DIR ne' nel PATH)"; exit 1
fi

BIT=$(ls "$BOOT_DIR"/*.bit 2>/dev/null | head -1) || true
for f in "$BOOT_DIR/fsbl.elf" "$BOOT_DIR/pmufw.elf" "$BIT" "$IMAGES/$DTB"; do
    [ -n "$f" ] && [ -f "$f" ] || { echo "mk-bootbin: manca $f"; exit 1; }
done

# u-boot, due strade:
#  A) u-boot.bin (dtb appeso, OF_SEPARATE): nessuna partizione dtb separata,
#     load/entry = CONFIG_TEXT_BASE letto dalla .config di U-Boot
#  B) fallback: ELF nudo + dtb caricato a 0x100000 (flusso PetaLinux)
UBOOT_LINE="" ; DTB_LINE=""
UBOOT_CFG=$(ls "$IMAGES"/../build/uboot-*/.config 2>/dev/null | head -1)
TEXT_BASE=""
[ -n "$UBOOT_CFG" ] && TEXT_BASE=$(grep -E '^CONFIG(_SYS)?_TEXT_BASE=' "$UBOOT_CFG" | head -1 | cut -d= -f2)
if [ -f "$IMAGES/u-boot.bin" ] && [ -n "$TEXT_BASE" ]; then
    echo "mk-bootbin: u-boot.bin con dtb appeso, TEXT_BASE=$TEXT_BASE (niente partizione dtb)"
    UBOOT_LINE="[destination_cpu=a53-0, exception_level=el-2, load=$TEXT_BASE, startup=$TEXT_BASE] $IMAGES/u-boot.bin"
else
    if [ -f "$IMAGES/u-boot.elf" ]; then UBOOT=$IMAGES/u-boot.elf
    elif [ -f "$IMAGES/u-boot" ]; then UBOOT=$IMAGES/u-boot
    else echo "mk-bootbin: manca u-boot in $IMAGES"; exit 1; fi
    UBOOT_LINE="[destination_cpu=a53-0, exception_level=el-2] $UBOOT"
    DTB_LINE="[destination_cpu=a53-0, load=0x00100000] $IMAGES/$DTB"
fi

# bl31.elf: esportato in images/ da Buildroot
# (BR2_TARGET_ARM_TRUSTED_FIRMWARE_IMAGES="bl31/bl31.elf" nel defconfig)
if [ ! -f "$IMAGES/bl31.elf" ]; then
    echo "mk-bootbin: manca $IMAGES/bl31.elf"
    echo "  generarlo con: ./build.sh arm-trusted-firmware-rebuild"
    exit 1
fi
BL31_LINE="[destination_cpu=a53-0, exception_level=el-3, trustzone] $IMAGES/bl31.elf"

BIF=$(mktemp)
cat > "$BIF" << BIFEOF
the_ROM_image:
{
    [bootloader, destination_cpu=a53-0] $BOOT_DIR/fsbl.elf
    [destination_cpu=pmu] $BOOT_DIR/pmufw.elf
    [destination_device=pl] $BIT
    $BL31_LINE
    $DTB_LINE
    $UBOOT_LINE
}
BIFEOF
if ! "$BOOTGEN" -arch zynqmp -image "$BIF" -o "$IMAGES/BOOT.BIN" -w on; then
    echo "mk-bootbin: bootgen fallito, BIF generato:"
    echo "----------------------------------------"
    cat "$BIF"
    echo "----------------------------------------"
    rm -f "$BIF"
    exit 1
fi
rm -f "$BIF"
echo "mk-bootbin: BOOT.BIN generato in $IMAGES (bit: $(basename "$BIT"))"
