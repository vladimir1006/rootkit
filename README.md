# rk2600 — Rootkit LKM Linux

Rootkit sous forme de module noyau (LKM) pour Linux 6.1.188, developpe dans le
cadre du cours de securite des systemes d'exploitation (Ecole 2600, M1).

## Architecture

```
rootkit/
├── include/rk2600_protocol.h   # ABI partagee kernel/userspace (ioctl)
├── module/rk2600.c             # Module noyau (hooks ftrace, VFS, stealth)
├── companion/companion.c       # CLI compagnon (status, privesc, hide, test)
├── infra/scripts/              # Build kernel, rootfs Alpine, image disque
├── docs/USER.md                # Guide utilisateur complet
└── Makefile                    # Cibles principales
```

## Fonctionnalites

| Fonctionnalite | Technique |
|---|---|
| Hook syscalls (read, write, sendfile64) | ftrace + kprobe (`kallsyms_lookup_name`) |
| Module hiding | `list_del_init` sur `THIS_MODULE->list` |
| File hiding (/, /proc, /dev) | Patch `iterate_shared` (CR0 WP bypass) + filldir unifie |
| Privilege escalation | `prepare_kernel_cred(NULL)` + `commit_creds` via ioctl |
| Persistance | Script OpenRC `/etc/local.d/snd-compat.start` |
| Read filter | Suppression des lignes insmod/rk2600 du script de persistance |
| Write stealth | Ecriture simulee (retourne count sans ecrire) |
| Hide/unhide dynamique | Liste chainee protegee par spinlock, via ioctl ou `/proc/.rk` |

## Interfaces de controle

- **ioctl** via `/dev/.rk_ctl` (mode 0666, accessible sans root)
- **proc** via `/proc/.rk` (write-only : `+name`, `-name`, `hide`, `unhide`, `flush`)

## Compilation

```sh
make module          # compile rk2600.ko (cross-compile vers 6.1.188)
make companion       # compile le binaire statique companion
make disk            # image disque Alpine + GRUB
make qcow2           # convertit en QCOW2 compresse (livrable)
```

## Lancement

```sh
make run-disk        # boot GRUB sur l'image disque
```

## Usage rapide dans la VM

```sh
./rk2600 install rk2600.ko    # installe + persistance + charge
./rk2600 status                # API version + module hidden
./rk2600 privesc               # shell root depuis user
./rk2600 hide <name>           # masque une entree de repertoire
./rk2600 test                  # suite de tests automatisee (12 tests)
```

## Comptes

| Compte | Mot de passe |
|---|---|
| `root` | voir `.env` |
| `user` | voir `.env` |
