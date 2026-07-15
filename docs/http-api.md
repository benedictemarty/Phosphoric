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
  | `GET`    | `/mem?addr=&len=[&bank=cpu\|ram\|rom\|overlay]` | `cmd_read`       |
  | `POST`   | `/mem`                             | `cmd_write`                   |
  | `POST`   | `/reset`                           | `cmd_reset`                   |
  | `POST`   | `/tape` `{path}` / `DELETE /tape`  | `cmd_load_tap` / `cmd_eject_tape` |
  | `POST`   | `/disk/{A-D}` / `DELETE`           | `cmd_load_disk` / `cmd_eject_disk` |
  | `GET`    | `/peek/{via\|psg\|disk\|acia\|tape\|loci\|video\|kbd\|joy\|printer}` | `cmd_peek` |
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

### EPIC 5 — Parité debug (bridge `--control` + 7 gaps) *(Sprint 97)* — LIVRÉ

Le debug interactif (dont les 7 gaps du Sprint 96) est rendu pilotable à
distance. Même principe : **chaque route mappe une ligne `--control`.**

| Méthode  | Route                                   | Commande `--control`        |
|----------|-----------------------------------------|-----------------------------|
| `GET`    | `/break`                                | `break-list`                |
| `POST`   | `/break` `{addr[,if]}`                  | `break <addr> [if <expr>]`  |
| `DELETE` | `/break/{id}`                           | `unbreak <id>`              |
| `GET`    | `/watch`                                | `watch-list`                |
| `POST`   | `/watch` `{addr[,mode=w\|r\|a\|c]}`     | `watch <addr> [mode]`       |
| `DELETE` | `/watch/{id}`                           | `unwatch <id>`              |
| `POST`   | `/raster` `{line}` / `DELETE /raster/{id}` | `raster` / `unraster`    |
| `GET`    | `/disasm?addr=&n=`                      | `disasm <addr> <n>`         |
| `POST`   | `/set` `{reg,val}` ou `{via,val}`       | `set <reg> <val>` / `set via` |
| `POST`   | `/hunt` `{[op[,val]]}`                  | `hunt [op] [val]`           |
| `POST`   | `/save` `{path,addr,len}`               | `save-mem` (sandbox)        |
| `POST`   | `/load` `{path,addr}`                   | `load-mem` (sandbox)        |
| `POST`   | `/state/save` `{path}` / `/state/load`  | `state-save` / `state-load` |
| `POST`   | `/sym` `{path}`                         | `load-sym` (sandbox)        |

- **Watch modes** : `w` write, `r` read, `a` access, `c` change.
- **Breakpoints conditionnels** : l'expression (`A==5 && M[$C000]>10`) est
  URL-encodée par le client et décodée par `get_param` (`&&` → `%26%26`).
- **`hunt`** (cheat-finder) : `op ∈ {eq <val>, same, changed, up, down, list,
  clear}` ; `POST /hunt` sans `op` amorce sur tout l'espace d'adressage.
- **Fichiers** : `/save` `/load` `/state/*` `/sym` sont **sandboxés** dans
  `--http-api-root` (`..` et chemins absolus rejetés → 403).
- **Littéraux `%` binaires** acceptés dans tous les paramètres numériques.
- Caps `hello` étendues : `watch-mode,break-cond,hunt,save-mem,load-mem,
  state-save,state-load,set-via,bin-literal` (extension additive,
  `CONTROL_PROTO_VERSION` inchangé).

Exemples :

```bash
curl -s -X POST --data 'addr=C000&mode=r'                localhost:8888/watch
curl -s -X POST --data-urlencode 'addr=0500' \
                --data-urlencode 'if=A==5 && X==3'       localhost:8888/break
curl -s -X POST                                          localhost:8888/hunt        # seed
curl -s -X POST --data 'op=eq&val=7B'                    localhost:8888/hunt        # narrow
curl -s -X POST --data 'path=snap.ost'                   localhost:8888/state/save
curl -s -X POST --data 'via=2&val=FF'                    localhost:8888/set
```

### EPIC 6 / US 2 — Inspection mémoire banque-aware *(Sprint 98)* — LIVRÉ

`GET /mem` accepte un paramètre **`bank`** pour lire une couche mémoire précise
**sous** l'overlay ORIC $C000-$FFFF, sans dépendre du paging courant (parité
avec les paging overrides de b2) :

| `bank`    | Couche lue |
|-----------|-----------|
| `cpu` *(défaut)* | vue CPU courante (ce qui est paginé) |
| `ram`     | RAM sous-jacente (`upper_ram` = RAM derrière la ROM ≥ $C000) |
| `rom`     | ROM BASIC/moniteur ($C000-$FFFF) |
| `overlay` | ROM overlay Microdisc ($E000-$FFFF) |

Même paramètre côté `--control` (`read <addr> <len> [bank]`) et REPL
(`m addr [len] [bank]`). Cap `mem-bank`.

