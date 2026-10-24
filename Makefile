# Makefile — projet rootkit rk2600 (Ecole 2600).
#
# Cibles principales :
#   make companion         compiler le programme compagnon
#   make test              tester le compagnon (CLI)
#   make owrt-run          demarrer la VM OpenWrt
#   make owrt-init         configurer DHCP + cle SSH (1 fois)
#   make owrt-setup-user  creer le compte utilisateur
#   make owrt-qcow2        image QCOW2 compressee (livraison)
#
# Variables :
#   STRICT=1   active -Werror sur le compagnon
#   PAUSE=1    demarrer la VM figee (GDB)

S := ./scripts
PAUSE_FLAG := $(if $(PAUSE),--pause,)

.DEFAULT_GOAL := help
.PHONY: help companion companion-static test clean-companion \
        owrt-fetch owrt-resize owrt-sdk owrt-run owrt-ssh \
        owrt-module owrt-push owrt-pull owrt-init \
        owrt-setup-user owrt-qcow2 owrt-status owrt-clean

help: ## Afficher cette aide
	@echo "Usage : make <cible> [STRICT=1] [PAUSE=1]"
	@echo
	@grep -E '^[a-z0-9-]+:.*##' $(MAKEFILE_LIST) \
		| sed 's/:.*##/\t/' | expand -t 22
	@echo

# ----------------------------------------------------------- companion ---

companion: ## Compiler le programme compagnon
	@$(MAKE) -C companion $(if $(STRICT),STRICT=1,)

companion-static: ## Compiler le compagnon en statique (pour la VM OpenWrt)
	@$(MAKE) -C companion static $(if $(STRICT),STRICT=1,)

test: companion ## Tester le compagnon (CLI + limites)
	@$(MAKE) -C companion STRICT=1 test

clean-companion: ## Nettoyer le binaire compagnon
	@$(MAKE) -C companion clean

# ------------------------------------------------------------ OpenWrt -----

owrt-fetch: ## Telecharger l'image OpenWrt x86-64
	@$(S)/openwrt.sh fetch

owrt-resize: ## Agrandir l'image OpenWrt (defaut 512M)
	@$(S)/openwrt.sh resize

owrt-sdk: ## Telecharger le SDK OpenWrt (compilation de modules)
	@$(S)/openwrt.sh sdk

owrt-run: ## Demarrer OpenWrt dans QEMU (SSH :2222, HTTP :8080)
	@$(S)/openwrt.sh run $(PAUSE_FLAG)

owrt-ssh: ## Se connecter en SSH a la VM OpenWrt
	@$(S)/openwrt.sh ssh

owrt-module: ## Creer un template de module kernel OpenWrt
	@$(S)/openwrt.sh module-template

owrt-init: ## Configurer DHCP + cle SSH (a faire une fois apres passwd)
	@$(S)/openwrt.sh init

owrt-push: ## Envoyer un fichier dans la VM : make owrt-push F=<fichier>
	@$(S)/openwrt.sh push $(F)

owrt-pull: ## Recuperer un fichier de la VM : make owrt-pull F=<fichier>
	@$(S)/openwrt.sh pull $(F)

owrt-setup-user: ## Creer un compte : make owrt-setup-user [U=nom P=pass]
	@$(S)/openwrt.sh setup-user $(U) $(P)

owrt-qcow2: ## Convertir l'image en QCOW2 compresse (livraison)
	@$(S)/openwrt.sh qcow2

owrt-status: ## Etat du workspace OpenWrt
	@$(S)/openwrt.sh status

owrt-clean: ## Supprimer image + SDK OpenWrt
	@$(S)/openwrt.sh clean
