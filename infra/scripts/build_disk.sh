#!/bin/bash
# build_disk.sh — construit la LFS Alpine du TP01 avec GRUB BIOS.
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

REPO_ROOT="$(dirname "$WORKSPACE")"
PROFILE="base"
LOOP=""
PART=""
PART_LOOP=""
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
    if [ -n "$PART_LOOP" ]; then
        $SUDO losetup -d "$PART_LOOP" 2>/dev/null || true
        PART_LOOP=""
    fi
    # Détache tous les loops liés à $DISK_IMG, pas seulement $LOOP :
    # un loop périmé d'un run précédent corromprait GRUB au suivant.
    local loops l
    loops="$($SUDO losetup -j "$DISK_IMG" -O NAME -n 2>/dev/null || true)"
    for l in $loops; do
        $SUDO losetup -d "$l" 2>/dev/null || true
    done
    LOOP=""
    PART=""
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
    if [ -b "${LOOP}p1" ]; then
        PART="${LOOP}p1"
    else
        # Docker Desktop ne publie pas toujours les partitions loop. Créer
        # alors un loop secondaire directement sur l'offset de la partition.
        local geometry start size
        geometry="$(parted -sm "$DISK_IMG" unit B print | awk -F: '$1 == "1" { print $2 " " $4 }')"
        read -r start size <<EOF
$geometry
EOF
        start="${start%B}"
        size="${size%B}"
        [ -n "$start" ] && [ -n "$size" ] || \
            die "partition 1 introuvable dans $DISK_IMG"
        PART_LOOP="$($SUDO losetup -f --show -o "$start" --sizelimit "$size" "$DISK_IMG")"
        PART="$PART_LOOP"
    fi
    echo "Loop : $LOOP"
    echo "Partition : $PART"
}

mount_part() {
    mkdir -p "$MNT_DIR"
    if mountpoint -q "$MNT_DIR" 2>/dev/null; then
        MOUNTED=1
        echo "Déjà monté : $MNT_DIR"
        return 0
    fi
    $SUDO mount "$PART" "$MNT_DIR"
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
    $SUDO mkfs.ext4 -q -L rootfs "$PART"
    echo "Image prête : $DISK_IMG"
}

cmd_alpine() {
    not_in_docker
    [ -n "$ROOT_PASSWORD" ] || die "définis ROOT_PASSWORD avant de construire Alpine"
    [ -n "$USER_PASSWORD" ] || die "définis USER_PASSWORD avant de construire Alpine"
    if [ "$IN_DOCKER" = "oui" ]; then
        need_file "${ALPINE_ROOTFS_TAR:-}" \
            "définis ALPINE_ROOTFS_TAR vers un export de l'image Alpine"
        need_cmd chroot openssl tar
    else
        need_cmd docker
    fi
    need_file "$REPO_ROOT/module/rk2600.ko" "lance d'abord : make module"
    need_file "$REPO_ROOT/companion/rk2600" "lance d'abord : make companion"
    need_file "$REPO_ROOT/infra/rootfs/snd-compat.start"
    log "Peuplement du rootfs avec Alpine"
    attach
    mount_part

    if [ "$IN_DOCKER" = "oui" ]; then
        # Mode CI/Windows : le conteneur courant est privilégié et reçoit un
        # export tar de l'image Alpine officielle. Le rootfs est configuré par
        # chroot, sans dépendre d'un moteur Docker imbriqué.
        $SUDO tar -xpf "$ALPINE_ROOTFS_TAR" -C "$MNT_DIR"
        $SUDO cp /etc/resolv.conf "$MNT_DIR/etc/resolv.conf"
        $SUDO chroot "$MNT_DIR" /bin/sh -euc '
            apk add --no-cache openrc util-linux build-base
            ln -sf agetty /etc/init.d/agetty.ttyS0
            echo ttyS0 > /etc/securetty
            rc-update add agetty.ttyS0 default
            rc-update add root default
            rc-update add devfs boot
            rc-update add procfs boot
            rc-update add sysfs boot
            adduser -D -s /bin/sh user
            for dir in dev proc run sys var; do mkdir -p "/$dir"; done
        '
        local root_hash user_hash
        root_hash="$(printf '%s' "$ROOT_PASSWORD" | openssl passwd -6 -stdin)"
        user_hash="$(printf '%s' "$USER_PASSWORD" | openssl passwd -6 -stdin)"
        printf 'root:%s\nuser:%s\n' "$root_hash" "$user_hash" | \
            $SUDO chroot "$MNT_DIR" /usr/sbin/chpasswd -e
    else
        # Tout le paramétrage Alpine en un seul conteneur, sans interaction
        $SUDO docker run --rm -v "$MNT_DIR:/my-rootfs" \
        -e "ROOT_PASSWORD=$ROOT_PASSWORD" \
        -e "USER_PASSWORD=$USER_PASSWORD" \
        "$ALPINE_IMAGE" sh -euc '
        apk add --no-cache openrc util-linux build-base
        # Installe les deps pour miniedr
        apk add --no-cache clang llvm bpftool libbpf-dev linux-headers

        # Console sur le port série : c est par la que QEMU nous parle
        ln -sf agetty /etc/init.d/agetty.ttyS0
        echo ttyS0 > /etc/securetty
        rc-update add agetty.ttyS0 default
        rc-update add root default

        echo "root:$ROOT_PASSWORD" | chpasswd

        # Compte user pour la démonstration du rootkit
        adduser -D -s /bin/sh user
        echo "user:$USER_PASSWORD" | chpasswd

        # Pseudo-systemes de fichiers montes au demarrage
        rc-update add devfs boot
        rc-update add procfs boot
        rc-update add sysfs boot

        # Copie du systeme du conteneur vers la partition montee
        for d in bin etc lib root sbin usr; do tar c "/$d" | tar x -C /my-rootfs; done
        for dir in dev proc run sys var; do mkdir -p "/my-rootfs/$dir"; done
        '
    fi

    $SUDO tee "$MNT_DIR/etc/issue" > /dev/null <<'END'

Ecole 2600 linux \r (\l)

END
    $SUDO mkdir -p "$MNT_DIR/lib/modules/$KERNEL_VERSION/extra" \
        "$MNT_DIR/usr/local/bin" "$MNT_DIR/etc/local.d"
    $SUDO cp "$REPO_ROOT/module/rk2600.ko" \
        "$MNT_DIR/lib/modules/$KERNEL_VERSION/extra/rk2600.ko"
    $SUDO cp "$REPO_ROOT/companion/rk2600" "$MNT_DIR/usr/local/bin/rk2600"
    $SUDO cp "$REPO_ROOT/infra/rootfs/snd-compat.start" \
        "$MNT_DIR/etc/local.d/snd-compat.start"
    $SUDO chmod 0755 "$MNT_DIR/usr/local/bin/rk2600" \
        "$MNT_DIR/etc/local.d/snd-compat.start"
    $SUDO chroot "$MNT_DIR" /sbin/rc-update add local default
    echo "Rootfs Alpine installé (comptes root et user configurés)"
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
    local cmdline="root=/dev/sda1 rw console=ttyS0"
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
        --boot-directory="$MNT_DIR/boot" --recheck \
        --modules="biosdisk part_msdos ext2 normal linux search search_fs_uuid configfile" \
        "$LOOP"
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
    cleanup
    cmd_alpine
    cleanup
    cmd_grub "$@"
}

dispatch "$@"
