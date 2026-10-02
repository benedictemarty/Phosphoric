# 0003 — Ordre des ticks par cycle explicite, distinct de l'ordre de répartition des E/S

- **Statut** : acceptée (2.4.0, sprint D du plan d'architecture)

## Contexte

À chaque cycle CPU, les périphériques avancent d'un pas (`cpu_cycle_tick`,
`src/main.c`) : VIA d'abord (il pilote les timers et l'interruption), puis la
cassette, puis les périphériques du bus d'E/S. La table `io_bus[]`
(`src/io/io_bus.c`) donne, elle, l'ordre de **répartition** des accès
($0300-$03FF) : quel périphérique répond à une adresse. Ces deux ordres
diffèrent historiquement, et l'ordre des ticks a des effets observables.

## Décision

L'ordre des ticks est une table à part, `io_bus_tick_order[]` (Microdisc,
Jasmin, LOCI, ACIA, DTL 2000, Mageco, SP0256, MEA8000), précédée par VIA et
cassette dans `cpu_cycle_tick`. `io_bus_tick()` la parcourt, boucle déroulée
avec `__builtin_expect` (une boucle générique coûtait +22 % par trame).

## Conséquences

- Unifier les deux ordres demanderait d'abord de prouver l'équivalence octet par
  octet (corpus, sauvegardes d'état, `cli_golden`).
- Un nouveau périphérique temporisé s'ajoute à `io_bus_tick_order[]`, à la place
  qui conserve le comportement mesuré.
