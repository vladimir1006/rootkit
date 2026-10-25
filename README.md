# rk2600 — Rootkit LKM

Projet de rootkit Linux (module noyau) pour le cours Noyau Linux, Ecole 2600.

⚠️ **Cette branche (`feat/openwrt-infra`) ne doit pas être utilisée pour le rendu final.**

Le sujet demande une image QCOW2 contenant un LFS (Linux From Scratch) construit à partir des scripts, dans la continuité du TP01. OpenWrt est une distribution pré-compilée, pas un système construit from scratch — ça ne répond pas à ce critère.

L'infra reste utile comme référence (workflow module, SDK, provisioning VM), mais le rootkit sera développé et livré sur une vraie base LFS.

## Architecture

```
rootkit/
├── companion/           programme compagnon (userspace CLI)
│   ├── companion.c      client ioctl -> /dev/rk2600
│   ├── Makefile
│   └── tests/           tests CLI et limites
├── include/
│   └── rk2600_protocol.h   ABI partagee kernel/userspace (ioctl)
├── module/              module noyau (.ko) — a ecrire
├── scripts/
│   ├── common.sh        variables et fonctions partagees
│   └── openwrt.sh       setup et gestion de la VM OpenWrt
└── Makefile             cibles racine (companion + OpenWrt)
```

## Quickstart

```sh
# 1. Telecharger et preparer la VM OpenWrt
make owrt-fetch
make owrt-resize
make owrt-run              # terminal 1 — quitter : Ctrl-A puis X

# 2. Dans la console QEMU : definir le mot de passe root
passwd

# 3. Initialiser la VM (terminal 2) — DHCP + cle SSH
make owrt-init

# 4. Creer un compte utilisateur (optionnel)
make owrt-setup-user                    # user/user par defaut
make owrt-setup-user U=myuser P=pass    # compte custom

# 5. Compiler le compagnon (statique pour la VM musl/OpenWrt)
make companion-static
make test
make owrt-push F=companion/companion

# 6. Livraison : image QCOW2 compressee
make owrt-qcow2
```

## Communication rootkit <-> compagnon

Le compagnon parle au module via ioctl sur `/dev/rk2600` :

| Commande | ioctl | Description |
|----------|-------|-------------|
| `status` | `RK2600_IOC_STATUS` | Lit la version de l'API du module |
| `elevate` | `RK2600_IOC_ELEVATE` | Demande une elevation de privileges |
| `hide NAME` | `RK2600_IOC_HIDE` | Cache une entree de repertoire |
| `unhide NAME` | `RK2600_IOC_UNHIDE` | Revele une entree cachee |

Le protocole est defini dans `include/rk2600_protocol.h`.

## VM OpenWrt

- Image : OpenWrt 23.05.6 x86-64 (ext4, modules chargeables sans signature)
- Libc : **musl** (pas glibc) — les binaires doivent etre compiles en statique (`make companion-static`) ou cross-compiles avec le SDK OpenWrt
- Reseau : DHCP via QEMU usernet (configure par `make owrt-init`)
- SSH : `ssh root@localhost -p 2222` (cle SSH installee par `owrt-init`)
- SCP : protocole legacy (`scp -O`) — OpenWrt n'a pas de sftp-server
- Comptes : user/user par defaut (`make owrt-setup-user U=nom P=pass`)
- Quitter QEMU : `Ctrl-A` puis `X`

## Pourquoi OpenWrt comme cible ?

On a choisi OpenWrt x86-64 comme environnement de déploiement du rootkit. Ce choix s'inscrit dans un scénario d'attaque réaliste sur un équipement IoT/réseau :

**Pertinence sécurité :**
- OpenWrt tourne sur des millions de routeurs et points d'accès en production
- Kernel Linux standard (5.15) avec musl libc et BusyBox — surface d'attaque réduite mais pas de mécanismes de protection avancés
- Pas de Secure Boot ni de vérification de signature des modules kernel → un attaquant avec un accès root (exploit RCE, firmware supply chain, credentials par défaut) peut charger un LKM sans obstacle
- C'est exactement le type de cible documenté dans les rapports ANSSI sur les menaces IoT

**Avantages pratiques :**
- Image légère (~120 Mo) manipulable sous QEMU avec port forwarding SSH
- SDK officiel pour cross-compiler les modules sans rebuilder le système


**Infra mise en place (branche `feat/openwrt-infra`) :**
- `make owrt-fetch/resize/sdk` — téléchargement et préparation de l'environnement
- `make owrt-compile/insmod/rmmod M=<nom>` — workflow complet de dev module
- `make owrt-init/setup-user` — provisioning automatisé de la VM
- `make owrt-module M=<nom>` — génération de template pour nouveaux modules
