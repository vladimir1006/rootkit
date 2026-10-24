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
#   init            configurer DHCP + cle SSH (a faire une fois)
#   setup-user      creer le compte utilisateur dans la VM
#   compile <mod>   compiler un module avec le SDK
#   insmod <mod>    push + charger un module dans la VM
#   rmmod <mod>     decharger un module de la VM
#   clean-module <m> supprimer un module (sources + build)
#   qcow2           convertir l'image en QCOW2 compresse (livraison)
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

SSH_COMMON="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

vm_exec() { ssh $SSH_COMMON -p "$OWRT_SSH_PORT" root@localhost "$@"; }
vm_scp()  { scp -O $SSH_COMMON -P "$OWRT_SSH_PORT" "$@"; }

# ------------------------------------------------------------ fetch ---

cmd_fetch() {
    need_cmd wget gzip
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
    vm_exec "$@"
}

cmd_push() {
    [ $# -ge 1 ] || die "usage : $0 push <fichier_local> [dest_vm]"
    need_cmd scp
    local src="$1"
    local dst="${2:-/tmp/$(basename "$1")}"
    need_file "$src" "fichier a envoyer"
    vm_scp "$src" "root@localhost:$dst"
    echo "Envoye : $src -> VM:$dst"
}

cmd_pull() {
    [ $# -ge 1 ] || die "usage : $0 pull <fichier_vm> [dest_locale]"
    need_cmd scp
    local src="$1"
    local dst="${2:-.}"
    vm_scp "root@localhost:$src" "$dst"
    echo "Recupere : VM:$src -> $dst"
}

# -------------------------------------------------------- module-template ---

cmd_module_template() {
    local name="${1:?usage : $0 module-template <nom>}"
    local dir="$WORKSPACE/module/${name}"
    if [ -d "$dir" ]; then
        die "Le module '${name}' existe deja : $dir"
    fi
    log "Creation du module '${name}'"
    mkdir -p "$dir/src"

    sed "s/MODNAME/${name}/g" > "$dir/src/${name}.c" <<'ENDC'
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ecole 2600");
MODULE_DESCRIPTION("MODNAME - module kernel");

static int __init MODNAME_init(void)
{
    pr_info("MODNAME: charge\n");
    return 0;
}

static void __exit MODNAME_exit(void)
{
    pr_info("MODNAME: decharge\n");
}

module_init(MODNAME_init);
module_exit(MODNAME_exit);
ENDC

    sed "s/MODNAME/${name}/g" > "$dir/src/Makefile" <<'ENDM'
obj-m += MODNAME.o
ENDM

    sed "s/MODNAME/${name}/g" > "$dir/Makefile" <<'ENDPKG'
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=MODNAME
PKG_VERSION:=1.0
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/MODNAME
  SUBMENU:=Other modules
  TITLE:=MODNAME (Ecole 2600)
  FILES:=$(PKG_BUILD_DIR)/MODNAME.ko
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,MODNAME))
ENDPKG

    echo "Module cree : $dir/"
    echo "  Compiler : make owrt-compile M=${name}"
    echo "  Charger  : make owrt-insmod M=${name}"
}

# ------------------------------------------------------------ compile ---

cmd_compile() {
    local name="${1:?usage : $0 compile <nom_module>}"
    local mod_dir="$WORKSPACE/module/${name}"
    need_file "$mod_dir/Makefile" "module '${name}' introuvable dans module/"

    if [ ! -d "$OWRT_SDK_DIR" ]; then
        die "SDK absent. Lance d'abord : make owrt-sdk"
    fi

    # Symlink dans le SDK
    local link="$OWRT_SDK_DIR/package/${name}"
    if [ ! -L "$link" ]; then
        ln -sf "$(realpath "$mod_dir")" "$link"
        log "Symlink : $link -> $mod_dir"
    fi

    log "Compilation du module '${name}'"
    make -C "$OWRT_SDK_DIR" "package/${name}/compile" V=s
}

# ------------------------------------------------------------- insmod ---

cmd_insmod() {
    local name="${1:?usage : $0 insmod <nom_module>}"
    need_cmd ssh

    # Trouver le .ko
    local ko
    ko=$(find "$OWRT_SDK_DIR" -name "${name}.ko" 2>/dev/null | head -1)
    if [ -z "$ko" ]; then
        die "Fichier ${name}.ko introuvable. Lance d'abord : make owrt-compile M=${name}"
    fi

    log "Push + chargement du module '${name}'"
    vm_scp "$ko" "root@localhost:/tmp/${name}.ko"
    vm_exec "insmod /tmp/${name}.ko"
    echo "Module charge. Verifier : dmesg | tail"
}

