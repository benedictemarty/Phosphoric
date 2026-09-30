# Build WebAssembly (navigateur)

Phosphoric compile en **WebAssembly** via Emscripten : l'émulateur complet
(CPU 6502, mémoire, VIA, PSG, ULA, clavier, cassette, disque) tourne dans un
onglet, rendu sur un `<canvas>`, audio via Web Audio, clavier via le DOM — en
réutilisant le chemin SDL2 existant (port SDL d'Emscripten).

## Prérequis

[Emscripten SDK](https://emscripten.org) actif (`emcc` dans le `PATH`) :

```bash
git clone https://github.com/emscripten-core/emsdk
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ./emsdk_env.sh
```

## Construire et lancer

```bash
make wasm
(cd web && python3 -m http.server 8000)
# ouvrir http://localhost:8000/phosphoric.html
```

`make wasm` produit dans `web/` : `phosphoric.html` (page + canvas),
`phosphoric.js`, `phosphoric.wasm`, et `phosphoric.data` (les ROMs de `roms/`
préchargées dans le système de fichiers virtuel). La page démarre l'Atmos
(`-r /roms/basic11b.rom`) ; cliquez l'écran pour le focus clavier et l'audio.

## Déploiement : assets requis + CSP

La page charge sa logique (définition de `Module`, UI, clavier, glisser-déposer)
depuis **`web/shell.js`** — un fichier externe qu'il faut **déployer à côté** de
`phosphoric.html`/`.js`/`.wasm`/`.data`. `shell.js` est versionné (source) et
référencé par `phosphoric.html` via `<script src="shell.js">`. Idem pour
**`web/picowifi_js.js`** (transport du modem sans relais), chargé juste avant.

Cette externalisation rend le bundle **compatible Content-Security-Policy stricte**.
Un hôte qui sert la page sous `script-src 'self' 'wasm-unsafe-eval'` bloquerait un
`<script>` inline (`script-src-elem`) → `Module` jamais défini → `Module.canvas`
`undefined` → erreur fatale « can't access property … canvas is undefined » au
`createContext` WebGL. En externalisant tout le JS (et en remplaçant l'attribut
inline `oncontextmenu` du canvas par un écouteur DOM), `phosphoric.html` n'a plus
**aucun script/gestionnaire inline** : il tourne sous CSP stricte comme sous une
politique permissive (GitHub Pages). `phosphoric.js` est déjà chargé en externe
par Emscripten (`<script async src=phosphoric.js>`), et la compilation WASM exige
`'wasm-unsafe-eval'` dans `script-src`.

### CSP minimale requise

```
Content-Security-Policy: script-src 'self' 'wasm-unsafe-eval'
```

- **`'self'`** — autorise `shell.js`, `picowifi_js.js` et `phosphoric.js` (externes).
- Modem picowifi : si la politique restreint `connect-src`, y ajouter le relais
  (`ws://127.0.0.1:8766`) ou, sans relais, les sites visés par `fetch()`.
- **`'wasm-unsafe-eval'`** — **obligatoire** : `phosphoric.js` compile le module via
  `WebAssembly.instantiateStreaming`/`instantiate`, bloqués sous `script-src 'self'`
  seul. C'est le sous-token WASM (≠ `'unsafe-eval'`, bien plus large) — sûr.

⚠️ **Ne pas retirer `'wasm-unsafe-eval'`** : sous `script-src 'self'` nu, la
compilation WebAssembly est refusée et **l'émulateur ne démarre pas** (aucun log
`Initializing Phosphoric …`). Après le fix d'externalisation, ni `'unsafe-inline'`
ni hash/nonce ne sont nécessaires — seul le token WASM l'est.

> Un blocage `script-src-elem` dont la source est `sandbox eval code` (et non
> `phosphoric.html`) provient d'une **extension navigateur** (content script), pas
> de Phosphoric : la page n'injecte aucun script inline ni `eval`. Sans effet sur
> l'émulateur.

## Interface (page web)

La page (`web/shell.html`) présente un **rail d'icônes vertical à gauche**
(façon JOric) et le clavier ORIC en overlay :

- **MODEL** — bascule machine ORIC-1 / Atmos (badge `1`/`A`, relance à froid avec
  la ROM choisie).
- **I/O** (ou **F1**) — **menu des périphériques** (voir le README) : disquettes,
  cassette, instantanés, imprimante, joystick, clavier ; la machine est figée tant
  qu'il est ouvert. Le navigateur ne reçoit pas F1 (pas d'aide qui s'ouvre). Au
  clavier virtuel, les flèches, **RET**, **ESC** et **DEL** pilotent le menu
  (masquer le clavier avec **KEYS** pour voir tout le menu). Le sélecteur de
  fichiers liste aussi `/media`, où la page dépose les fichiers chargés ; sans
  contrôleur disque au boot, les lecteurs sont affichés « absent » (charger un
  `.dsk` par **LOAD** redémarre avec le Microdisc). `phosphoric.cfg` et
  `snapshots/` vivent dans le système de fichiers en mémoire de la page (perdus au
  rechargement). E2E : `make test-web-iomenu`.
- **LOAD** + **glisser-déposer** d'un `.tap`/`.dsk` sur l'écran : le fichier est
  inséré et la machine redémarre dessus (cassette `-t …-f`, ou disquette
  `--disk-rom microdis.rom -d …`). Bouton **EJECT** pour le retirer.
- **Liens profonds (paramètres URL)** — `?rom=oric1|atmos` choisit la machine et
  `?media=<fichier>` charge un média dès le premier chargement. Le **type** est
  déduit de l'extension : `.tap` → cassette (`-t … -f`), `.dsk` → disquette. Pour
  un `.dsk`, le **contrôleur Microdisc est activé au boot** (`--disk-rom
  microdis.rom`) dès que l'URL vise une disquette, puis l'image est insérée à
  chaud — sans cela le boot se ferait sans FDC et l'insertion échouerait. Le
  fichier ciblé doit être servi en **binaire** (un serveur renvoyant une page
  HTML de repli en 200 fait échouer l'insertion : « not a valid TAP/DSK »).
- **LOCI** (ou `?loci=1` dans l'URL) — branche la **cartouche LOCI** (émulation
  HLE `--loci`, relance à froid) : la machine démarre sur le **menu LOCI**
  (`roms/loci/locirom`). Son **stockage « flash interne »** est `/loci`, un
  système de fichiers **IDBFS persistant dans IndexedDB** du navigateur (il
  survit aux rechargements), semé au premier lancement avec `basic11b.rom`,
  `basic10.rom`, `microdis.rom` et `locirom`. En mode LOCI, **LOAD / glisser-
  déposer copie le fichier dans ce flash** (`.dsk`, `.tap`, `.rom`, …) au lieu de
  l'insérer : on le choisit ensuite dans le menu (**Espace** ouvre le sélecteur
  du champ courant, **ESC** = boot), comme sur la vraie cartouche. **F8** =
  bouton Action (retour au menu). `?loci=0` ou un nouveau clic sur LOCI
  revient au mode normal. **LOCI + cassette** : `?loci=1&media=prog.tap` démarre
  BASIC directement sur la cassette avec la cartouche présente (ACIA picowifi en
  `$0380`, flash persistant) — équivalent de `-t prog.tap -f --loci --loci-flash …`
  en natif (cas ProphetOric/OricTel, qui ne sondent l'ACIA qu'en `$0380`).
  Non disponibles en web : co-simulation du vrai
  firmware (`--loci-emu`), clés USB de l'hôte et image SD (`--loci-sdimg`).
- **MODEM** (ou `?modem=1`) — modem **picowifi** (firmware PicoWiFiModemUSB
  émulé, `--serial picowifi:Web:web`, WiFi simulé « Web ») sur l'ACIA 6551 :
  `$0380` avec LOCI, `$031C` sinon. Avec LOCI, `--serial-buffer 32` est ajouté :
  c'est l'anneau RX de 32 octets du firmware LOCI (`acia.c`,
  `ACIA_RX_BUFFER_SIZE`), sans lequel l'écho `ATZ` déborde (ProphetOric : « pas
  de modem »). Sans LOCI, 6551 nu : pas de FIFO. Le navigateur n'ouvrant pas de TCP, chaque
  connexion (ATDT, ATGET, ATRD/ATRT, ATDISKRD…) passe par un **relais
  WebSocket** à lancer sur la machine : `python3 tools/picowifi_ws_relay.py`
  (défaut `ws://127.0.0.1:8766/`, autre relais : `?relay=ws://hôte:port/`,
  mémorisé). Le relais termine aussi le **TLS** (`ATDT` sécurisé, `ATGET
  https://`) avec vérification de certificat système. Avec LOCI, la NVRAM du
  modem (`AT&W`) est persistée dans le flash (`/loci/picowifi.cfg`).
  **Sans relais** (`?relay=none`, à la manière du `neomodem.js` de Phosphoneo) :
  `web/picowifi_js.js` fournit des sockets virtuelles — les requêtes HTTP du
  modem (`ATGET http(s)://`, `ATDISKRD/WR`) sont rejouées par `fetch()`
  (sites autorisant CORS, ou via un proxy même origine `?httpproxy=/proxy?url=`)
  et la réponse est rendue en HTTP/1.1 reconstitué ; `ATRD`/`ATRT` lisent
  l'horloge du navigateur. Les en-têtes de la requête sont transmis (ex.
  `ResponseFormat`), sauf ceux que `fetch()` refuse ou fixe (Host, Connection,
  Content-Length, User-Agent…). **`?httpsame=h1,h2`** : une requête vers `h1`/`h2`
  (quels que soient port et schéma, ex. `ATD-prophet.3617.fr:8998` puis HTTP
  brut) part vers **l'origine de la page** + chemin — pour une page servie par le
  même serveur sous CSP `connect-src 'self'`, sans contenu mixte. Telnet/BBS et
  TCP brut exigent le relais : sans lui, la connexion est coupée au premier
  octet non-HTTP (NO CARRIER).
  Exemple : `phosphoric.html?loci=1&modem=1&relay=none&httpsame=prophet.3617.fr&media=prophetoric.tap`.
