# Architecture decision records (ADR)

One file per structuring decision: the context, what was decided, what it costs.
A superseded decision is not deleted: its record is marked "superseded" and
points to the next one.

| N° | Décision | Statut |
|----|----------|--------|
| [0001](0001-makefile-seul-build.md) | Le Makefile est le seul système de build | acceptée (2.1.3) |
| [0002](0002-file-de-commandes.md) | Les commandes externes passent par une file vidée par la boucle de l'émulateur | acceptée (1.52) |
| [0003](0003-ordre-des-ticks.md) | Ordre des ticks par cycle explicite, distinct de l'ordre de répartition des E/S | acceptée (2.4.0) |
| [0004](0004-cartes-par-relance.md) | Changer de cartes d'extension relance le processus | acceptée (2.11.0) |
| [0005](0005-miroirs-fr-en.md) | Deux branches miroir : français et anglais, qui ne diffèrent que par les commentaires | acceptée (2.1.4) |
| [0006](0006-cartes-en-modules.md) | Cartes d'extension en modules auto-enregistrés, liste explicite | proposée |
