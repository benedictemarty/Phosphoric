# Décisions d'architecture (ADR)

Une fiche par décision structurante : le contexte, ce qui a été décidé, ce que
cela coûte. Une décision remplacée n'est pas effacée : sa fiche passe au statut
« remplacée » et renvoie à la suivante.

| N° | Décision | Statut |
|----|----------|--------|
| [0001](0001-makefile-seul-build.md) | Le Makefile est le seul système de build | acceptée (2.1.3) |
| [0002](0002-file-de-commandes.md) | Les commandes externes passent par une file vidée par la boucle de l'émulateur | acceptée (1.52) |
| [0003](0003-ordre-des-ticks.md) | Ordre des ticks par cycle explicite, distinct de l'ordre de répartition des E/S | acceptée (2.4.0) |
| [0004](0004-cartes-par-relance.md) | Changer de cartes d'extension relance le processus | acceptée (2.11.0) |
| [0005](0005-miroirs-fr-en.md) | Deux branches miroir : français et anglais, qui ne diffèrent que par les commentaires | acceptée (2.1.4) |
