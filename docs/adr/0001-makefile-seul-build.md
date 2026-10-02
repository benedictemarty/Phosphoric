# 0001 — Le Makefile est le seul système de build

- **Statut** : acceptée (2.1.3, sprint A du plan d'architecture)

## Contexte

Le dépôt avait un `Makefile` et un `CMakeLists.txt`. Le second n'était plus
entretenu : vingt sources y manquaient et l'édition de liens échouait. Deux
systèmes de build divergent toujours ; un seul était réellement utilisé (CI,
tests, distribution Windows, WebAssembly).

## Décision

`CMakeLists.txt` est supprimé ; le `Makefile` est le seul build. Depuis 2.2.0
(sprint B), les objets sont construits hors des sources, un dossier par
configuration (`build/<config>/` : SDL2, HTTPAPI, CAST, MIDI, TLS, TUI, backend
LOCI, DEBUG, COVERAGE, SANITIZE, VIA_NO_LAZY), et les tests lient des objets
(macros `UNIT_TEST` / `DIRECT_TEST`).

## Conséquences

- Pas de génération de projets d'IDE (CMake les produisait).
- Changer d'options ne mélange jamais des objets de configurations différentes
  (le piège « `make SDL2=0` puis `make tests` » disparaît par construction).
- Toute nouvelle source doit être ajoutée au `Makefile` (listes `SOURCES` et,
  si un test la compile directement, sa liste de test).
