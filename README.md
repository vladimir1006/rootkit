# rk2600 — Rootkit LKM

Projet de rootkit Linux (module noyau) pour le cours Noyau Linux, Ecole 2600.

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
make owrt-run              # terminal 1

# 2. Configurer les comptes (terminal 2)
make owrt-setup-users

# 3. Compiler le compagnon
make companion
make test                  # tests CLI

# 4. Envoyer dans la VM
make owrt-push F=companion/companion

# 5. Livraison : image QCOW2 compressee
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
- SSH : `ssh root@localhost -p 2222`
- Comptes : root / root, user / user (configurable)
- Quitter QEMU : `Ctrl-A` puis `X`

## A terminer

1. Ecrire le module noyau (`module/`) : syscall hooking, stealth (getdents/read/write), persistance
2. Creer `/dev/rk2600` au chargement du module (misc_register)
3. Tester le compagnon avec le module dans la VM
4. Persistence : script init pour charger le .ko au boot
5. Documentation et rapport de conception