- **RESET** — reboot à froid en conservant ROM et média.
- **KEYS** — affiche/masque le clavier virtuel.
- **FULL** — plein écran (le canvas est centré et mis à la hauteur de l'écran,
  ratio 240/224 conservé).
- **CRT** — filtre scanlines + vignette par-dessus l'écran (état mémorisé).
- **SAVE / REST** — sauvegarde l'état dans un `.ost` (téléchargé) / restaure un
  `.ost` (appliqué **à chaud**, sans reboot).
- **LEDs TAPE / DISK** (sous l'écran) — s'allument pendant un CLOAD (cassette)
  ou un accès disque (WD1793 BUSY).
- **Clavier virtuel ORIC fidèle**, en **overlay semi-transparent par-dessus
  l'écran** : disposition réelle (ESC, CTRL, FUNCT, 2× SHIFT, RETURN, DEL, SPACE,
  flèches ↑←↓→) avec modificateurs **collants CTRL / FUNCT / SHIFT** (le Shift/
  Ctrl/Alt physiques pilotent aussi ces modificateurs). Les symboles shiftés
  affichés en exposant sont **dérivés de la matrice réelle** (`char_map`) :
  `2`→`@`, `6`→`^`, `-`→`_`, `;`→`:`, `[`→`{`, `\`→`|`, etc. En **ORIC-1**, la
  touche **FUNCT (Atmos-only) est absente**.

> **CTRL+T et autres chords :** le navigateur réserve certains raccourcis
> (CTRL+T = nouvel onglet) au niveau de l'OS — ils n'atteignent jamais le
> canvas. Utilisez la **touche CTRL du clavier virtuel** : elle écrit la
> matrice ORIC via un appel C direct (`web_key`), donc le navigateur ne
> l'intercepte pas. Idem pour FUNCT.

> Servez les fichiers par **HTTP** (pas `file://`) : le navigateur refuse de
> charger un `.wasm` depuis le système de fichiers local.

