# 0004 — Changer de cartes d'extension relance le processus

- **Statut** : acceptée (2.11.0)

## Contexte

Le menu F1 permet de changer les cartes d'extension (Microdisc, Jasmin, LOCI,
ACIA…). Une carte se branche au démarrage : ROM servie, overlay, périphérique sur
le bus, transport série ouvert, et pour le LOCI co-simulé des threads et un
firmware en cours d'exécution. Réinitialiser tout cela dans le même processus
réutiliserait des états globaux et des ressources de l'hôte.

## Décision

« Appliquer et redémarrer » remplace le processus (`execv` ; `_execv` sous
Windows) par l'émulateur relancé avec la ligne de commande d'origine, sans les
options de cartes, plus celles des cartes choisies et `--no-config-cards`
(`src/cards.c`, `cards_build_argv`, `cards_exec`). Les cartes sont décrites par
un registre de données (`k_cards[]`) que le menu affiche tel quel.

## Conséquences

- C'est un vrai redémarrage à froid : la mémoire de l'Oric est perdue (le menu
  le dit).
- La version web ne peut pas se relancer : ses cartes sont en lecture seule.
- Une carte ajoutée au registre apparaît dans le menu sans autre changement.
