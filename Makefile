# Makefile — raccourcis vers scripts/. Aucune logique ici.
#
# Variables :
#   PROFILE=base|debug   quel noyau (défaut : base)
#   PAUSE=1              démarrer figé, en attente de GDB
#
# Exemples :
#   make kernel PROFILE=debug
#   make run-nfs PROFILE=debug PAUSE=1

S := ./infra/scripts
PROFILE ?= base
PAUSE_FLAG := $(if $(PAUSE),--pause,)

.DEFAULT_GOAL := help
.PHONY: help status deps kernel kernel-debug config clean-kernel mrproper \
        rootfs busybox initramfs nfsroot nfs-server \
        disk disk-image disk-alpine disk-grub mount umount qcow2 \
        run run-nfs run-disk gdb module companion test-companion test-vm \
        clean-module clean-companion all

help: ## Afficher cette aide
	@echo "Usage : make <cible> [PROFILE=base|debug] [PAUSE=1]"
	@echo
	@grep -E '^[a-z-]+:.*##' $(MAKEFILE_LIST) \
		| sed 's/:.*##/\t/' | expand -t 18
	@echo
	@echo "PROFILE courant : $(PROFILE)"

status: ## État du workspace (fichiers présents ou non)
	@$(S)/common.sh

# ------------------------------------------------------------------ noyau ---

deps: ## Installer les paquets de compilation
	@$(S)/build_kernel.sh deps

kernel: ## Compiler le noyau (fetch + config + compile)
	@$(S)/build_kernel.sh all $(PROFILE)

kernel-debug: ## Compiler le noyau avec les symboles de debug
	@$(S)/build_kernel.sh all debug

config: ## Écrire seulement la config du profil
	@$(S)/build_kernel.sh config $(PROFILE)

clean-kernel: ## Supprimer le dossier de build du profil
	@$(S)/build_kernel.sh clean $(PROFILE)

mrproper: ## Nettoyer les sources (une fois, avant d'utiliser les builds séparés)
	@$(S)/build_kernel.sh mrproper

# ----------------------------------------------------------------- rootfs ---

rootfs: ## busybox + initramfs + nfsroot
	@$(S)/build_rootfs.sh all

busybox: ## Compiler busybox (statique, sans tc)
	@$(S)/build_rootfs.sh busybox

initramfs: ## Reconstruire initramfs.cpio.gz
	@$(S)/build_rootfs.sh initramfs

nfsroot: ## Construire le rootfs NFS
	@$(S)/build_rootfs.sh nfsroot

nfs-server: ## Exporter nfsroot/ et activer NFS (hôte)
	@$(S)/build_rootfs.sh nfs-server

# ------------------------------------------------------------ image disque --

disk: kernel module companion ## Image LFS du TP01 avec rootkit persistant et GRUB
	@$(S)/build_disk.sh all $(PROFILE)

disk-image: ## Créer et formater disk.img
	@$(S)/build_disk.sh image

disk-alpine: ## (Re)peupler une image existante avec Alpine
	@$(S)/build_disk.sh alpine

disk-grub: ## Installer le noyau du profil et GRUB dans l'image
	@$(S)/build_disk.sh grub $(PROFILE)

qcow2: ## Convertir disk.img en QCOW2 compressé (livrable)
	@test -f infra/disk.img || { echo "Erreur : infra/disk.img absent, lancez 'make disk' d'abord"; exit 1; }
	qemu-img convert -c -O qcow2 infra/disk.img rk2600.qcow2
	@echo "Image : rk2600.qcow2 ($$(du -sh rk2600.qcow2 | cut -f1))"

mount: ## Monter disk.img pour la modifier à la main
	@$(S)/build_disk.sh mount

umount: ## Démonter disk.img
	@$(S)/build_disk.sh umount

# -------------------------------------------------------------- module -----

module: ## Compiler le module noyau rk2600.ko
	@$(MAKE) -C module KDIR=$(CURDIR)/infra/build

companion: ## Compiler l'outil companion rk2600
	@$(MAKE) -C companion

test-companion: companion ## Tests unitaires et CLI du compagnon
	@$(MAKE) -C companion test

test-vm: ## Démarrer rk2600.qcow2 et exécuter la suite fonctionnelle
	@test -f rk2600.qcow2 || { echo "Erreur : rk2600.qcow2 absent"; exit 1; }
	@command -v expect >/dev/null || { echo "Erreur : expect absent"; exit 1; }
	@expect $(S)/test_vm.exp

clean-module: ## Nettoyer le module noyau
	@$(MAKE) -C module KDIR=$(CURDIR)/infra/build clean

clean-companion: ## Nettoyer le companion
	@$(MAKE) -C companion clean

# ---------------------------------------------------------------- QEMU -----

run: ## Démarrer sur initramfs
	@$(S)/run.sh initramfs $(PROFILE) $(PAUSE_FLAG)

run-nfs: ## Démarrer sur le rootfs NFS
	@$(S)/run.sh nfs $(PROFILE) $(PAUSE_FLAG)

run-disk: ## Démarrer sur l'image disque (GRUB)
	@$(S)/run.sh disk $(PAUSE_FLAG)

gdb: ## Lancer GDB et se connecter à la VM en attente
	@$(S)/run.sh gdb

all: kernel rootfs module companion ## Tout compiler (noyau + rootfs + module + companion)
