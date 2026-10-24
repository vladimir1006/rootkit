#!/bin/bash
# common.sh — variables et fonctions partagées par les scripts du workspace.
#
# À sourcer, pas à exécuter :
#     source "$(dirname "$0")/common.sh"
#
# Lancé directement, il affiche l'état du workspace.
#
# Toutes les variables sont surchargeables par l'environnement :
#     KERNEL_VERSION=6.6.1 ./scripts/build_kernel.sh all

# Garde-fou : ne charger qu'une fois
[ -n "${COMMON_SH:-}" ] && return 0
COMMON_SH=1

# ---------------------------------------------------------------- chemins ---

# WORKSPACE = le parent de scripts/, calculé depuis CE fichier
# (BASH_SOURCE et non $0 : dans un fichier sourcé, $0 est le script appelant)
_COMMON_DIR="$(cd "$(dirname "$(realpath "${BASH_SOURCE[0]}")")" && pwd)"
WORKSPACE="${WORKSPACE:-$(dirname "$_COMMON_DIR")}"

KERNEL_VERSION="${KERNEL_VERSION:-6.1.188}"
KERNEL_URL="${KERNEL_URL:-https://cdn.kernel.org/pub/linux/kernel/v${KERNEL_VERSION%%.*}.x/linux-${KERNEL_VERSION}.tar.xz}"
KERNEL_SRC="${KERNEL_SRC:-$WORKSPACE/linux-${KERNEL_VERSION}}"

# Un dossier de build par profil (make O=...) : les deux noyaux coexistent
# et passer de l'un à l'autre ne recompile rien.
KERNEL_BUILD_BASE="${KERNEL_BUILD_BASE:-$WORKSPACE/build}"
KERNEL_BUILD_DEBUG="${KERNEL_BUILD_DEBUG:-$WORKSPACE/build-debug}"

BUSYBOX_REPO="${BUSYBOX_REPO:-https://git.busybox.net/busybox}"
BUSYBOX_SRC="${BUSYBOX_SRC:-$WORKSPACE/busybox}"
BUSYBOX_INSTALL="${BUSYBOX_INSTALL:-$BUSYBOX_SRC/_install}"

INIT_LOOP_DIR="${INIT_LOOP_DIR:-$WORKSPACE/my_init_loop}"
INITRAMFS_BUILD="${INITRAMFS_BUILD:-$WORKSPACE/initramfs}"
INITRAMFS_IMG="${INITRAMFS_IMG:-$WORKSPACE/initramfs.cpio.gz}"

NFS_ROOT="${NFS_ROOT:-$WORKSPACE/nfsroot}"
NFS_HOST_IP="${NFS_HOST_IP:-10.0.2.2}"     # l'hôte, vu depuis la VM (QEMU user)
NFS_GUEST_IP="${NFS_GUEST_IP:-10.0.2.15}"  # la VM

DISK_IMG="${DISK_IMG:-$WORKSPACE/disk.img}"
DISK_SIZE="${DISK_SIZE:-1G}" 
MNT_DIR="${MNT_DIR:-/tmp/my-rootfs}"
ALPINE_IMAGE="${ALPINE_IMAGE:-alpine}"
ROOT_PASSWORD="${ROOT_PASSWORD:-toor}"   # VM de labo : mot de passe volontairement trivial

QEMU_MEM="${QEMU_MEM:-512M}"
QEMU_GDB_PORT="${QEMU_GDB_PORT:-1234}"

# ------------------------------------------------------------ environnement -

JOBS="${JOBS:-$(nproc)}"

# En root (cas de Docker), sudo est inutile et souvent absent
if [ "$(id -u)" -eq 0 ]; then SUDO=""; else SUDO="sudo"; fi

if [ -f /.dockerenv ]; then IN_DOCKER="oui"; else IN_DOCKER="non"; fi

# --------------------------------------------------------------- fonctions --

log() {
    echo
    echo "================================================"
    echo " $*"
    echo "================================================"
}

die() {
    echo "ERREUR : $*" >&2
    exit 1
}

