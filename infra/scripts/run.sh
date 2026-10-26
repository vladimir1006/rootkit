#!/bin/bash
# run.sh — démarre le noyau dans QEMU, selon le rootfs voulu.
#
# Usage : ./infra/scripts/run.sh <mode> [profil] [--pause] [-- options qemu...]
#
# Modes :
#   initramfs   noyau + initramfs.cpio.gz (rootfs en RAM)
#   nfs         rootfs monté par le réseau depuis nfsroot/
#   disk        démarrage BIOS sur disk.img via GRUB
#   gdb         lance GDB et se connecte à la VM en attente
#
# Profils : base (défaut) ou debug — quel noyau démarrer.
#
# Options :
#   --pause     démarrer figé, en attente de GDB (option -S de QEMU)
#   -- ...      tout ce qui suit est passé tel quel à QEMU
#
# Le serveur GDB est toujours ouvert sur le port 1234.
# Quitter QEMU : Ctrl-A puis X.
#
# Hôte uniquement (QEMU, et serveur NFS pour le mode nfs).

set -eu
source "$(dirname "$(realpath "$0")")/common.sh"

PROFILE="base"
PAUSE=""
QEMU_EXTRA=()

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            base|debug) PROFILE="$1" ;;
            --pause|-S) PAUSE="-S" ;;
            --) shift; QEMU_EXTRA=("$@"); return 0 ;;
            *) die "argument inconnu : $1" ;;
        esac
        shift
    done
}

# Noyau du profil demandé, avec avertissement si les sources ont bougé
kernel_or_die() {
    local img; img="$(kernel_image "$PROFILE")"
    need_file "$img" "lance d'abord : ./infra/scripts/build_kernel.sh all $PROFILE"
    warn_if_outdated "$PROFILE"
    echo "$img"
}

# run_qemu "<append>" [options...]
run_qemu() {
    local append="$1"; shift
    need_cmd qemu-system-x86_64
    echo "Profil  : $PROFILE"
    [ -n "$append" ] && echo "cmdline : $append"
    [ -n "$PAUSE" ] && echo "En pause : connecte GDB avec  ./infra/scripts/run.sh gdb"
    echo "Quitter QEMU : Ctrl-A puis X"
    echo
    qemu-system-x86_64 \
        -m "$QEMU_MEM" -smp 1 -nographic \
        -gdb "tcp::$QEMU_GDB_PORT" $PAUSE \
        "$@" "${QEMU_EXTRA[@]+"${QEMU_EXTRA[@]}"}"
}

cmd_initramfs() {
    not_in_docker
    parse_args "$@"
    need_file "$INITRAMFS_IMG" "lance d'abord : ./infra/scripts/build_rootfs.sh initramfs"
    log "Démarrage sur initramfs"
    run_qemu "console=ttyS0 nokaslr" \
        -kernel "$(kernel_or_die)" \
        -initrd "$INITRAMFS_IMG" \
        -append "console=ttyS0 nokaslr"
}

cmd_nfs() {
    not_in_docker
    parse_args "$@"
    need_file "$NFS_ROOT" "lance d'abord : ./infra/scripts/build_rootfs.sh nfsroot"
    if command -v exportfs > /dev/null && ! $SUDO exportfs -s 2>/dev/null | grep -q "^$NFS_ROOT"; then
        echo "Attention : $NFS_ROOT ne semble pas exporté." >&2
        echo "            Lance : ./infra/scripts/build_rootfs.sh nfs-server" >&2
    fi

    # vers=3 et tcp : Debian n'active pas NFS en UDP par défaut
    local append="init=/linuxrc root=/dev/nfs"
    append="$append nfsroot=$NFS_HOST_IP:$NFS_ROOT,vers=3,tcp rw"
    append="$append ip=$NFS_GUEST_IP::10.0.2.1:255.255.255.0"
    append="$append console=ttyS0 raid=noautodetect nokaslr"

    log "Démarrage sur rootfs NFS"
    run_qemu "$append" -kernel "$(kernel_or_die)" -append "$append"
}

cmd_disk() {
    not_in_docker
    parse_args "$@"
    need_file "$DISK_IMG" "lance d'abord : ./infra/scripts/build_disk.sh all"
    # Une image montée sur l'hôte et écrite par la VM = corruption assurée
    if mountpoint -q "$MNT_DIR" 2>/dev/null; then
        die "$MNT_DIR est monté — démonte d'abord : ./infra/scripts/build_disk.sh umount"
    fi

    # Pas de -kernel ici : c'est GRUB, dans l'image, qui charge le noyau.
    # Le profil se choisit donc au moment du ./infra/scripts/build_disk.sh grub.
    log "Démarrage sur image disque"
    run_qemu "" -drive "file=$DISK_IMG,format=raw,if=ide"
}

cmd_gdb() {
    not_in_docker
    parse_args "$@"
    need_cmd gdb
    local vm; vm="$(kernel_vmlinux debug)"
    need_file "$vm" "lance d'abord : ./infra/scripts/build_kernel.sh all debug"
    log "GDB sur le port $QEMU_GDB_PORT"
    echo "Astuce : hbreak start_kernel avant le premier continue"
    gdb -q \
        -ex "set pagination off" \
        -ex "set print pretty on" \
        -ex "target remote :$QEMU_GDB_PORT" \
        "$vm"
}

dispatch "$@"
