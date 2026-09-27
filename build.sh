#!/bin/sh
# ============================================================================
# build.sh - Bootstrap e build Buildroot per KED ZU-NEXUS-AI
#
# Uso (da qualunque directory):
#   /path/ked-external/build.sh [comando]
#
# Comandi:
#   (nessuno)      clona buildroot se assente, applica defconfig, compila
#   menuconfig     apre menuconfig (dopo il defconfig)
#   linux-menuconfig   menuconfig del kernel
#   xsa <file.xsa> con Vitis (xsct) nel PATH: rigenera fsbl.elf/pmufw.elf
#                  in board/.../boot/ dallo XSA (+ estrae il .bit dallo XSA)
#   bootbin        genera BOOT.BIN con bootgen dai file in board/.../boot/
#                  e da bl31.elf/u-boot.elf/dtb in output/images
#   vivado [synth] con Vivado nel PATH: ricrea il progetto FPGA da
#                  vivado/ (tcl + xdc + ip_repo); con "synth" prosegue
#                  fino a bitstream + export XSA (poi: build.sh xsa <xsa>)
#   rt-patch [ver] scarica la patch PREEMPT_RT in patches/linux/
#                  (default: 6.1.33-rt11; base kernel xlnx 2023.2 = 6.1.30,
#                  eventuali hunk in fuzz vanno verificati)
#   distclean      azzera la build dir (defconfig incluso)
#   <target>       qualunque altro target make (es. weston-dirclean, sdk)
#
# Variabili sovrascrivibili da ambiente:
#   BUILDROOT_VERSION  tag/branch buildroot   (default: 2025.02.3)
#   BUILDROOT_DIR      dove clonare/compilare (default: <qui>/../buildroot)
#   OUTPUT_DIR         build out-of-tree      (default: dentro BUILDROOT_DIR)
#   JOBS               parallelismo           (default: nproc)
# ============================================================================
set -eu

# --- posizione dell'external (la directory di questo script) ---------------
EXTERNAL_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEFCONFIG=zu_nexus_ai_defconfig

BUILDROOT_VERSION=${BUILDROOT_VERSION:-2025.02.3}
BUILDROOT_DIR=${BUILDROOT_DIR:-$(dirname "$EXTERNAL_DIR")/buildroot}
OUTPUT_DIR=${OUTPUT_DIR:-}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

log() { printf '\033[1;34m[build]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[build] ERRORE:\033[0m %s\n' "$*" >&2; exit 1; }

# --- prerequisiti host: installazione automatica se possibile ---------------
MISSING=""
for t in git make gcc g++ patch perl python3 rsync bc cpio unzip wget file xz; do
    command -v "$t" >/dev/null 2>&1 || MISSING="$MISSING $t"
done
if [ -n "$MISSING" ]; then
    log "tool host mancanti:$MISSING"
    APT_PKGS="build-essential git patch perl python3 rsync bc cpio unzip wget file xz-utils libncurses-dev libssl-dev"
    if command -v apt-get >/dev/null 2>&1; then
        if [ "$(id -u)" = "0" ]; then SUDO=""; 
        elif command -v sudo >/dev/null 2>&1; then SUDO="sudo";
        else die "servono i permessi di root per installare:$MISSING
  eseguire: apt-get install -y $APT_PKGS"; fi
        log "installo le dipendenze via apt-get (richiede rete e privilegi)"
        $SUDO apt-get update -qq || true
        $SUDO apt-get install -y $APT_PKGS || die "installazione dipendenze fallita"
        # ricontrollo
        for t in $MISSING; do
            command -v "$t" >/dev/null 2>&1 || die "dipendenza ancora mancante dopo l'installazione: $t"
        done
        log "dipendenze installate"
    else
        die "tool mancanti:$MISSING e apt-get non disponibile:
  installarli con il package manager della distribuzione
  (equivalenti di: $APT_PKGS)"
    fi
fi

# --- clone (o riuso) di buildroot -------------------------------------------
if [ ! -d "$BUILDROOT_DIR/.git" ]; then
    log "clono buildroot $BUILDROOT_VERSION in $BUILDROOT_DIR"
    git clone --branch "$BUILDROOT_VERSION" --depth 1 \
        https://gitlab.com/buildroot.org/buildroot.git "$BUILDROOT_DIR"
else
    CUR=$(git -C "$BUILDROOT_DIR" describe --tags --always 2>/dev/null || echo '?')
    log "riuso buildroot esistente in $BUILDROOT_DIR (versione: $CUR)"
    [ "$CUR" = "$BUILDROOT_VERSION" ] || \
        log "NB: versione diversa da $BUILDROOT_VERSION richiesta; per cambiarla: rm -rf $BUILDROOT_DIR"
