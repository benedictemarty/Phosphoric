# Sprint 90 — Émulation cassette au niveau signal (CB1 + Timer 2)

> Statut : **TERMINÉ** (v1.50.0-alpha, 2026-07-04)
> Version : 1.49.0-alpha → **1.50.0-alpha** (MINOR — nouvelle fonctionnalité)
> Déclencheur : le jeu *Soccer Manager* (KnightSoft 1984) ne se lance pas
> (chargeur cassette maison + déchiffrement `EOR #$55`), comme sur Oricutron,
> alors qu'il fonctionne sur Euphoric et sur vraie machine.

## 1. Problème

Phosphoric émule la cassette **uniquement par patch des routines ROM**
(interception `getsync`/`readbyte` dans `tape_patches()`, `src/main.c`).
`src/io/cassette.c::cassette_read_bit()` est un stub qui renvoie 0.

Les jeux à **chargeur cassette non standard** (turbo-loaders, routines de
déprotection, séquencement multi-blocs maison) contournent tout ou partie des
routines ROM patchées → chargement incorrect → plantage. C'est la même limite
que sur Oricutron (patch-based). Euphoric et le vrai matériel marchent car ils
émulent la **prise cassette au niveau du signal électrique**.

## 2. Mécanisme matériel réel (rétro-ingénierie ROM BASIC 1.0, vérifiée)

