# Popeye Issue #1 — Fast-load écrasé par le test RAM

**Date** : 2026-03-04
**Statut** : Corrigé (v1.14.1-alpha)

## Problème signalé

Le mode fast-load (`-f`) injecte les données TAP en RAM **avant** le `cpu_reset()`.
La ROM BASIC 1.0 exécute ensuite un test RAM complet ($FA1F-$FA45, ~2.5M cycles) qui
écrase toute la zone $0000-$BFFF, détruisant le code injecté.

## Cause racine

L'injection immédiate par `memory_write()` dans la boucle de parsing CLI se fait
avant que l'émulation ne démarre. Le `cpu_reset()` au début de `emulator_run()`
réinitialise le PC et la ROM exécute son test RAM qui balaie toute la mémoire basse.

## Correction appliquée

**Approche : Injection différée par seuil de cycles**

1. **`emulator.h`** : Ajout de 4 champs dans `emulator_t` :
   - `fastload_buf` : buffer des données TAP
   - `fastload_addr` : adresse de destination
   - `fastload_size` : taille en octets
   - `fastload_pending` : flag d'injection en attente

2. **`main.c`** (section fast-load) : Au lieu d'injecter immédiatement via
   `memory_write()`, les données sont stockées dans le buffer dédié.

3. **`main.c`** (boucle `emulator_run()`) : Après chaque frame, si
   `total_executed > 3_000_000` et `fastload_pending == true`, les données
   sont injectées en RAM et le buffer est libéré. Le seuil de 3M cycles
   garantit que le test RAM de la ROM (~2.5M cycles) est terminé.

4. **Cleanup** : Le buffer est libéré dans `emulator_cleanup()` en cas
   d'arrêt prématuré.

## Tests ajoutés

- `test_deferred_fastload_fields` : vérification des valeurs initiales
- `test_deferred_fastload_buffer` : vérification du buffering et de l'injection
- `test_deferred_fastload_survives_ram_clear` : le buffer survit à un effacement RAM

## Vérification

```bash
# Compilation et tests
make tests    # 283 tests, 100% pass

# Test manuel
./oric1-emu -r roms/basic10.rom -t prog.tap -f
# Log attendu : "Deferred fast-load: injected N bytes at $XXXX-$YYYY (after ZZZZ cycles)"
```

## Version

- **v1.14.1-alpha** — Sprint 32 bugfix
- Commit : fix: deferred fast-load to survive ROM RAM test (Popeye #1)