## Détails techniques

- **Boucle principale** : sous le navigateur, la boucle `while` C doit rendre
  la main à la boucle d'événements à chaque frame. C'est fait via
  **Asyncify** (`-sASYNCIFY`) + `emscripten_sleep()` dans le limiteur de frame
  (`src/main.c`, gardé par `__EMSCRIPTEN__`) — qui cadence aussi à ~50 Hz.
- **Pile** : `emulator_t` est volumineux (framebuffer + mémoire) et vit sur la
  pile de `main()` ; le build force `-sSTACK_SIZE=8MB` (le défaut 64 Ko
  déborderait).
- **Réseau** : les fonctionnalités qui exigent des sockets/threads natifs
  (backends série TCP/PTY/COM, stub GDB, serveur Cast) se lient en no-op dans
  le navigateur — sauf le modem picowifi, dont `serial_picowifi.c` route les
  connexions (`pw_read`/`pw_write`/`pw_close`/`pw_wait`) vers des WebSocket JS
  (`EM_JS`, fd ≥ `0x100000`) ouverts vers le relais
  `?host=H&port=P&tls=0|1` ; les attentes rendent la main au navigateur par
  `emscripten_sleep` (Asyncify). Relais : `tools/picowifi_ws_relay.py`
  (stdlib Python, 127.0.0.1 par défaut, `--allow HOTE[:PORT]`, `--origin URL`) — le cœur machine, la vidéo, l'audio, le
  clavier, la cassette et le disque fonctionnent. La cartouche LOCI (HLE)
  fonctionne ; seule sa co-simulation RP2040 (`libemul`, native) est remplacée
  par un bouchon (`loci_emu_stub.c`).
