#!/bin/sh
# post-build: ritocchi al rootfs prima della creazione delle immagini
set -e

# SSH: consenti il login root con password (immagine di sviluppo/bring-up;
# per la produzione tornare a prohibit-password + chiavi)
SSHD_CFG="$TARGET_DIR/etc/ssh/sshd_config"
if [ -f "$SSHD_CFG" ]; then
    if grep -qE '^#?PermitRootLogin' "$SSHD_CFG"; then
        sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin yes/' "$SSHD_CFG"
    else
        echo 'PermitRootLogin yes' >> "$SSHD_CFG"
    fi
fi

# --- fuso orario ------------------------------------------------------------
# /etc/localtime lo crea il pacchetto tzdata da BR2_TARGET_LOCALTIME. Se manca
# (defconfig non riapplicato, tzdata non compilato) la board resta in UTC e
# l'orologio dell'app e' 1-2 ore indietro: meglio accorgersene in build.
if [ ! -e "$TARGET_DIR/etc/localtime" ]; then
    echo "ATTENZIONE: /etc/localtime assente nel rootfs -> la board sara' in UTC."
    echo "  Verificare BR2_TARGET_TZ_INFO=y e BR2_TARGET_LOCALTIME in output/.config"
    echo "  (./build.sh defconfig && ./build.sh)"
else
    echo "post-build: fuso orario $(readlink "$TARGET_DIR/etc/localtime" | sed 's|.*/zoneinfo/||')"
fi