# need_cmd parted mkfs.ext4 — vérifie les binaires AVANT de commencer
need_cmd() {
    local missing=()
    for c in "$@"; do
        command -v "$c" > /dev/null 2>&1 || missing+=("$c")
    done
    [ ${#missing[@]} -eq 0 ] || die "commande(s) absente(s) : ${missing[*]}"
}

# need_file /chemin "lance d'abord : build_kernel.sh all"
need_file() {
    [ -e "$1" ] || die "$1 est absent — ${2:-étape précédente non faite}"
}

# kernel_build [base|debug] — dossier de build du profil
kernel_build() {
    case "${1:-base}" in
        debug) echo "$KERNEL_BUILD_DEBUG" ;;
        base|"") echo "$KERNEL_BUILD_BASE" ;;
        *) die "profil inconnu : $1 (attendu : base ou debug)" ;;
    esac
}

kernel_image()   { echo "$(kernel_build "${1:-base}")/arch/x86/boot/bzImage"; }
kernel_vmlinux() { echo "$(kernel_build "${1:-base}")/vmlinux"; }

# kernel_outdated [profil] — vrai si une source est plus récente que le bzImage
kernel_outdated() {
    local img; img="$(kernel_image "${1:-base}")"
    [ -f "$img" ] || return 0
    [ -n "$(find "$KERNEL_SRC" \( -name '*.c' -o -name '*.h' -o -name '*.S' \) \
        -newer "$img" -print -quit 2>/dev/null)" ]
}

# warn_if_outdated [profil] — prévient sans bloquer
warn_if_outdated() {
    local p="${1:-base}"
    if kernel_outdated "$p"; then
        echo "Attention : des sources ont changé depuis la dernière compilation." >&2
        echo "            Lance : ./scripts/build_kernel.sh compile $p" >&2
    fi
}

not_in_docker() {
    [ "$IN_DOCKER" = "non" ] || die "à lancer sur l'hôte, pas dans Docker"
}

# confirm "Écraser disk.img ?" — refuse par défaut, et hors terminal
confirm() {
    [ -t 0 ] || die "$1 (refusé : pas de terminal, réponse impossible)"
    local reply
    read -r -p "$1 [o/N] " reply
    case "$reply" in
        [oOyY]) return 0 ;;
        *) die "annulé" ;;
    esac
}

# dispatch "$@" — route vers cmd_<sous-commande>, tirets remplacés par _
# Sans argument : usage. Sous-commande inconnue : usage + code 1.
dispatch() {
    local cmd="${1:-}"
    [ $# -gt 0 ] && shift
    local fn="cmd_${cmd//-/_}"
    if [ -z "$cmd" ] || ! declare -F "$fn" > /dev/null; then
        [ -n "$cmd" ] && echo "Sous-commande inconnue : $cmd" >&2
        usage_from_header
        return 1
    fi
    "$fn" "$@"
}

# Affiche l'en-tête de commentaires du script appelant comme aide
usage_from_header() {
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' \
        "${BASH_SOURCE[-1]}"
}

# ------------------------------------------------------------- diagnostic ---

_state() { [ -e "$1" ] && echo "présent" || echo "absent"; }

common_status() {
    printf '%-14s = %s\n' WORKSPACE "$WORKSPACE"
    printf '%-14s = %s  [%s]\n' KERNEL_SRC "$KERNEL_SRC" "$(_state "$KERNEL_SRC")"
    printf '%-14s = %s  [%s]\n' "BUILD base" "$KERNEL_BUILD_BASE" "$(_state "$(kernel_image base)")"
    printf '%-14s = %s  [%s]\n' "BUILD debug" "$KERNEL_BUILD_DEBUG" "$(_state "$(kernel_image debug)")"
    printf '%-14s = %s  [%s]\n' BUSYBOX "$BUSYBOX_INSTALL" "$(_state "$BUSYBOX_INSTALL")"
    printf '%-14s = %s  [%s]\n' INITRAMFS "$INITRAMFS_IMG" "$(_state "$INITRAMFS_IMG")"
    printf '%-14s = %s  [%s]\n' NFS_ROOT "$NFS_ROOT" "$(_state "$NFS_ROOT")"
    printf '%-14s = %s  [%s]\n' DISK_IMG "$DISK_IMG" "$(_state "$DISK_IMG")"
    echo
    printf 'IN_DOCKER = %s   JOBS = %s   SUDO = %s\n' \
        "$IN_DOCKER" "$JOBS" "${SUDO:-(root)}"
}

# Exécuté directement (et non sourcé) : afficher l'état
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    common_status
fi
