# Programme compagnon

Le compagnon est le client en espace utilisateur du projet. Il valide la
commande, ouvre `/dev/rk2600` et envoie une requête ioctl au module. Il ne crée
pas le périphérique et ne remplace pas le module noyau.

## Commandes

La syntaxe complète s'affiche avec `./companion --help`. Les commandes sont :

- `status` lit la version de l'API et vérifie qu'elle vaut `RK2600_API_VERSION`.
- `elevate` transmet une demande d'élévation au module.
- `hide NAME` et `unhide NAME` transmettent au module une entrée de répertoire.

Le périphérique par défaut est `/dev/rk2600`. Placez `--device PATH` avant la
commande pour en choisir un autre. `NAME` doit contenir de 1 à 255 octets. Le
compagnon refuse les noms vides, les chemins contenant `/`, `.` et `..`.

## Compiler et tester

Depuis la racine du dépôt, lancez :

```sh
make -C companion
make -C companion STRICT=1
make -C companion STRICT=1 test
make -C companion clean
```

`STRICT=1` ajoute `-Werror`. `test` vérifie l'aide, les arguments invalides,
l'erreur de périphérique absent et les limites de longueur des noms.

## État et limites

Le client, le protocole ioctl, le Makefile, les tests CLI et ce guide sont en
place. `make -C companion STRICT=1 test` passe sous WSL avec Ubuntu.

Le dépôt ne contient pas encore le module noyau. Les tests actuels ne valident
donc pas une requête ioctl réussie, les permissions du périphérique, une version
d'API incompatible ni le comportement du module. Le code retourne 1 si
l'ouverture, un ioctl, la vérification de version ou la fermeture échoue. Il
retourne 2 si la commande ou ses arguments sont invalides.

## Travail restant

1. Valider avec la personne qui écrit le module le chemin `/dev/rk2600`, les
   numéros ioctl, les structures et `RK2600_API_VERSION`.
2. Créer le périphérique dans la VM et régler ses permissions pour le compte
   utilisateur prévu par le projet.
3. Tester chaque commande dans la VM avec le module chargé, puis ajouter des
   vérifications des réponses ioctl et des erreurs de permission.
4. Mettre à jour ce guide si le protocole ou le comportement du module change.
