# 0002 — Les commandes externes passent par une file vidée par la boucle de l'émulateur

- **Statut** : acceptée (sprints 92-95, 1.51 → 1.54)

## Contexte

L'émulateur exécute CPU, VIA et vidéo dans un seul thread, une trame après
l'autre. Les commandes externes — `--control` (stdin), API HTTP (un thread
serveur), menu F1 — modifient l'état de la machine (reset, chargement de
disquette, sauvegarde d'état…). Exécutées depuis un autre thread, en plein
milieu d'une instruction, elles corrompraient cet état.

## Décision

Un seul interpréteur, `control_dispatch()` (`src/control.c`), pour toutes les
sources de commandes. Les producteurs d'un autre thread (serveur HTTP) passent
par `control_queue` (`include/control_queue.h`) : `control_queue_submit()`
enfile une ligne et attend la réponse ; la boucle de l'émulateur appelle
`control_queue_drain()` une fois par trame et exécute chaque commande sur son
propre thread, à une frontière de trame.

## Conséquences

- Une commande HTTP attend au plus une trame (20 ms) plus son exécution.
- Un seul consommateur : la boucle de l'émulateur. Quand elle s'arrête (CPU
  bloqué, limite de cycles, signal), la file est fermée **avant** l'arrêt du
  serveur (`control_queue_shutdown`) : les producteurs en attente sont libérés,
  sinon l'attente de fin du thread serveur ne se terminerait jamais (défaut
  corrigé en 2.12.1).
- Les commandes sont découpées par thème depuis 2.12.0 (`control_cmd_*.c`) ;
  `control_dispatch()` reste l'unique point d'entrée.
