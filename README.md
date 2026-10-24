# Projet rootkit Linux

Projet de laboratoire en VM QEMU. Le dépôt contient des scripts de compilation
du noyau, des rootfs BusyBox, une image disque Alpine avec GRUB, un protocole
ioctl partagé et un programme compagnon.

## État actuel

- `companion/` contient la CLI, son Makefile, les tests d'arguments et le guide utilisateur. Le client envoie ses requêtes à `/dev/rk2600`.
- `include/rk2600_protocol.h` définit les commandes ioctl et les structures partagées avec le module.
- `infra/scripts/` contient les scripts de compilation du noyau, de BusyBox, des rootfs et de l'image disque.
- L'image disque générée utilise Alpine et le format raw. Les fichiers du module noyau ne sont pas encore présents dans ce dépôt.
- La construction de l'image Alpine exige `ROOT_PASSWORD`, fourni dans l'environnement.

Pour compiler et tester le compagnon sous WSL, lancez `make -C companion STRICT=1 test`.

## À terminer pour le rendu

1. Construire la véritable LFS demandée, avec les comptes `root` et `user`.
2. Écrire et intégrer le module noyau. Confirmer avec son auteur les numéros ioctl, les structures et la version définis dans `include/rk2600_protocol.h`.
3. Créer `/dev/rk2600`, régler ses permissions, puis tester le compagnon avec le module chargé dans la VM. Les tests actuels ne vérifient pas les réponses réelles du module.
4. Convertir le disque raw en QCOW2 compressé avec `qemu-img convert -c -O qcow2 disk.img disk.qcow2`, puis vérifier que l'image démarre dans QEMU.
5. Compléter la documentation utilisateur et le rapport de conception avec la répartition des tâches.

Le Makefile racine appelle les scripts de `infra/scripts/`.
