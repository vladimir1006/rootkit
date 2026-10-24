#!/bin/bash
# openwrt.sh — setup et lancement d'OpenWrt x86-64 sous QEMU.
#
# Usage : ./scripts/openwrt.sh <commande> [options]
#
# Commandes :
#   fetch           telecharger l'image OpenWrt x86-64
#   resize [taille] agrandir l'image (defaut : 512M)
#   sdk             telecharger le SDK (compilation de modules)
#   run             demarrer la VM QEMU
#   ssh             se connecter en SSH a la VM
#   push <fichier>  envoyer un fichier dans la VM via SCP
#   pull <fichier>  recuperer un fichier de la VM via SCP
#   setup-user      creer le compte utilisateur dans la VM
#   qcow2           convertir l'image en QCOW2 compresse
#   status          etat du workspace OpenWrt
#   clean           supprimer image et SDK
#
# L'image ext4 est utilisee (pas squashfs) pour pouvoir ecrire
# sur le rootfs et charger des modules kernel (insmod).
#
# SSH : root@localhost:2222 (pas de mot de passe par defaut).
# Quitter QEMU : Ctrl-A puis X.

set -eu
source "$(dirname "$(realpath "$0")")/common.sh"

# ------------------------------------------------------------ variables ---

OWRT_VERSION="${OWRT_VERSION:-23.05.6}"
OWRT_DIR="${OWRT_DIR:-$WORKSPACE/openwrt}"
OWRT_IMG_GZ="openwrt-${OWRT_VERSION}-x86-64-generic-ext4-combined.img.gz"
OWRT_IMG="${OWRT_IMG:-$OWRT_DIR/openwrt.img}"
OWRT_BASE_URL="${OWRT_BASE_URL:-https://archive.openwrt.org/releases/${OWRT_VERSION}/targets/x86/64}"

OWRT_SDK_TARBALL="openwrt-sdk-${OWRT_VERSION}-x86-64_gcc-12.3.0_musl.Linux-x86_64.tar.xz"
OWRT_SDK_DIR="${OWRT_SDK_DIR:-$OWRT_DIR/sdk}"

OWRT_SSH_PORT="${OWRT_SSH_PORT:-2222}"
OWRT_HTTP_PORT="${OWRT_HTTP_PORT:-8080}"
OWRT_RESIZE="${OWRT_RESIZE:-512M}"
OWRT_QCOW2="${OWRT_QCOW2:-$OWRT_DIR/openwrt.qcow2}"
OWRT_USER_NAME="${OWRT_USER_NAME:-user}"
OWRT_USER_PASS="${OWRT_USER_PASS:-user}"

# --- options SSH partagees ---
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p $OWRT_SSH_PORT"

vm_exec() { ssh $SSH_OPTS root@localhost "$@"; }


# ------------------------------------------------------------ fetch ---

cmd_fetch() {
    need_cmd wget gunzip
    log "Telechargement OpenWrt ${OWRT_VERSION} x86-64"
    mkdir -p "$OWRT_DIR"

    if [ -f "$OWRT_IMG" ]; then
        echo "Image deja presente : $OWRT_IMG"
        return 0
    fi

    local gz="$OWRT_DIR/$OWRT_IMG_GZ"
    if [ ! -f "$gz" ]; then
        wget -q --show-progress -O "$gz" "${OWRT_BASE_URL}/${OWRT_IMG_GZ}"
    fi

    echo "Decompression..."
    gzip -dc "$gz" > "$OWRT_IMG"

    echo "Image : $OWRT_IMG ($(du -h "$OWRT_IMG" | cut -f1))"
    echo
    echo "L'image fait ~120 Mo par defaut. Pour installer des paquets"
    echo "ou travailler avec le SDK, agrandis-la :"
    echo "  ./scripts/openwrt.sh resize"
}

# ------------------------------------------------------------ resize ---

