#!/bin/bash
# build_rootfs.sh — construit les rootfs à base de busybox.
#
# Usage : ./infra/scripts/build_rootfs.sh <étape>
#
# Étapes :
#   busybox      compiler busybox (statique, sans tc)
#   initramfs    construire initramfs.cpio.gz (init + init_loop)
#   nfsroot      construire le rootfs NFS dans nfsroot/
#   nfs-server   exporter nfsroot/ et activer NFS      (hôte uniquement)
#   all          busybox + initramfs + nfsroot
#
# busybox et initramfs fonctionnent dans Docker comme sur l'hôte.
# nfs-server doit tourner sur l'hôte : le serveur NFS fait partie du noyau.

set -eu
source "$(dirname "$(realpath "$0")")/common.sh"

# ------------------------------------------------------------------ busybox -

cmd_busybox() {
    need_cmd git make gcc
    log "Compilation de busybox (statique, sans tc)"
    mkdir -p "$WORKSPACE"
    [ -d "$BUSYBOX_SRC" ] || git clone --depth 1 "$BUSYBOX_REPO" "$BUSYBOX_SRC"

    make -C "$BUSYBOX_SRC" defconfig
    # tc ne compile plus avec les en-têtes récents ; statique = pas de libs à copier
    sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' "$BUSYBOX_SRC/.config"
    sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' "$BUSYBOX_SRC/.config"
    yes "" | make -C "$BUSYBOX_SRC" oldconfig > /dev/null

    grep -q '^# CONFIG_TC is not set' "$BUSYBOX_SRC/.config" || die "tc est toujours activé"
    grep -q '^CONFIG_STATIC=y' "$BUSYBOX_SRC/.config" || die "busybox n'est pas en statique"

    make -C "$BUSYBOX_SRC" -j"$JOBS"
    make -C "$BUSYBOX_SRC" install    # -> _install/

    if ldd "$BUSYBOX_INSTALL/bin/busybox" > /dev/null 2>&1; then
        die "busybox est dynamique : les bibliothèques manqueraient au démarrage"
    fi
    echo "busybox : $BUSYBOX_INSTALL"
}

# ---------------------------------------------------------------- init_loop -

build_init_loop() {
    need_cmd gcc
    mkdir -p "$INIT_LOOP_DIR"
    cat > "$INIT_LOOP_DIR/main.c" <<'END'
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

int main(void)
{
    printf("ECOLE 2600 MY INIT\n");
    for (;;) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            sleep(1);
            continue;
        }
        if (pid == 0) {
            char *argv[] = {"/usr/bin/setsid", "/bin/cttyhack", "/bin/sh", NULL};
            execv(argv[0], argv);
            perror("execv");      /* on n'arrive ici que si execv échoue */
            _exit(1);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        printf("Respawn\n");
        sleep(1);                 /* évite une boucle folle si le shell meurt aussitôt */
    }
}
END
    gcc -static -O2 -Wall -o "$INIT_LOOP_DIR/init_loop" "$INIT_LOOP_DIR/main.c"
}

# La bannière, partagée par les deux rootfs
write_banner() {
    cat > "$1" <<'END'
   ______           __
  / ____/________  / /__
 / __/ / ___/ __ \/ / _ \
/ /___/ /__/ /_/ / /  __/
\____/\___/\____/_/\___/
   ___   _____ ____  ____
  |__ \ / ___// __ \/ __ \
  __/ // __ \/ / / / / / /
 / __// /_/ / /_/ / /_/ /
/____/\____/\____/\____/

Welcome to "Ecole 2600 linux"
END
}

# ---------------------------------------------------------------- initramfs -

cmd_initramfs() {
    need_cmd cpio gzip find
    need_file "$BUSYBOX_INSTALL" "lance d'abord : $0 busybox"
    build_init_loop

    log "Construction de l'initramfs"
    rm -rf "$INITRAMFS_BUILD"
    mkdir -p "$INITRAMFS_BUILD"/{bin,sbin,etc,proc,sys,dev,tmp,root,usr/bin,usr/sbin}
    cp -a "$BUSYBOX_INSTALL/." "$INITRAMFS_BUILD/"
    cp "$INIT_LOOP_DIR/init_loop" "$INITRAMFS_BUILD/init_loop"
    write_banner "$INITRAMFS_BUILD/etc/banner"

    # Lancé par le noyau comme PID 1
    cat > "$INITRAMFS_BUILD/init" <<'END'
#!/bin/sh
export PATH=/sbin:/usr/sbin:/bin:/usr/bin
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
echo "Boot took $(cut -d' ' -f1 /proc/uptime) seconds"
cat /etc/banner
exec /init_loop
END
    chmod +x "$INITRAMFS_BUILD/init"

    (cd "$INITRAMFS_BUILD" && find . -print0 | cpio --null -o --format=newc --quiet) \
        | gzip -9 > "$INITRAMFS_IMG"
    echo "initramfs : $INITRAMFS_IMG ($(du -h "$INITRAMFS_IMG" | cut -f1))"
}

# ------------------------------------------------------------------ nfsroot -

cmd_nfsroot() {
    need_file "$BUSYBOX_INSTALL" "lance d'abord : $0 busybox"
    log "Construction du rootfs NFS"

    # On ne vide pas le dossier : tes modifications dans le rootfs sont conservées
    mkdir -p "$NFS_ROOT"
    cp -a "$BUSYBOX_INSTALL/." "$NFS_ROOT/"
    mkdir -p "$NFS_ROOT"/{proc,sys,dev,etc/init.d,tmp,root}
    write_banner "$NFS_ROOT/etc/banner"

    # Le noyau ouvre /dev/console avant de lancer init : il doit déjà exister
    [ -e "$NFS_ROOT/dev/console" ] || $SUDO mknod -m 600 "$NFS_ROOT/dev/console" c 5 1
    [ -e "$NFS_ROOT/dev/null" ]    || $SUDO mknod -m 666 "$NFS_ROOT/dev/null" c 1 3

    cat > "$NFS_ROOT/etc/init.d/rcS" <<'END'
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
cat /etc/banner
END
    chmod +x "$NFS_ROOT/etc/init.d/rcS"

    cat > "$NFS_ROOT/etc/inittab" <<'END'
::sysinit:/etc/init.d/rcS
::askfirst:-/bin/sh
::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
END
    echo "rootfs NFS : $NFS_ROOT"
}

cmd_nfs_server() {
    not_in_docker
    need_file "$NFS_ROOT" "lance d'abord : $0 nfsroot"
    command -v exportfs > /dev/null || die "installe le paquet nfs-kernel-server"
    log "Configuration du serveur NFS"

    # Export limité à 127.0.0.1 : QEMU en mode utilisateur se présente depuis
    # l'hôte lui-même, et no_root_squash ne doit pas être ouvert au réseau.
    local line="$NFS_ROOT 127.0.0.1(rw,sync,no_subtree_check,no_root_squash,insecure)"
    if grep -qs "^$NFS_ROOT " /etc/exports; then
        echo "Déjà présent dans /etc/exports"
    else
        echo "$line" | $SUDO tee -a /etc/exports > /dev/null
        echo "Ajouté à /etc/exports : $line"
    fi
    $SUDO systemctl restart nfs-kernel-server
    $SUDO exportfs -rav
    showmount -e localhost
}

cmd_all() {
    cmd_busybox
    cmd_initramfs
    cmd_nfsroot
}

dispatch "$@"
