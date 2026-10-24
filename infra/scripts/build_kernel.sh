#!/bin/bash
# build_kernel.sh — télécharge, configure et compile le noyau Linux.
#
# Usage : ./infra/scripts/build_kernel.sh <étape> [profil] [options]
#
# Étapes :
#   deps              installer les paquets de compilation
#   fetch             télécharger et extraire les sources
#   config [profil]   écrire la config du profil
#   compile [profil]  compiler (config faite si besoin)
#   all [profil]      fetch + config + compile
#   clean [profil]    supprimer le dossier de build du profil
#   mrproper          nettoyer les sources (obligatoire une fois avant O=)
#
# Profils :
#   (défaut)   NFS + disque (ext4/SATA), sans symboles de debug  -> build/
#   debug      idem + DEBUG_INFO, GDB_SCRIPTS, KGDB              -> build-debug/
#
# Options :
#   --no-nfs    ne pas activer le support du rootfs NFS
#   --no-disk   ne pas activer ext4 et les pilotes disque
#
# Fonctionne dans Docker comme sur l'hôte.

set -eu
source "$(dirname "$(realpath "$0")")/common.sh"

PROFILE="base"
WITH_NFS=1
WITH_DISK=1

# Groupes d'options passés à scripts/config
NFS_OPTS="NETWORK_FILESYSTEMS NFS_FS NFS_V3 IP_PNP ROOT_NFS E1000"
# Support disque (ext4, SATA, AHCI, PIIX)
DISK_OPTS="EXT4_FS BLK_DEV_SD ATA SATA_AHCI ATA_PIIX"
# Support debug (symboles, GDB, KGDB)
DEBUG_OPTS="DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT GDB_SCRIPTS KGDB KGDB_SERIAL_CONSOLE KGDB_KDB DEBUG_INFO_BTF"
# Support des modules chargeables (LKM)
MODULE_OPTS="MODULES MODULE_UNLOAD"
# Instrumentation kernel (ftrace, kprobes, tracing) — utile LKM + eBPF
TRACE_OPTS="KPROBES KPROBE_EVENTS UPROBE_EVENTS TRACEPOINTS PERF_EVENTS FTRACE FUNCTION_TRACER"
# eBPF core + JIT + intégration réseau/LSM
BPF_OPTS="BPF BPF_SYSCALL BPF_JIT BPF_EVENTS BPF_LSM NET_CLS_BPF NET_ACT_BPF"
# Sécurité kernel (LSM stack)
SECURITY_OPTS="SECURITY SECURITYFS SECURITY_LOCKDOWN_LSM SECURITY_LANDLOCK SECURITY_YAMA INTEGRITY"

parse_args() {
    for arg in "$@"; do
        case "$arg" in
            base|debug) PROFILE="$arg" ;;
            --no-nfs)   WITH_NFS=0 ;;
            --no-disk)  WITH_DISK=0 ;;
            *) die "argument inconnu : $arg" ;;
        esac
    done
}

_yesno() { [ "$1" = 1 ] && echo "activé" || echo "ignoré"; }

show_profile() {
    echo "Profil : $PROFILE  ->  $(kernel_build "$PROFILE")"
    printf '  %-26s %s\n' "NFS (nfsroot, IP_PNP)"     "$(_yesno "$WITH_NFS")"
    printf '  %-26s %s\n' "Disque (ext4, SATA)"       "$(_yesno "$WITH_DISK")"
    printf '  %-26s %s\n' "Debug (DEBUG_INFO, KGDB)"  "$([ "$PROFILE" = debug ] && echo activé || echo ignoré)"
}

# Les sources doivent être propres pour utiliser make O=
check_srctree_clean() {
    if [ -f "$KERNEL_SRC/.config" ] || [ -f "$KERNEL_SRC/vmlinux" ]; then
        die "les sources contiennent une compilation en place.
       Lance une fois : $0 mrproper   (les objets déjà compilés seront perdus)"
    fi
}

cmd_deps() {
    log "Installation des dépendances de compilation"
    $SUDO apt-get update
    $SUDO apt-get install -y build-essential flex bison libssl-dev libelf-dev \
        bc cpio git wget libncurses-dev
}