cmd_resize() {
    local size="${1:-$OWRT_RESIZE}"
    need_cmd qemu-img
    need_file "$OWRT_IMG" "lance d'abord : $0 fetch"

    log "Agrandissement de l'image a $size"
    qemu-img resize -f raw "$OWRT_IMG" "$size"

    echo "Image redimensionnee : $(du -h "$OWRT_IMG" | cut -f1)"
    echo
    echo "Au premier boot, le rootfs sera etendu automatiquement"
    echo "par OpenWrt (losetup + resize2fs)."
    echo "Sinon dans la VM :  resize2fs /dev/sda2"
}

# ------------------------------------------------------------ sdk ---

cmd_sdk() {
    need_cmd wget tar
    log "Telechargement du SDK OpenWrt ${OWRT_VERSION}"
    mkdir -p "$OWRT_DIR"

    if [ -d "$OWRT_SDK_DIR" ]; then
        echo "SDK deja present : $OWRT_SDK_DIR"
        return 0
    fi

    local tarball="$OWRT_DIR/$OWRT_SDK_TARBALL"
    if [ ! -f "$tarball" ]; then
        wget -q --show-progress -O "$tarball" "${OWRT_BASE_URL}/${OWRT_SDK_TARBALL}"
    fi

    echo "Extraction..."
    mkdir -p "$OWRT_SDK_DIR"
    tar -xf "$tarball" -C "$OWRT_SDK_DIR" --strip-components=1

    echo "SDK : $OWRT_SDK_DIR"
    echo
    echo "Pour compiler un module kernel :"
    echo "  1. Cree un dossier package/monmodule/ dans le SDK"
    echo "  2. Ecris le Makefile OpenWrt (voir template : $0 module-template)"
    echo "  3. make package/monmodule/compile V=s"
}

# ------------------------------------------------------------ run ---

cmd_run() {
    not_in_docker
    need_cmd qemu-system-x86_64
    need_file "$OWRT_IMG" "lance d'abord : $0 fetch"

    local pause=""
    for arg in "$@"; do
        case "$arg" in
            --pause|-S) pause="-S" ;;
            *) die "argument inconnu : $arg" ;;
        esac
    done

    log "Demarrage OpenWrt x86-64"
    echo "SSH  : ssh root@localhost -p $OWRT_SSH_PORT"
    echo "LuCI : http://localhost:$OWRT_HTTP_PORT"
    echo "Quitter QEMU : Ctrl-A puis X"
    [ -n "$pause" ] && echo "En pause : connecte GDB"
    echo

    qemu-system-x86_64 \
        -m "$QEMU_MEM" -smp 2 -nographic \
        -drive "file=$OWRT_IMG,format=raw,if=virtio" \
        -device virtio-net-pci,netdev=lan \
        -netdev "user,id=lan,hostfwd=tcp::${OWRT_SSH_PORT}-:22,hostfwd=tcp::${OWRT_HTTP_PORT}-:80" \
        $pause
}

# ------------------------------------------------------------ ssh ---

cmd_ssh() {
    need_cmd ssh
    echo "Connexion SSH a la VM OpenWrt..."
    ssh $SSH_OPTS root@localhost "$@"
}