# -------------------------------------------------------------- rmmod ---

cmd_rmmod() {
    local name="${1:?usage : $0 rmmod <nom_module>}"
    need_cmd ssh

    log "Dechargement du module '${name}'"
    vm_exec "rmmod ${name}"
    echo "Module decharge."
}

# --------------------------------------------------------- clean-module ---

cmd_clean_module() {
    local name="${1:?usage : $0 clean-module <nom_module>}"
    local mod_dir="$WORKSPACE/module/${name}"
    local link="$OWRT_SDK_DIR/package/${name}"
    local build="$OWRT_SDK_DIR/build_dir"

    log "Nettoyage du module '${name}'"

    # Supprimer le symlink dans le SDK
    [ -L "$link" ] && rm -f "$link" && echo "Symlink supprime : $link"

    # Supprimer les artefacts de build
    local build_found=$(find "$build" -type d -name "${name}" 2>/dev/null)
    if [ -n "$build_found" ]; then
        rm -rf $build_found
        echo "Build supprime"
    fi

    # Supprimer le dossier module
    if [ -d "$mod_dir" ]; then
        confirm "Supprimer $mod_dir ?"
        rm -rf "$mod_dir"
        echo "Module supprime : $mod_dir"
    else
        echo "Dossier $mod_dir introuvable."
    fi
}

# -------------------------------------------------------------- qcow2 ---

cmd_qcow2() {
    need_cmd qemu-img
    need_file "$OWRT_IMG" "lance d'abord : $0 fetch"

    log "Conversion en QCOW2 compresse"
    qemu-img convert -c -f raw -O qcow2 "$OWRT_IMG" "$OWRT_QCOW2"

    echo "Image QCOW2 : $OWRT_QCOW2 ($(du -h "$OWRT_QCOW2" | cut -f1))"
    echo
    echo "Pour tester le boot depuis le QCOW2 :"
    echo "  qemu-system-x86_64 -m 256 -smp 2 -nographic \\"
    echo "    -drive file=$OWRT_QCOW2,format=qcow2,if=virtio \\"
    echo "    -device virtio-net-pci,netdev=lan \\"
    echo "    -netdev user,id=lan,hostfwd=tcp::${OWRT_SSH_PORT}-:22"
}

# ------------------------------------------------------------ status ---

cmd_status() {
    printf '%-14s = %s\n' OWRT_VERSION "$OWRT_VERSION"
    printf '%-14s = %s  [%s]\n' OWRT_IMG "$OWRT_IMG" "$(_state "$OWRT_IMG")"
    printf '%-14s = %s  [%s]\n' OWRT_QCOW2 "$OWRT_QCOW2" "$(_state "$OWRT_QCOW2")"
    printf '%-14s = %s  [%s]\n' OWRT_SDK "$OWRT_SDK_DIR" "$(_state "$OWRT_SDK_DIR")"
    printf '%-14s = %s\n' SSH_PORT "$OWRT_SSH_PORT"
    printf '%-14s = %s\n' HTTP_PORT "$OWRT_HTTP_PORT"

    # Etat de la VM (si joignable)
    if vm_exec "echo ok" >/dev/null 2>&1; then
        printf '%-14s = %s\n' VM "en ligne"
        local proto
        proto=$(vm_exec "uci get network.lan.proto" 2>/dev/null || echo "?")
        printf '%-14s = %s\n' NET_PROTO "$proto"
        local root_ok
        root_ok=$(vm_exec "awk -F: '\$1==\"root\" && \$2!=\"\" && \$2!=\"*\" && \$2!=\"!\"{print \"oui\"}' /etc/shadow" 2>/dev/null)
        printf '%-14s = %s\n' ROOT_PASSWD "${root_ok:-non}"
        local users
        users=$(vm_exec "awk -F: '\$3>=1000 && \$3<65534{printf sep \$1; sep=\", \"}' /etc/passwd" 2>/dev/null)
        printf '%-14s = %s\n' COMPTES "${users:-(aucun)}"
    else
        printf '%-14s = %s\n' VM "hors ligne"
    fi
}

# ------------------------------------------------------------ clean ---

cmd_clean() {
    confirm "Supprimer $OWRT_DIR (image + SDK) ?"
    rm -rf "$OWRT_DIR"
    echo "Supprime : $OWRT_DIR"
}

dispatch "$@"
