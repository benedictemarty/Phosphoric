# Architecture — Bus I/O & périphériques (`io_device_t`)

> **Branche `feature/io-device-bus`.** Sort de `main.c` la cascade de `if
> (has_X && X_addr_in_range(addr)) return X_read(...)` (le dispatch page 3 câblé
> en dur, ~25 périphériques) au profit d'une **table de périphériques**. But :
> ajouter/retirer un périphérique = enregistrer/retirer **une entrée**, sans
> toucher au cœur. Le retrait d'OCULA a montré le coût de l'absence de cette
> abstraction (code saupoudré dans main/memory/video/savestate).

## 1. Le problème

`main.c` (~4600 lignes) est un god-object : chaque périphérique y est recâblé à
la main en 5+ endroits (CLI, init, **dispatch I/O**, boucle principale,
savestate, glue). `io_read_callback`/`io_write_callback` sont deux cascades de
`if` avec priorités et **interdépendances croisées** (ACIA↔Microdisc, fenêtre
ORICON qui recouvre le Microdisc, fiabilité MIA du LOCI pour l'ACIA à $0380…).

## 2. Ce qui est un « périphérique de bus » (et ce qui ne l'est pas)

Critère : **revendique une plage d'adresses en page 3** (± ROM overlay).

- **Cœur** (NE PAS traiter comme périphérique) : 6502, mémoire, **VIA**, ULA,
  PSG, clavier. Soudés, c'est l'Oric.
- **Devices bus** (cible de `io_device_t`) : **Microdisc** ($0310-$031F),
  **ACIA 6551** ($031C-$031F), **ULA-NG** ($0340-$035F), **LOCI**
  ($03A0-$03BF + TAP/DSK), **DTL 2000** ($03F8-$03FD), **Mageco/ORICON**
  ($03FE-$03FF / $031C-$031E).
- **Port-attached** (périphériques, mais PAS des devices bus) : joystick
  (PSG Port A), imprimante/MCP-40 (VIA Port A + CA2), cassette (VIA CB1). Ils
  se pilotent via les callbacks de port existants — **hors** de cette abstraction.

## 3. Le contrat

`include/io/io_device.h` :

```c
typedef struct io_device_s {
    const char* name;
    bool    (*claims)(struct emulator_s* emu, uint16_t addr);        /* claim LECTURE (+ écriture par défaut) */
    uint8_t (*read)(struct emulator_s* emu, uint16_t addr);
    bool    (*write)(struct emulator_s* emu, uint16_t addr, uint8_t value); /* true = consommé, false = repli VIA */
    bool    (*claims_write)(struct emulator_s* emu, uint16_t addr);  /* optionnel (NULL → claims) */
} io_device_t;
```

**Pourquoi `emulator_t*` et pas un simple `self` ?** Parce que les `claims`
sont conditionnels/croisés : `microdisc.claims` doit savoir si l'ACIA est
présente ; l'ACIA à $0380 doit consulter la fiabilité MIA du LOCI. Le contexte
complet est nécessaire. (Un `self` seul suffirait pour un device isolé, mais
pas pour le graphe de priorités réel.)

**`write` renvoie « consommé »**, `claims_write` distinct.** L'écriture peut
**décliner** (renvoyer `false`) pour retomber sur le VIA — indispensable à
l'ULA-NG, qui doit *voir* les écritures de sa fenêtre même verrouillée (pour
guetter la séquence de déverrouillage 'N','G') tout en laissant passer les
octets neutres à l'identique du VIA. Comme son claim d'écriture (fenêtre seule)
diffère de son claim de lecture (déverrouillée + fenêtre), un `claims_write`
optionnel s'ajoute (NULL → réutilise `claims`). Les périphériques à plage
exclusive laissent `claims_write` à NULL et renvoient toujours `true`.

**Dispatch** (`main.c`) : une table `io_bus[]`. En lecture, `io_bus_find(emu, addr)`
renvoie le **premier** device qui `claims()` ; en écriture, `io_bus_find_write`
utilise `claims_write ?: claims`, puis le dispatch respecte le verdict de `write`
(false → repli VIA). L'ordre de la table = la priorité. `io_read/write_callback`
se réduisent à **la boucle du bus + le repli VIA** — tous les périphériques à
plage sont désormais dans la table (**pattern strangler** mené à son terme).

