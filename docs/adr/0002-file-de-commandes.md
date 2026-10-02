# 0002 — External commands go through a queue drained by the emulator loop

- **Status**: accepted (sprints 92-95, 1.51 → 1.54)

## Context

The emulator runs CPU, VIA and video in a single thread, one frame after the
other. External commands — `--control` (stdin), HTTP API (a server thread), F1
menu — change the machine state (reset, loading a floppy, saving a state…).
Executed from another thread, in the middle of an instruction, they would
corrupt that state.

## Decision

A single interpreter, `control_dispatch()` (`src/control.c`), for every command
source. Producers on another thread (HTTP server) go through `control_queue`
(`include/control_queue.h`): `control_queue_submit()` enqueues a line and waits
for the reply; the emulator loop calls `control_queue_drain()` once per frame and
runs each command on its own thread, at a frame boundary.

## Consequences

- Une commande HTTP attend au plus une trame (20 ms) plus son exécution.
- Un seul consommateur : la boucle de l'émulateur. Quand elle s'arrête (CPU
  bloqué, limite de cycles, signal), la file est fermée **avant** l'arrêt du
  serveur (`control_queue_shutdown`) : les producteurs en attente sont libérés,
  sinon l'attente de fin du thread serveur ne se terminerait jamais (défaut
  corrigé en 2.12.1).
- Les commandes sont découpées par thème depuis 2.12.0 (`control_cmd_*.c`) ;
  `control_dispatch()` reste l'unique point d'entrée.