```bash
curl -s "localhost:8888/mem?addr=C000&len=1&bank=rom"   # 1er octet ROM BASIC
curl -s "localhost:8888/mem?addr=C000&len=1&bank=ram"   # RAM cachée derrière
```

### EPIC 6 / US 1 — Tracing conditionnel *(Sprint 99)* — LIVRÉ

Le tracing CPU (`--trace`) devient **conditionnel** et **borné** (parité avec
les déclencheurs et le buffer circulaire de b2).

| Méthode  | Route             | Effet |
|----------|-------------------|-------|
| `POST`   | `/trace` `{spec}` | arme la trace (`trace start <spec>`) |
| `GET`    | `/trace`          | statut (`active/armed/count/ring`) |
| `POST`   | `/trace/stop`     | arrête l'enregistrement (garde le ring) |
| `POST`   | `/trace/save` `{path}` | écrit le ring dans un fichier (sandbox) |
| `DELETE` | `/trace`          | désarme + libère le ring |

**Spec** (`spec=`, tokens séparés par espaces) :
`now` | `pc:HEX` — départ ; `stop:cycle:N` | `stop:brk` | `stop:write:HEX` |
`stop:read:HEX` — arrêt ; `ring:N` — buffer circulaire ; `sym` — symboles inline.

Même syntaxe côté `--control` (`trace start <spec>`, `trace stop|save|status|
off`) et REPL. Cap `trace-cond`. Le triggers write/read s'appuient sur un second
hook mémoire (`trace_callback2`), indépendant des watchpoints.

```bash
# tracer 500 instr autour d'une routine, avec symboles, puis récupérer le ring
curl -s -X POST --data-urlencode 'spec=pc:E000 stop:brk ring:500 sym' localhost:8888/trace
curl -s -X POST --data 'path=run.log'  localhost:8888/trace/save
```

### EPIC 6 / US 3 — Carte d'accès mémoire r/w/x *(Sprint 100)* — LIVRÉ

Marque des **régions arbitraires** avec des flags read/write/execute, sans la
limite des 8 watchpoints fixes (parité avec les flags par octet de b2).

| Méthode  | Route             | Effet |
|----------|-------------------|-------|
| `POST`   | `/watch-region` `{start,end[,flags]}` | flag la région (défaut `rw`) |
| `GET`    | `/watch-region`   | liste les runs flaggés |
| `DELETE` | `/watch-region`   | efface toute la carte |

`flags` = sous-ensemble de `rwx`. Read/write déclenchent via le callback de
trace mémoire (mutualisé avec les watchpoints) ; execute est testé avant chaque
instruction. Même chose côté `--control` (`watch-region <start> <end> [flags]`,
`watch-region-list`, `watch-region-clear`) et REPL (`wr START END [rwx]`). Cap
`access-map`.

```bash
curl -s -X POST --data 'start=2000&end=2010&flags=rw' localhost:8888/watch-region
curl -s "localhost:8888/watch-region"     # liste
```

### EPIC 6 / US 5 — Couverture d'inspection élargie *(Sprint 101)* — LIVRÉ

`GET /peek/{sub}` couvre désormais 4 sous-systèmes de plus (parité avec les
fenêtres d'inspection de b2), en n'exposant que des champs réels :
`video`/`ula`, `kbd`, `joy`, `printer` (état MCP-40 inclus). Mêmes commandes en
REPL (`video`/`kbd`/`joy`/`printer`) et `--control` (`peek …`).

```bash
curl -s "localhost:8888/peek/video"    # mode ULA, OCULA, framebuffer
curl -s "localhost:8888/peek/kbd"      # matrice clavier 8 colonnes
```

### EPIC 6 / US 4 — Groupes de symboles *(Sprint 102)* — LIVRÉ

Les symboles peuvent être **tagués par groupe** (0-255) et un groupe
activé/désactivé — pratique pour distinguer les symboles de banques différentes
(BASIC ROM vs overlay Microdisc vs LOCI), à la manière des groupes de b2.

| Méthode  | Route                    | Effet |
|----------|--------------------------|-------|
| `POST`   | `/sym` `{path[,group]}`  | charge un fichier de symboles dans un groupe |
| `POST`   | `/sym/group` `{group,enabled}` | active/désactive un groupe |

`symbol_lookup`/`symbol_resolve` ignorent les symboles des groupes désactivés.
Mêmes commandes en `--control` (`load-sym FILE [group]`, `sym-group N on|off`) et
REPL (`sym load FILE [g]`, `sym group N on|off`, `sym groups`).

```bash
curl -s -X POST --data 'path=basic.sym&group=1'   localhost:8888/sym
curl -s -X POST --data 'path=microdisc.sym&group=2' localhost:8888/sym
curl -s -X POST --data 'group=2&enabled=off'      localhost:8888/sym/group
```

## 5. Estimation

~600-900 LOC au total (sink+dispatch ~200, file ~120, serveur HTTP+routing
~300, sécurité chemins ~80, tests ~200). Ordre de grandeur comparable au
module cast.