La lecture cassette ORIC repose sur **VIA CB1** (fronts du signal bande) +
**VIA Timer 2** (mesure de largeur d'impulsion). Désassemblage de la ROM :

### Primitive de mesure d'impulsion `$E67D`
```
$E67D: PHA
$E67E: LDA $0300     ; lit ORB → EFFACE le flag CB1 (ré-arme la détection de front)
$E681: LDA $030D     ; lit IFR
$E684: AND #$10      ; bit 4 = flag CB1
$E686: BEQ $E681     ; attend le front CB1 suivant (transition bande)
$E688: LDA $0309     ; lit Timer2 high = cycles écoulés / 256
$E68C: LDA #$FF ; STA $0309   ; recharge T2 high = $FF
$E692: CMP #$FE      ; intervalle < ~512 cyc → C=1 (COURT) ; >= 512 → C=0 (LONG)
$E695: RTS           ; retourne avec Carry = classification de l'impulsion
```

### Décodeur de bit `$E661`
```
lit 1 impulsion ; si COURTE → lire 6 de plus (7 au total), sinon 2 (3 au total)
compte les impulsions COURTES ; CMP #$04 : >=4 → bit '1', sinon bit '0'
```

### Conséquence pour la génération du signal
- Le CPU compte **un front actif CB1 par période** du ton bande.
- Ton **'1' = 2400 Hz** → période ≈ 416 cycles (< 512 → COURT).
- Ton **'0' = 1200 Hz** → période ≈ 833 cycles (>= 512 → LONG).
- Seuil ROM ($FE sur T2-high) ⇒ frontière ≈ 512 cycles : sépare proprement.
- Trame octet = 14 bits, LSB first : start(0) · 8 données · parité impaire ·
  stop(1,1,1,1) — cf. `tap_encode_frame()` (loci_bus.c) déjà présent.

Adresses équivalentes BASIC 1.1 (Atmos) : primitives cassette autour de
`$E6C9`/`$E735` (à re-vérifier au désassemblage lors du portage Atmos).

## 3. Conception

### Générateur de signal (`src/io/cassette.c`)
État : buffer bande (réutilise `emu->tapebuf`/`tapelen`/`tapeoffs`), position en
bits, phase d'impulsion, horloge cycles. `cassette_tick(emu, cycles)` avance
l'horloge et, aux instants d'impulsion, appelle `via_set_cb1()` pour basculer la
broche → front actif → flag IFR CB1. Le Timer 2 du VIA (déjà modélisé) mesure.

Constantes de timing (à caler empiriquement via test d'intégration) :
```
CAS_PERIOD_2400 ≈ 416   /* cycles φ2 : ton '1' */
CAS_PERIOD_1200 ≈ 833   /* cycles φ2 : ton '0' */
CAS_PULSES_PER_1  = ...  /* nb de périodes 2400 Hz par bit '1' */
CAS_PULSES_PER_0  = ...  /* nb de périodes 1200 Hz par bit '0' */
CAS_LEADER        = ...  /* impulsions de synchro (pilot tone) avant chaque bloc */
```

### Contrôle moteur
Moteur cassette = VIA ORB bit 6 (déjà snoopé pour LOCI, `main.c:1196`).
Le signal n'avance que moteur ON.

### Intégration boucle
Ajouter dans `cpu_cycle_tick()` (main.c:1240) :
`if (emu->tape_signal_mode) cassette_tick(emu, cycles);`

### Activation
Nouveau flag CLI **`--tape-signal`** : bascule en lecture niveau signal (désarme
les patches `getsync`/`readbyte`). Le mode patch reste le défaut (compatibilité,
rapidité). Incompatible avec `-f` (fast-load) → message clair.

## 4. Découpage (incréments testables)

- **P1** — Squelette : struct état cassette, timing constants, `cassette_tick`
  + `cassette_signal_*` API, flag `--tape-signal`, câblage `via_set_cb1`.
  Tests unitaires : encodage trame 14 bits, planning des fronts, gate moteur.
- **P2** — Boucle de calage : test d'intégration « octets → signal → routine ROM
  `readbyte` réelle → octets relus » ; tuning des constantes jusqu'à identité.
- **P3** — Chargement bout-en-bout d'un `.tap` standard via la vraie ROM (sans
  patch), vérifié par comparaison mémoire (programme BASIC chargé).
- **P4** — Validation *Soccer Manager* (chargeur maison) : titre + lancement jeu.
- **P5** — Portage Atmos (adresses ROM 1.1), doc, mise à jour fichiers de suivi.

## 5. Critères d'acceptation

1. `--tape-signal` charge un `.tap` BASIC standard via la ROM réelle (ORIC-1),
   programme identique octet à octet.
2. *Soccer Manager* : écran-titre PUIS jeu lancé (plus de retour BASIC).
3. Tous les tests passent (`make tests`) ; pas de régression du mode patch.
4. Valgrind propre sur la nouvelle suite.
5. Fichiers de suivi à jour (CHANGELOG, VERSION_TRACKING, CIRRUS_OS, ROADMAP).

## 6. Résultat (livré)

- **Constantes finales** : demi-période bit '1' = 208 cyc (période 416),
  demi-période haute d'un bit '0' = 416 cyc (période 624), 1ʳᵉ demi-période
  toujours 208 (phase ancrée), seuil ROM ~512 cyc. Leader = 64 trames 0x16.
- **Découverte clé** : la ligne moteur PB6 est écrasée par le scan clavier
  (mêmes bits ORB au repos et pendant CLOAD) → gating sur le PC dans la routine
  de lecture ROM à la place, avec rembobinage à la première entrée. Position
  préservée entre octets/blocs (le mode signal « met en pause » hors lecture),
  ce qui gère naturellement les chargeurs multi-blocs protégés.
- **Vérifié** : `hello.tap` (BASIC standard) chargé + listé via la vraie ROM ;
  *Soccer Manager* (ORIC-1) chargé + lancé (« What is your first name please ? »).
- **Tests** : `make test-cassette` (8/8), suite complète 876/876, valgrind 0
  erreur, analyse statique stricte propre.
- **Non couvert v1** (backlog) : mode rapide (2400 bauds, `zp$67`), portage des
  adresses de gating Atmos (le protocole est identique ; seules les bornes
  `readbyte_entry/getsync_end` de `rom_patches` diffèrent, déjà présentes).
