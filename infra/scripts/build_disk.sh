#!/bin/bash
# build_disk.sh — construit une image disque bootable (Alpine + GRUB BIOS).
#
# Usage : ./infra/scripts/build_disk.sh <étape> [profil]
#
# Étapes :
#   image           créer disk.img, la partitionner et la formater en ext4
#   alpine          peupler le rootfs avec Alpine via Docker
#   grub [profil]   installer le noyau et GRUB dans l'image
#   all [profil]    image + alpine + grub
#   mount           monter l'image pour aller la modifier à la main
#   umount          démonter et détacher l'image
#
# Profils : base (défaut) ou debug — choisit quel noyau installer.
#
# Hôte uniquement : nécessite losetup, mount, docker et grub-pc-bin.
# Paquets : parted dosfstools grub-pc-bin docker.io

set -eu
source "$(dirname "$(realpath "$0")")/common.sh"

PROFILE="base"
LOOP=""
MOUNTED=0
KEEP=0      # mis à 1 par la sous-commande mount : on ne démonte pas en sortant

parse_args() {
    for arg in "$@"; do
        case "$arg" in
            base|debug) PROFILE="$arg" ;;
            *) die "argument inconnu : $arg" ;;
        esac
    done
}

# Démontage systématique en sortie, y compris sur erreur ou Ctrl-C :
# sans ça, l'image reste montée et le lancement suivant échoue.
cleanup() {
    [ "$KEEP" = 1 ] && return 0
    if [ "$MOUNTED" = 1 ]; then
        sync
        $SUDO umount "$MNT_DIR" 2>/dev/null || true
        MOUNTED=0
    fi
    if [ -n "$LOOP" ]; then
        $SUDO losetup -d "$LOOP" 2>/dev/null || true
        LOOP=""
    fi
}
trap cleanup EXIT

attach() {
    need_file "$DISK_IMG" "lance d'abord : $0 image"
    LOOP="$($SUDO losetup -Pf --show "$DISK_IMG")"
    # udev met un instant à créer loopXp1
    local i
    for i in 1 2 3 4 5 6 7 8 9 10; do
        [ -b "${LOOP}p1" ] && break
        sleep 0.2
    done
    [ -b "${LOOP}p1" ] || die "${LOOP}p1 n'apparaît pas — table de partitions absente ?"
    echo "Loop : $LOOP"
}

mount_part() {
    mkdir -p "$MNT_DIR"
    $SUDO mount "${LOOP}p1" "$MNT_DIR"
    MOUNTED=1
    echo "Monté : $MNT_DIR"
}

cmd_image() {
    not_in_docker
    need_cmd truncate parted losetup mkfs.ext4
    [ -e "$DISK_IMG" ] && confirm "Écraser $DISK_IMG ?"

    log "Création de l'image disque ($DISK_SIZE)"
    rm -f "$DISK_IMG"
    truncate -s "$DISK_SIZE" "$DISK_IMG"
    parted -s "$DISK_IMG" mktable msdos
    parted -s "$DISK_IMG" mkpart primary ext4 1 "100%"
    parted -s "$DISK_IMG" set 1 boot on

    attach
    $SUDO mkfs.ext4 -q -L rootfs "${LOOP}p1"
    echo "Image prête : $DISK_IMG"
}

cmd_alpine() {
    not_in_docker
    [ -n "$ROOT_PASSWORD" ] || die "définis ROOT_PASSWORD avant de construire Alpine"
    need_cmd docker
    log "Peuplement du rootfs avec Alpine"
    attach
    mount_part

    # Tout le paramétrage Alpine en un seul conteneur, sans interaction
    $SUDO docker run --rm -v "$MNT_DIR:/my-rootfs" \
        -e "ROOT_PASSWORD=$ROOT_PASSWORD" "$ALPINE_IMAGE" sh -euc '
        apk add --no-cache openrc util-linux build-base
        apk add --no-cache openrc util-linux build-base
        # Installe les deps pour miniedr
        apk add --no-cache clang llvm bpftool libbpf-dev linux-headers

        # Console sur le port série : c est par la que QEMU nous parle
        ln -sf agetty /etc/init.d/agetty.ttyS0
        echo ttyS0 > /etc/securetty
        rc-update add agetty.ttyS0 default
        rc-update add root default

        echo "root:$ROOT_PASSWORD" | chpasswd

        # Pseudo-systemes de fichiers montes au demarrage
        rc-update add devfs boot
        rc-update add procfs boot
        rc-update add sysfs boot

        # Copie du systeme du conteneur vers la partition montee
        for d in bin etc lib root sbin usr; do tar c "/$d" | tar x -C /my-rootfs; done
        for dir in dev proc run sys var; do mkdir -p "/my-rootfs/$dir"; done
    '

    $SUDO tee "$MNT_DIR/etc/issue" > /dev/null <<'END'

Ecole 2600 linux \r (\l)

END
    echo "Rootfs Alpine installé (root / $ROOT_PASSWORD)"
}

cmd_grub() {
    not_in_docker
    parse_args "$@"
    need_cmd grub-install
    [ -f /usr/lib/grub/i386-pc/modinfo.sh ] \
        || die "GRUB BIOS absent — installe le paquet grub-pc-bin (pas grub-pc)"

    local img; img="$(kernel_image "$PROFILE")"
    need_file "$img" "lance d'abord : ./infra/scripts/build_kernel.sh all $PROFILE"
    warn_if_outdated "$PROFILE"

    log "Installation du noyau ($PROFILE) et de GRUB"
    attach
    mount_part

    $SUDO mkdir -p "$MNT_DIR/boot/grub"
    $SUDO cp "$img" "$MNT_DIR/boot/vmlinuz"

    # nokaslr : indispensable pour que les breakpoints GDB tombent juste
    local cmdline="root=/dev/sda1 console=ttyS0"
    [ "$PROFILE" = debug ] && cmdline="$cmdline nokaslr"

    $SUDO tee "$MNT_DIR/boot/grub/grub.cfg" > /dev/null <<END
serial
terminal_input serial
terminal_output serial
set timeout=3
set root=(hd0,1)
menuentry "Linux2600 ($PROFILE)" {
    linux /boot/vmlinuz $cmdline
}
END

    # Le MBR est au debut du DISQUE, pas de la partition : cible $LOOP, pas ${LOOP}p1
    $SUDO grub-install --directory=/usr/lib/grub/i386-pc \
        --boot-directory="$MNT_DIR/boot" "$LOOP"
    echo "GRUB installé — cmdline : $cmdline"
}

cmd_mount() {
    not_in_docker
    attach
    mount_part
    KEEP=1      # on laisse monté : c'est le but de cette sous-commande
    echo
    echo "Image montée sur $MNT_DIR (loop $LOOP)"
    echo "Pense à démonter : $0 umount"
}

cmd_umount() {
    not_in_docker
    local loops
    loops="$($SUDO losetup -j "$DISK_IMG" -O NAME -n 2>/dev/null || true)"
    sync
    mountpoint -q "$MNT_DIR" 2>/dev/null && $SUDO umount "$MNT_DIR"
    for l in $loops; do
        $SUDO losetup -d "$l" && echo "Détaché : $l"
    done
    [ -n "$loops" ] || echo "Aucun loop device associé à $DISK_IMG"
}

cmd_all() {
    cmd_image
    cmd_alpine
    cmd_grub "$@"
}

dispatch "$@"