- **Flash LOCI persistant** : lien `-lidbfs.js` ; `web/shell.js` monte IDBFS sur
  `/loci` en `preRun` (dépendance de lancement le temps du `syncfs(true)`),
  sème les ROM dans `onRuntimeInitialized` (les fichiers préchargés `/roms`
  n'existent qu'à ce stade) puis resynchronise vers IndexedDB toutes les 5 s et
  à `pagehide`.
- **`web_peek(addr)`** : lecture mémoire sans effet de bord (`memory_peek`)
  exportée pour l'UI et les tests e2e (lecture de l'écran texte `$BB80`).
- **Test e2e navigateur** : `make test-web-loci` construit la build web, la sert
  en local et la pilote dans Chrome headless via Playwright (menu LOCI au boot,
  flash semé, import + persistance au rechargement, fichier listé par le
  sélecteur, et montage + boot d'un vrai `.dsk` si `disks/3dfongus.dsk` existe).
  SKIP si emsdk, node, Playwright ou Chrome manquent ; hors `make tests`.
- **Test e2e modem** : `make test-web-picowifi` (9/9), programmes BASIC
  tokenisés en `.tap` auto-run (`bas2tap`) et chargés par `?media=` :
  1. relais : BASIC → ACIA `$031C` → picowifi WASM → WebSocket → relais →
     serveur TCP local, et retour ;
  2. sans relais : `ATGET` rejoué par `fetch()` (serveur CORS), `ATRD` local ;
  3. LOCI + cassette + `?httpsame=` : ACIA `$0380`, `ATD-` puis HTTP brut
     réécrit vers l'origine, `ResponseFormat` reçu par le serveur ;
  4. LOCI + modem par défaut : anneau RX de 32 octets ; sans LOCI, aucun.
- **Arguments de test** : un tableau JSON dans `sessionStorage`
  `phos_extra_args` est ajouté à la ligne de commande (ex. `--type-keys`).

## Fidélité — vérifié

La sortie WASM est **byte-identique au build natif** pour des entrées
identiques : un boot Atmos headless (`-n -c N --screenshot`) compilé en WASM et
exécuté sous Node.js produit la **même capture PPM exacte** que le binaire natif
(testé à 2M et 5M cycles). Le déterminisme au cycle bus du cœur est préservé à
travers la compilation WebAssembly.

Le rendu **navigateur** a aussi été validé : la page chargée dans Chromium
headless (`--virtual-time-budget`) affiche le canvas avec l'écran de boot Atmos
correct (« ORIC EXTENDED BASIC V1.1 / © 1983 TANGERINE / 37631 BYTES FREE /
Ready » + indicateur CAPS) — le chemin SDL2 → WebGL/canvas fonctionne de bout
en bout. La **saisie clavier** a aussi été validée : taper `PRINT 6*7` + RETURN
via le pont `web_key` (clavier virtuel) affiche `42` — boot ROM → injection
clavier → exécution BASIC → rendu, entièrement dans le navigateur.
