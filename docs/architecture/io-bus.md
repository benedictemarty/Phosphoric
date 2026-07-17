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
    bool    (*claims)(struct emulator_s* emu, uint16_t addr);
    uint8_t (*read)(struct emulator_s* emu, uint16_t addr);
    void    (*write)(struct emulator_s* emu, uint16_t addr, uint8_t value);
} io_device_t;
```

**Pourquoi `emulator_t*` et pas un simple `self` ?** Parce que les `claims`
sont conditionnels/croisés : `microdisc.claims` doit savoir si l'ACIA est
présente ; l'ACIA à $0380 doit consulter la fiabilité MIA du LOCI. Le contexte
complet est nécessaire. (Un `self` seul suffirait pour un device isolé, mais
pas pour le graphe de priorités réel.)

**Dispatch** (`main.c`) : une table `io_bus[]` + `io_bus_find(emu, addr)` qui
renvoie le **premier** device qui `claims()` (l'ordre de la table = la priorité).
`io_read/write_callback` interrogent la table en tête, puis retombent sur les
`if` en dur restants (**pattern strangler** : migration progressive, jamais un
big-bang).

## 4. Étape 1 réalisée — preuve du modèle

**Digitelec DTL 2000** ($03F8-$03FD, **plage exclusive** → migration
iso-comportement, aucun risque de priorité) migré derrière `io_device_t` :
wrappers `dtl2000_dev_{claims,read,write}`, entrée dans `io_bus[]`, `if` en dur
retirés des 2 callbacks. **Suite complète verte** (test-dtl2000 15/15 + intégration).

## 5. Ordre de migration proposé

1. ✅ **DTL 2000** (fait — plage exclusive, valide le contrat).
2. **Mageco / ORICON** puis **ACIA 6551** : petites plages, mais l'ORICON et
   l'ACIA **recouvrent** le Microdisc → migrer ensemble et encoder la priorité
   dans l'ordre de la table + les `claims` (ACIA possède $031C-$031F si présente).
3. **Microdisc** : `claims` = `has_microdisc && 0x0310-0x031F && !(ACIA/ORICON le réclame)`.
4. **LOCI** (MIA + TAP + DSK, 3 sous-plages, priorité haute, conditions vs
   Microdisc) : le plus gros ; à faire en dernier, une fois le modèle rodé.
5. **ULA-NG** : cas particulier (passthrough VIA si verrouillé) → `claims` =
   `ula_ng_active(...) && ula_ng_addr_in_window(addr)` (déjà la sémantique
   actuelle). Le rendu reste dans `video`.

Une fois tout migré, `io_read/write_callback` = **une boucle + le repli VIA**.

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