## 4. Étape 1 réalisée — preuve du modèle

**Digitelec DTL 2000** ($03F8-$03FD, **plage exclusive** → migration
iso-comportement, aucun risque de priorité) migré derrière `io_device_t` :
wrappers `dtl2000_dev_{claims,read,write}`, entrée dans `io_bus[]`, `if` en dur
retirés des 2 callbacks. **Suite complète verte** (test-dtl2000 15/15 + intégration).

## 5. Ordre de migration

1. ✅ **DTL 2000** (fait — plage exclusive, valide le contrat).
2. ✅ **ACIA 6551**, **Mageco / ORICON** : petites plages recouvrant le Microdisc
   → priorité encodée dans l'ordre de la table + les `claims` (ACIA possède
   $031C-$031F si présente ; l'ACIA à $0380 consulte la fiabilité MIA du LOCI).
3. ✅ **Microdisc** : `claims` = `has_microdisc && 0x0310-0x031F` (l'ACIA, placée
   avant, possède déjà $031C-$031F si présente). Le wrapper conserve `fdc_trace`
   et la synchro des drapeaux d'overlay (`basic_rom_disabled`/`overlay_active`).
4. ✅ **LOCI** (MIA $03A0-$03BF + TAP $0315-$0317 + DSK $0310-$0314/$0318-$0319,
   3 sous-plages **disjointes**) : un seul `io_device_t` **en tête de table** qui
   dispatche en interne. Le `claims` encode la priorité (TAP recouvre toujours le
   Microdisc ; DSK seulement `!has_microdisc`). Le snoop VIA ORB $0300
   (`loci_tap_motor`, ligne moteur cassette) n'est **pas** un claim → reste dans
   le chemin VIA.
5. ✅ **ULA-NG** : migrée grâce à l'extension du contrat (`write` renvoyant
   « consommé » + `claims_write` distinct). Lecture : `claims` = déverrouillée &&
   en fenêtre. Écriture : `claims_write` = en fenêtre (toujours) ; `ula_ng_dev_write`
   renvoie le verdict de `ula_ng_write` (false verrouillée → repli VIA) et
   synchronise l'IRQ raster quand l'écriture est consommée. Placée **en dernier**
   dans la table (repli avant VIA). Non-régression : boots déverrouillage+palette
   byte-identiques au pré-migration, `test-ula-ng` 60/60, garde visible 2/2.

Aujourd'hui, `io_read/write_callback` = **la boucle du bus + le repli VIA**. Tous
les périphériques à plage (LOCI et ULA-NG compris) sont sur `io_device_t`.

## 6. Étapes suivantes (au-delà du dispatch I/O)

Le même principe s'étend à ce qui rend main.c monolithique :
- **init / reset / cleanup** : `io_device_t` pourrait porter `init/reset/cleanup`
  → boucles génériques (au lieu du câblage manuel).
- **savestate** : chaque device écrit/lit **sa** section .ost via le contrat →
  supprime les sections codées en dur (le point qui a fait mal avec OCB/OGP).
- **tick / IRQ** : hook `tick(emu, cycles)` optionnel pour les périphériques
  temporisés (ACIA baud, FDC).

Ces extensions se feront **après** que le dispatch I/O soit entièrement migré et
vert, une étape à la fois.

## 7. Contrainte opérationnelle

L'exécutable `oric1-emu` est utilisé par d'autres programmes : **jamais de
`make clean`** pendant ce chantier (il efface le binaire) ; builds **incrémentaux**
uniquement (le binaire n'est remplacé qu'en cas de link réussi) ; chaque étape
laisse un `oric1-emu` fonctionnel et **iso-comportement**.

## 8. Références
- `include/io/io_device.h`, `src/main.c` (`io_bus[]`, `io_bus_find`).
- Symptôme d'origine : `docs/ocula/CODE-MAP.md` (retrait OCULA, même mal).
