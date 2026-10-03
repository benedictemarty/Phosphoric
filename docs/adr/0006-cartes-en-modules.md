# 0006 — Cartes d'extension en modules auto-enregistrés, liste explicite

- **Statut** : proposée (2026-10-03) ; plan : `docs/specs/CARD_MODULES.md`

## Contexte

Les périphériques d'extension partagent déjà un contrat d'E/S (`io_device_t`) et
un registre pour le menu F1 (`cards.c`), mais restent assemblés à la main : une
carte simple comme MEA8000 touche une dizaine de fichiers (options de lancement,
état dans `emulator_t`, bus, mise en route, audio, menu). Ajouter une carte est
long et sujet aux oublis.

## Décision proposée

Chaque carte est décrite par un descripteur `card_module_t` dans son propre
fichier (menu, options, configuration, mise en route, contrat de bus, rangs de
répartition et de tick, audio). Une liste explicite `k_card_modules[]` les
énumère ; le cœur en dérive options, aide, configuration, menu et tables du bus.

- Pas de chargement dynamique (`dlopen`) : ABI stable, WASM, Windows, confiance.
- Pas d'enregistrement par constructeur ou section du linker : non portable
  entre GCC/MinGW, clang/macOS et emscripten ; une ligne par carte suffit.
- Les ordres de répartition et de tick restent explicites (ADR 0003), et le
  chemin par cycle reste sans boucle générique.

## Conséquences

- Ajouter une carte : un fichier + une ligne (cible, prouvée par un test en G7).
- Migration par étapes (G1 pilote MEA8000 → G6 LOCI), chacune prouvée identique
  (`cli_golden`, `.ost` à l'octet, banc de performance).
- Si le pilote G1 n'apporte pas assez, la décision est rejetée et notée ici.