fi

# --- make wrapper: out-of-tree opzionale ------------------------------------
if [ -n "$OUTPUT_DIR" ]; then
    mkdir -p "$OUTPUT_DIR"
    MAKE="make -C $BUILDROOT_DIR O=$OUTPUT_DIR"
    CFG="$OUTPUT_DIR/.config"
    IMAGES_DIR="$OUTPUT_DIR/images"
else
    MAKE="make -C $BUILDROOT_DIR"
    CFG="$BUILDROOT_DIR/.config"
    IMAGES_DIR="$BUILDROOT_DIR/output/images"
fi

# --- defconfig (solo se .config assente o external cambiato) ----------------
apply_defconfig() {
    log "applico $DEFCONFIG (BR2_EXTERNAL=$EXTERNAL_DIR)"
    $MAKE BR2_EXTERNAL="$EXTERNAL_DIR" "$DEFCONFIG"
}

CMD=${1:-build}
case "$CMD" in
build)
    # riapplica il defconfig se assente o piu' recente della .config
    if [ ! -f "$CFG" ] || [ "$EXTERNAL_DIR/configs/$DEFCONFIG" -nt "$CFG" ]; then
        apply_defconfig
    fi
    # bonifica residui di install a path assoluto nella staging: se un
    # pacchetto (in passato) ha installato in staging/<path build>, la
    # directory residua fa fallire il check di TUTTI i pacchetti successivi
    BASE_PATH=$(dirname "$CFG")
    for sr in "$BASE_PATH"/host/*/sysroot; do
        [ -d "$sr" ] || continue
        leftover="${sr}${BASE_PATH}"
        if [ -d "$leftover" ]; then
            log "rimuovo residuo di install assoluta nella staging: $leftover"
            rm -rf "${sr}/$(echo "$BASE_PATH" | cut -d/ -f2)"
        fi
    done
    log "compilo con $JOBS job (la prima build scarica toolchain+kernel+qt6: ore, non minuti)"
    ST_FILE="$EXTERNAL_DIR/.build-status"
    # set +e nella subshell: altrimenti con set -e il make fallito esce
    # prima di scrivere lo status e l'errore non viene riportato
    ( set +e; $MAKE -j"$JOBS" 2>&1; echo "$?" > "$ST_FILE" ) | tee "$EXTERNAL_DIR/last-build.log"
    ST=$(cat "$ST_FILE"); rm -f "$ST_FILE"
    [ "$ST" = "0" ] || die "build fallita (exit $ST), dettagli in last-build.log"
    log "fatto. Immagini in: $IMAGES_DIR"
    log "BOOT.BIN va generato con bootgen (vedi README.md) e copiato li',"
    log "poi rilanciare '$0 build' per includerlo nella sdcard.img."
    ;;
menuconfig)
    [ -f "$CFG" ] || apply_defconfig
    $MAKE menuconfig
    log "per salvare le modifiche nel defconfig dell'external:"
    log "  $MAKE savedefconfig BR2_DEFCONFIG=$EXTERNAL_DIR/configs/$DEFCONFIG"
    ;;
linux-menuconfig)
    [ -f "$CFG" ] || apply_defconfig
    $MAKE linux-menuconfig
    log "NB: modifiche persistenti vanno nel fragment board/ked/zu-nexus/linux.fragment"
    ;;
defconfig)   apply_defconfig ;;
distclean)   $MAKE distclean ;;
xsa)
    XSA=${2:?uso: $0 xsa /percorso/system.xsa}
    [ -f "$XSA" ] || die "XSA non trovato: $XSA"
    command -v xsct >/dev/null 2>&1 ||         die "xsct non nel PATH: sorgente l'ambiente Vitis 2023.2 (settings64.sh)
  In alternativa usare i binari gia' presenti in board/ked/zu-nexus/boot/."
    BOOT_DIR=$EXTERNAL_DIR/board/ked/zu-nexus/boot
    log "build FSBL+PMUFW da $XSA (Vitis XSCT)"
    xsct "$EXTERNAL_DIR/fsbl/build_fsbl.tcl" "$XSA"
    FSBL=$EXTERNAL_DIR/fsbl/fsbl_ws/ked_fsbl/Debug/ked_fsbl.elf
    [ -f "$FSBL" ] || die "FSBL non prodotto (vedi output XSCT)"
    cp -f "$FSBL" "$BOOT_DIR/fsbl.elf"
    PMUFW=$(find "$EXTERNAL_DIR/fsbl/fsbl_ws" -name 'pmufw.elf' | head -1)
    [ -n "$PMUFW" ] && cp -f "$PMUFW" "$BOOT_DIR/pmufw.elf"         || log "ATTENZIONE: pmufw.elf non trovato nel workspace, resta quello in boot/"
    # il .bit sta dentro lo XSA (e' uno zip)
    TMP=$(mktemp -d)
    if unzip -o -q "$XSA" -d "$TMP" && BIT=$(find "$TMP" -name '*.bit' | head -1) && [ -n "$BIT" ]; then
        rm -f "$BOOT_DIR"/*.bit
        cp -f "$BIT" "$BOOT_DIR/$(basename "$BIT")"
        log "bitstream estratto: $(basename "$BIT")"
    fi
    rm -rf "$TMP"
    log "boot/ aggiornata: ora '$0 bootbin' (o una build completa) genera BOOT.BIN"
    ;;
bootbin)
    "$EXTERNAL_DIR/board/ked/zu-nexus/mk-bootbin.sh" "$IMAGES_DIR"
    ;;
vivado)
    command -v vivado >/dev/null 2>&1 || \
        die "vivado non nel PATH: sorgente l'ambiente (settings64.sh di Vivado 2023.2)"
    VDIR=$EXTERNAL_DIR/vivado
    [ -f "$VDIR/ZCU-DM-X1-R3.tcl" ] || die "sorgenti FPGA non trovati in $VDIR"
    if [ -d "$VDIR/ZCU-DM-X1-R3" ]; then
        die "progetto gia' presente in $VDIR/ZCU-DM-X1-R3: rimuoverlo per ricrearlo
  (rm -rf $VDIR/ZCU-DM-X1-R3)"
    fi
    WRAP=$(mktemp --suffix=.tcl)
    {
        echo "cd $VDIR"
        echo "source $VDIR/ZCU-DM-X1-R3.tcl"
        if [ "${2:-}" = "synth" ]; then
            cat << 'TCLEOF'
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
open_run impl_1
file mkdir out
write_hw_platform -fixed -include_bit -force out/system.xsa
puts ">>> XSA esportato in out/system.xsa"
TCLEOF
        fi
    } > "$WRAP"
    log "ricreo il progetto Vivado in $VDIR (log: vivado.log/jou)"
    ( cd "$VDIR" && vivado -mode batch -nojournal -log vivado.log -source "$WRAP" )
    RC=$?
    rm -f "$WRAP"
    [ $RC -eq 0 ] || die "vivado terminato con errore (vedi $VDIR/vivado.log)"
    if [ "${2:-}" = "synth" ]; then
        log "bitstream + XSA pronti: ora '$0 xsa $VDIR/out/system.xsa'"
        log "rigenera FSBL/PMUFW e aggiorna il .bit nella cartella boot/"
    else
        log "progetto creato: aprirlo con 'vivado $VDIR/ZCU-DM-X1-R3/ZCU-DM-X1-R3.xpr'"
        log "oppure sintetizzare direttamente con: $0 vivado synth (dopo rm del progetto)"
    fi
    ;;
rt-patch)
    RTVER=${2:-6.1.33-rt11}
    DEST=$EXTERNAL_DIR/board/ked/zu-nexus/patches/linux
    URL="https://cdn.kernel.org/pub/linux/kernel/projects/rt/6.1/older/patch-$RTVER.patch.xz"
    log "scarico patch RT $RTVER"
    wget -q -O "$DEST/patch-$RTVER.patch.xz" "$URL" ||         wget -q -O "$DEST/patch-$RTVER.patch.xz" "${URL%older/*}patch-$RTVER.patch.xz" ||         die "download fallito: verificare la versione su kernel.org/pub/linux/kernel/projects/rt/6.1/"
    xz -d -f "$DEST/patch-$RTVER.patch.xz"
    mv "$DEST/patch-$RTVER.patch" "$DEST/0001-preempt-rt-$RTVER.patch"
    log "patch in $DEST/0001-preempt-rt-$RTVER.patch"
    log "NB: base xlnx 2023.2 = 6.1.30; se l'applicazione fallisce (make linux-dirclean"
    log "    per riprovare) provare una versione RT piu' vicina o risolvere gli hunk."
    log "Il fragment linux-rt.fragment attiva CONFIG_PREEMPT_RT alla prossima build:"
    log "    $0 linux-dirclean && $0"
    ;;
*)
    [ -f "$CFG" ] || apply_defconfig
    $MAKE "$CMD"
    ;;
esac
