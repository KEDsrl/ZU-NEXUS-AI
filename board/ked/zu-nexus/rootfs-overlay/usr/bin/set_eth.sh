#!/bin/sh
# ============================================================================
# set_eth.sh - configura la rete cablata, SUBITO e in modo persistente
#
# Uso:
#   set_eth.sh dhcp                          torna al DHCP (default)
#   set_eth.sh <ip>[/prefix] [gw] [dns]      IP statico (prefix default /24)
#
# Esempi:
#   set_eth.sh 192.168.1.50
#   set_eth.sh 192.168.1.50/24 192.168.1.1 8.8.8.8
#   set_eth.sh dhcp
#
# Scrive /etc/systemd/network/20-wired.network (persistente al riavvio)
# e riapplica subito via networkctl.
# ============================================================================
set -eu

CFG=/etc/systemd/network/20-wired.network
MODE=${1:?uso: $0 dhcp | <ip>[/prefix] [gateway] [dns]}

# interfaccia cablata attuale (end0 con il predictable naming, eth0 legacy)
IFACE=$(ls /sys/class/net/ | grep -E '^(end|eth)' | head -1)
[ -n "$IFACE" ] || { echo "nessuna interfaccia end*/eth* trovata"; exit 1; }

case "$MODE" in
dhcp|DHCP)
    cat > "$CFG" << NET
[Match]
Name=end* eth*

[Network]
DHCP=yes
NET
    echo "modo DHCP scritto in $CFG"
    ;;
*)
    # valida l'IP (con o senza /prefix)
    IP=$MODE
    echo "$IP" | grep -qE '^([0-9]{1,3}\.){3}[0-9]{1,3}(/[0-9]{1,2})?$' \
        || { echo "indirizzo non valido: $IP"; exit 1; }
    case "$IP" in */*) ;; *) IP="$IP/24" ;; esac
    GW=${2:-}
    DNS=${3:-}
    {
        printf '[Match]\nName=end* eth*\n\n[Network]\nAddress=%s\n' "$IP"
        [ -n "$GW" ]  && printf 'Gateway=%s\n' "$GW"
        [ -n "$DNS" ] && printf 'DNS=%s\n' "$DNS"
    } > "$CFG"
    echo "IP statico $IP${GW:+ gw $GW}${DNS:+ dns $DNS} scritto in $CFG"
    ;;
esac

# applica subito
networkctl reload
networkctl reconfigure "$IFACE"
sleep 1
networkctl status "$IFACE" --no-pager 2>/dev/null | grep -E 'Address|Gateway|State' || \
    ip -4 addr show "$IFACE"
echo "OK: configurazione attiva ora e persistente al riavvio."