cmd_push() {
    [ $# -ge 1 ] || die "usage : $0 push <fichier_local> [dest_vm]"
    need_cmd scp
    local src="$1"
    local dst="${2:-/tmp/$(basename "$1")}"
    need_file "$src" "fichier a envoyer"
    scp $SSH_OPTS "$src" "root@localhost:$dst"
    echo "Envoye : $src -> VM:$dst"
}

cmd_pull() {
    [ $# -ge 1 ] || die "usage : $0 pull <fichier_vm> [dest_locale]"
    need_cmd scp
    local src="$1"
    local dst="${2:-.}"
    scp $SSH_OPTS "root@localhost:$src" "$dst"
    echo "Recupere : VM:$src -> $dst"
}

# --------------------------------------------------------- setup-user ---

cmd_setup_user() {
    need_cmd ssh
    log "Creation du compte ${OWRT_USER_NAME}"

    local name="$OWRT_USER_NAME"
    local pass="$OWRT_USER_PASS"

    # Verifier si le compte existe deja
    if vm_exec "grep -q '^${name}:' /etc/passwd" 2>/dev/null; then
        echo "Compte deja present."
        return 0
    fi

    # Creer l'entree passwd + group
    vm_exec "echo '${name}:x:1000:1000:${name}:/home/${name}:/bin/ash' >> /etc/passwd"
    vm_exec "echo '${name}:x:1000:' >> /etc/group"

    # Home directory
    vm_exec "mkdir -p /home/${name} && chown ${name}:${name} /home/${name}"

    # Mot de passe
    vm_exec "echo '${name}:${pass}' | chpasswd"

    echo "Compte cree : ${name} / ${pass}"
}

# -------------------------------------------------------- module-template ---

cmd_module_template() {
    local dir="${1:-$WORKSPACE/my_owrt_module}"
    log "Creation du template de module kernel OpenWrt"
    mkdir -p "$dir/src"

    cat > "$dir/src/monmodule.c" <<'ENDC'
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ecole 2600");
MODULE_DESCRIPTION("Module kernel de test");

static int __init monmodule_init(void)
{
    pr_info("monmodule: charge\n");
    return 0;
}

static void __exit monmodule_exit(void)
{
    pr_info("monmodule: decharge\n");
}

module_init(monmodule_init);
module_exit(monmodule_exit);
ENDC

    cat > "$dir/src/Makefile" <<'ENDM'
obj-m += monmodule.o
ENDM

    cat > "$dir/Makefile" <<'ENDPKG'
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=monmodule
PKG_VERSION:=1.0
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/monmodule
  SUBMENU:=Other modules
  TITLE:=Module de test Ecole 2600
  FILES:=$(PKG_BUILD_DIR)/monmodule.ko
endef

define KernelPackage/monmodule/description
  Module kernel de test pour le projet cybersec.
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,monmodule))
ENDPKG

    echo "Template cree : $dir/"
    echo
    echo "Pour compiler avec le SDK :"
    echo "  1. ln -s $dir $OWRT_SDK_DIR/package/monmodule"
    echo "  2. cd $OWRT_SDK_DIR"
    echo "  3. make package/monmodule/compile V=s"
    echo
    echo "Pour charger dans la VM :"
    echo "  ./scripts/openwrt.sh push <chemin>/monmodule.ko"
    echo "  ./scripts/openwrt.sh ssh insmod /tmp/monmodule.ko"
}

# ------------------------------------------------------------ qcow2 ---

cmd_qcow2() {
    need_cmd qemu-img
    need_file "$OWRT_IMG" "lance d'abord : $0 fetch"

    log "Conversion en QCOW2 compresse"
    qemu-img convert -c -f raw -O qcow2 "$OWRT_IMG" "$OWRT_QCOW2"
    echo "Image QCOW2 : $OWRT_QCOW2 ($(du -h "$OWRT_QCOW2" | cut -f1))"
}

# ------------------------------------------------------------ status ---

cmd_status() {
    printf '%-14s = %s\n' OWRT_VERSION "$OWRT_VERSION"
    printf '%-14s = %s  [%s]\n' OWRT_IMG "$OWRT_IMG" "$(_state "$OWRT_IMG")"
    printf '%-14s = %s  [%s]\n' OWRT_SDK "$OWRT_SDK_DIR" "$(_state "$OWRT_SDK_DIR")"
    printf '%-14s = %s\n' SSH_PORT "$OWRT_SSH_PORT"
    printf '%-14s = %s\n' HTTP_PORT "$OWRT_HTTP_PORT"
}

# ------------------------------------------------------------ clean ---

cmd_clean() {
    confirm "Supprimer $OWRT_DIR (image + SDK) ?"
    rm -rf "$OWRT_DIR"
    echo "Supprime : $OWRT_DIR"
}

dispatch "$@"