cmd_fetch() {
    need_cmd wget tar
    log "Sources du noyau $KERNEL_VERSION"
    mkdir -p "$WORKSPACE"
    cd "$WORKSPACE"
    [ -f "linux-${KERNEL_VERSION}.tar.xz" ] || wget -q --show-progress "$KERNEL_URL"
    [ -d "$KERNEL_SRC" ] || tar -xf "linux-${KERNEL_VERSION}.tar.xz"
    echo "Sources : $KERNEL_SRC"
}

cmd_mrproper() {
    need_file "$KERNEL_SRC" "lance d'abord : $0 fetch"
    log "Nettoyage des sources"
    make -C "$KERNEL_SRC" mrproper
}

cmd_config() {
    parse_args "$@"
    need_file "$KERNEL_SRC" "lance d'abord : $0 fetch"
    check_srctree_clean

    local build opts=() o
    build="$(kernel_build "$PROFILE")"
    log "Configuration du noyau"
    show_profile

    mkdir -p "$build"
    [ -f "$build/.config" ] || make -C "$KERNEL_SRC" O="$build" defconfig

    if [ "$PROFILE" = debug ]; then
        opts+=(--disable DEBUG_INFO_NONE)
        for o in $DEBUG_OPTS; do opts+=(--enable "$o"); done
    else
        # Sans symboles : compilation ~2x plus rapide, vmlinux ~10x plus petit
        opts+=(--enable DEBUG_INFO_NONE)
    fi

    # ajout des options de configuration pour les modules, tracing, bpf et security (toujours activés)
    for o in $MODULE_OPTS;   do opts+=(--enable "$o"); done
    for o in $TRACE_OPTS;    do opts+=(--enable "$o"); done
    for o in $BPF_OPTS;      do opts+=(--enable "$o"); done
    for o in $SECURITY_OPTS; do opts+=(--enable "$o"); done

    if [ "$WITH_NFS" = 1 ]; then
        for o in $NFS_OPTS; do opts+=(--enable "$o"); done
    fi
    if [ "$WITH_DISK" = 1 ]; then
        for o in $DISK_OPTS; do opts+=(--enable "$o"); done
    fi

    "$KERNEL_SRC/scripts/config" --file "$build/.config" "${opts[@]}"
    make -C "$KERNEL_SRC" O="$build" olddefconfig

    # Ces options doivent être intégrées au noyau (=y) pour être disponibles au démarrage.
    local expected=""
    [ "$WITH_NFS" = 1 ]  && expected="$expected $NFS_OPTS"
    [ "$WITH_DISK" = 1 ] && expected="$expected $DISK_OPTS"
    [ "$PROFILE" = debug ] && expected="$expected DEBUG_INFO GDB_SCRIPTS KGDB"
    expected="$expected $MODULE_OPTS $TRACE_OPTS $BPF_OPTS $SECURITY_OPTS"
    for o in $expected; do
        grep -q "^CONFIG_${o}=y" "$build/.config" || die "CONFIG_${o} n'est pas à =y"
    done
    echo "Config écrite : $build/.config"
}

cmd_compile() {
    parse_args "$@"
    local build; build="$(kernel_build "$PROFILE")"
    [ -f "$build/.config" ] || cmd_config "$@"

    log "Compilation ($JOBS jobs)"
    show_profile
    make -C "$KERNEL_SRC" O="$build" -j"$JOBS"
    if [ "$PROFILE" = debug ]; then
        make -C "$KERNEL_SRC" O="$build" scripts_gdb
    fi

    echo
    echo "Noyau : $(kernel_image "$PROFILE")"
    [ "$PROFILE" = debug ] && echo "GDB   : gdb $(kernel_vmlinux debug)"
    return 0
}

cmd_clean() {
    parse_args "$@"
    local build; build="$(kernel_build "$PROFILE")"
    [ -d "$build" ] || die "$build n'existe pas"
    confirm "Supprimer $build ?"
    rm -rf "$build"
    echo "Supprimé : $build"
}

cmd_all() {
    cmd_fetch
    cmd_config "$@"
    cmd_compile "$@"
}

dispatch "$@"
