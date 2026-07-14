# API REST de commande — Phosphoric

> Statut : **initiative TERMINÉE — Epics 1-4 livrés** (Sprints 92-95,
> v1.54.0-alpha). L'API pilote entièrement Phosphoric via HTTP/JSON
> (`make HTTPAPI=1`, option `--http-api`).
>
> Démarrage rapide :
> ```bash
> make HTTPAPI=1                       # (ajouter SDL2=1 pour la GUI)
> ./oric1-emu -r roms/basic11b.rom --http-api=8888 --http-api-root ./disks
> curl -s localhost:8888/regs
> curl -s -X POST --data 'path=game.tap' localhost:8888/tape
> curl -s -X POST localhost:8888/reset
> # Taper (et exécuter) une ligne BASIC à distance — \n = RETURN :
> curl -s -X POST --data-urlencode 'text=PRINT 2+2\n' localhost:8888/keys
> ```

## 1. Vision

Exposer le pilotage de Phosphoric (média, état CPU/mémoire, exécution) via
HTTP/JSON sur un port dédié, pour le scripting distant, les dashboards
navigateur et les tests e2e — **sans dupliquer** la logique du protocole
`--control` existant.

## 2. Contexte : ce qui existe déjà

Phosphoric dispose déjà de trois surfaces de contrôle, mais aucune n'est une
API REST « classique » :

| Surface       | Transport            | Usage                                   |
|---------------|----------------------|-----------------------------------------|
| `--control`   | stdin/stdout (texte) | 30 commandes (OricForge IDE)            |
| Cast server   | HTTP `:8080`         | Streaming MJPEG + audio + snapshot      |
| GDB stub      | TCP `:1234`          | Debug (protocole RSP)                   |

Le protocole `--control` (`src/control.c`) contient déjà les *handlers*
métier réutilisables (`cmd_load_tap`, `cmd_reset`, `cmd_load_disk`,
`cmd_peek`…). L'API REST **ne réécrit pas** cette logique : elle ajoute un
transport HTTP branché sur le même dispatch.

## 3. Contrainte architecturale directrice

L'émulateur est **single-thread** : la boucle principale exécute
`CYCLES_PER_FRAME` (19968) cycles par frame dans `src/main.c`. Toute mutation
d'état (`savestate_load`, `microdisc_set_disk`, `cpu_reset`) doit s'exécuter
**aux frontières de frame**, jamais depuis un thread HTTP en plein milieu
d'une instruction. C'est le risque technique n°1 ; il est isolé dans l'Epic 2
(file de commandes thread-safe).

## 4. Backlog

### EPIC 1 — Découpler le dispatch de son transport *(Sprint 92, en cours)*

Fondation réutilisable même si l'API REST est abandonnée ensuite.

- **US 1.1** — `control_sink_t` : abstraction de sortie (`ok`/`err`/`raw`),
  deux implémentations : `stream` (stdout, comportement actuel byte-identique)
  et `buffer` (accumulation mémoire pour HTTP). Les handlers `cmd_*` écrivent
  dans un sink au lieu de `stdout`.
- **US 1.2** — Extraire `control_dispatch(emu, sink, line)` du grand `if/else`
  de `control_repl()`. Retour `control_result_t`
  (`CONTINUE`/`RESUME`/`QUIT`). `control_repl` devient une boucle mince.
- **DoD** : `make test-control` reste vert à l'octet près + nouveau test
  unitaire du dispatch via sink buffer.

### EPIC 2 — File de commandes thread-safe *(Sprint 93)*

- **US 2.1** — File mono-producteur (thread HTTP) / mono-consommateur (boucle
  émulateur), protégée par mutex + condvar, drainée une fois par frame dans
  `main.c` (à côté de `control_poll_pause`).
- **DoD** : `test-httpapi` — 100 commandes concurrentes, zéro corruption,
  valgrind propre.

### EPIC 3 — Serveur HTTP minimal *(Sprint 94)*

- **US 3.1** — Serveur HTTP réutilisant le pattern socket/`select` de
  `src/network/cast_server.c` (aucune dépendance externe). Option
  `--http-api=PORT`, binding **`127.0.0.1` par défaut**, exposition réseau
  opt-in explicite (`--http-api-bind`). Flag Makefile `HTTPAPI ?= 0` sur le
  modèle de `CAST=1`.
- **US 3.2** — Endpoints (mappés sur les handlers existants) :

  | Méthode  | Route                              | Handler                       |
  |----------|------------------------------------|-------------------------------|
  | `GET`    | `/hello`                           | `cmd_hello`                   |
  | `GET`    | `/regs`                            | `cmd_regs`                    |
  | `GET`    | `/mem?addr=&len=`                  | `cmd_read`                    |
  | `POST`   | `/mem`                             | `cmd_write`                   |
  | `POST`   | `/reset`                           | `cmd_reset`                   |
  | `POST`   | `/tape` `{path}` / `DELETE /tape`  | `cmd_load_tap` / `cmd_eject_tape` |
  | `POST`   | `/disk/{A-D}` / `DELETE`           | `cmd_load_disk` / `cmd_eject_disk` |
  | `GET`    | `/peek/{via\|psg\|disk\|acia\|tape\|loci}` | `cmd_peek`            |
  | `POST`   | `/exec/{step\|next\|step-out\|continue\|pause}` | dispatch exécution |
  | `POST`   | `/keys` `{text}`                   | `cmd_keys` (Epic 4)           |

  Réponses en **JSON** (`{"ok":true,"reply":…}` / `{"ok":false,"error":…}`),
  en-tête CORS. *(Livré : le mapping se fait vers une ligne `--control` passée
  à `control_queue_submit`, pas un appel direct au handler.)*
- **US 3.3** — Sécurité : chemins de fichiers (`/tape`, `/disk`) restreints à
  un répertoire autorisé (`--http-api-root`), refus des chemins absolus et de
  `..`.

### EPIC 4 — Injection clavier & documentation *(Sprint 95)* — LIVRÉ

- **US 4.1** ✅ — `POST /keys {text}` → commande `keys` → buffer d'injection
  dynamique (`kbd_inject_*`), consommé touche par touche par la boucle
  (`feed_kbd_inject`, press/hold/release ~5 frames/touche) sur le thread
  émulateur. Échappements : `\n`/`\r` → RETURN, `\t`, `\e`, `\\`.
- **US 4.2** ✅ — README + `docs/http-api.md` + exemples `curl` ; CHANGELOG /
  VERSION_TRACKING / CIRRUS_OS / ROADMAP à jour.

## 5. Estimation

~600-900 LOC au total (sink+dispatch ~200, file ~120, serveur HTTP+routing
~300, sécurité chemins ~80, tests ~200). Ordre de grandeur comparable au
module cast.
