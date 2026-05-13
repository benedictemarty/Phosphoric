# Note technique — Asteroids Oric‑1 : VSync via CB1 ≠ hardware réel

**Date initiale** : 2026-05-13
**Révision v2** : 2026-05-13 (sources primaires ajoutées, Option B retirée)
**De** : équipe Phosphoric (bmarty)
**Pour** : équipe Asteroids Oric‑1
**Objet** : `frame_wait()` repose sur un câblage CB1 = VSync inexistant sur l'Oric d'usine

---

## TL;DR

Votre `frame_wait()` (`src/game.c:289-300` v1) polle `IFR bit 4` (CB1) en
supposant que la broche est pulsée à chaque trame par l'ULA.
**Vérifié sur sources primaires Oric (oric.free.fr, defence-force.org,
twilighte.oric.org, etc. — cf. §9) : sur l'Oric d'usine CB1 = entrée signal
cassette**, *pas* VSync. Phosphoric ≤ 1.16.10 émulait par défaut une
convention non‑conforme (équivalente à l'option `vsynchack` opt‑in
d'Oricutron) qui masquait le bug. Depuis **Phosphoric 1.16.11** (commit
`f98f828`, 2026-05-13), Phosphoric s'aligne sur le câblage usine et le
programme **boucle à l'infini** comme dans Oricutron WIP `f79d5d4` (par
défaut `vsynchack=OFF`).

Action requise côté Asteroids : remplacer la synchro CB1 par **Timer 1 du
VIA en mode continu** (seule option portable Oric vrai + tous émulateurs).
L'« option lecture bit ULA mémoire-mappé » que j'avais suggérée en v1
**n'existe pas** sur l'Oric d'usine — voir §3 et §5 corrigés.

---

## 1. Symptôme observé

Capture Oricutron `f79d5d4` après chargement de `asteroids.tap` :

```
PC=0D8E   AD 0D 03   LDA $030D    ; VIA_IFR
0D91      29 10      AND #$10     ; isole bit 4 (CB1)
0D93      F0 F9      BEQ $0D8E    ; loop while flag = 0
```

PC oscille entre `$0D8E` et `$0D9F` (deux instances du même `frame_wait()` à
des endroits différents du code). L'écran titre "ASTEROIDS / PRESS SPACE"
reste figé, aucune anim de polish, aucun keyscan.

Reproduit à l'identique sur Phosphoric 1.16.11 :

```
oric1-emu -r basic10.rom -t asteroids.tap -f -n -c 10000000
→ Final CPU state: A:40 X:00 Y:0E SP:FB P:..-..... PC:0D8E
```

## 2. Code incriminé

`src/game.c:126-130` :

```c
/* Phase 9 — synchro VSync ULA via CB1.
 * Sur Oric-1, CB1 est connecté au signal VSync de l'ULA (50 Hz PAL).
 * IFR bit 4 = flag CB1, set sur transition. À 25 Hz = 2 VSync par frame. */
#define VSYNC_FLAG       0x10        /* IFR bit 4 = CB1 */
#define VSYNCS_PER_FRAME 2           /* 50 Hz / 2 = 25 Hz */
```

`src/game.c:289-300` :

```c
static void frame_wait(void)
{
    unsigned char i;
    for (i = 0; i < VSYNCS_PER_FRAME; i++) {
        while (!(VIA_IFR & VSYNC_FLAG)) { }
        VIA_IFR = VSYNC_FLAG;        /* clear par écriture du bit */
    }
}
```

Le commentaire « CB1 est connecté au signal VSync de l'ULA » est **incorrect
pour le hardware Oric**.

## 3. Réalité hardware Oric‑1 / Atmos (sources primaires)

Sur l'Oric d'usine, la broche **CB1 du VIA 6522 = entrée signal cassette**,
**pas** VSync. Citations directes des sources autoritaires de la communauté
Oric :

> *« The CB1 pin on the 6522 is the tape signal input, with a 1K resistor and
> 2.2nF capacitor connected between 5V and the CB1 pin. »*
> — [Hardware Programming on the Oric (oric.free.fr)](http://oric.free.fr/programming.html)

> *« The cassette circuitry connects to the CB1 line on the 6522. When this
> goes from low to high the CB1 flag is set in the interrupt flag register of
> the 6522. »*
> — [VIA — twilighte.oric.org](http://twilighte.oric.org/twinew/via.htm)

> *« A 1-bit digitised version of the tape's audio stream is fed as an input
> to the CB1 pin of the Oric's VIA. The ROM configures the 6522 to raise an
> interrupt upon a low-to-high transition of CB1. It then measures the amount
> of time between those interrupts using the 6522's second timer. »*
> — [Defence Force Wiki — Oric tape encoding](https://wiki.defence-force.org/doku.php?id=oric:hardware:tape_encoding)

> *« Reading the tape requires measuring the time interval between edges on
> the CB1 pin (with a VIA's timer of course). »*
> — [Connecting a Commodore tape drive to the Oric‑1 (Marko Mäkelä)](https://www.ktverkko.fi/~msmakela/8bit/c2n-oric/index.en.html)

**Conséquences** :

- En idle (pas de lecture cassette en cours), CB1 reste à l'état haut. Aucun
  front, aucun `IFR.CB1` n'est positionné, et `frame_wait()` boucle à l'infini.
- Pendant un `CLOAD`, CB1 transite à la fréquence des bits cassette
  (~2400 Hz Standard / ~1200 Hz Slow), **jamais à 50 Hz**. Donc même pendant
  un chargement, le polling renverrait n'importe quoi.

### Et la VSync de l'ULA, alors ?

Sur le câblage usine, **la VSync n'est pas exposée au CPU via le VIA**. Elle
sort uniquement sur la broche SYNC du connecteur RGB pour le moniteur.
L'option qu'on croyait noble — « lire un bit ULA mémoire-mappé pour la
VSync » — **n'existe pas sur le hardware standard**. Les programmes Oric
qui veulent une cadence trame se reposent sur :

- **Timer 1** du VIA en mode continu (cadence stable à 20 ms PAL)
- ou comptage de cycles 6502 à la main

Il existe aussi une **modification hardware DIY** documentée par la
communauté : recâbler la broche SYNC du connecteur RGB sur l'entrée TAPE,
de sorte que la VSync arrive sur CB1. C'est cette modif qu'Oricutron émule
avec son option `vsynchack` (OFF par défaut) — pas le câblage usine. Voir
le code Oricutron `tape.c:1414-1423` (`if (oric->vsynchack)`) et le
commentaire `ula.c:295-312` qui illustre le waveform de cette modif.

Le code asteroids actuel ne marchait que sur des émulateurs avec ce
« hack » activé (Phosphoric ≤ 1.16.10 le simulait par défaut, ce qui était
non‑conforme à l'Oric d'usine).

## 4. Pourquoi Phosphoric ≤ 1.16.10 « marchait »

Par simplification historique, Phosphoric pulsait CB1 falling/rising à chaque
trame (`src/main.c:857-866`) :

```c
if (!vsync_triggered && frame_cycles >= VSYNC_CYCLE) {
    via_set_cb1(&emu->via, false);  /* falling */
    vsync_triggered = true;
}
...
if (vsync_triggered) {
    via_set_cb1(&emu->via, true);   /* rising */
}
```

Avec `PCR bit 4 = 1` (configuration de la ROM Oric), le rising edge de CB1
mettait `IFR bit 4 = 1`, et `frame_wait()` voyait sa condition satisfaite.

**Ce comportement était non‑conforme au hardware.** Il a été supprimé en
1.16.11 pour rendre Phosphoric utilisable comme référence hardware en
développement.

## 5. Solutions proposées (par ordre de qualité)

### Option A — Timer 1 VIA en mode continu (recommandée, portable partout)

Programmer T1 sur 20 000 µs (= 20 000 cycles à 1 MHz, soit `$4E1F`) en mode
free-run. Le flag IFR bit 6 (T1) sera mis tous les 20 ms, le polling devient :

```c
#define T1_FLAG     0x40        /* IFR bit 6 */

static void timer_init(void)
{
    VIA_T1CL = 0x1F;
    VIA_T1CH = 0x4E;            /* 20000 → 20 ms PAL */
    VIA_ACR  = (VIA_ACR & 0x3F) | 0x40;  /* T1 free-run, no PB7 */
    VIA_IER  = 0x40;            /* disable T1 IRQ (polling only) */
    VIA_IFR  = 0x40;            /* clear flag initial */
}

static void frame_wait(void)
{
    unsigned char i;
    for (i = 0; i < VSYNCS_PER_FRAME; i++) {
        while (!(VIA_IFR & T1_FLAG)) { }
        VIA_T1CL_RESET;          /* relire T1CL pour clear IFR T1 */
    }
}
```

**Avantages** : fonctionne sur vrai Oric, Oricutron, Phosphoric, MAME. Dérive
de quelques cycles par trame (acceptable pour un jeu 25 Hz).

**Inconvénient** : pas synchro stricte avec le balayage écran → léger tearing
possible sur HIRES dynamique. Pour Asteroids c'est négligeable.

### ~~Option B — Lecture bit VSync de l'ULA~~ (n'existe pas sur Oric d'usine)

Cette option avait été suggérée dans la première version de cette note. **Elle
est invalide** : après vérification sur sources primaires (cf. §3), la VSync
n'est pas exposée au CPU via un bit mémoire-mappé sur Oric‑1/Atmos standard.
Le seul moyen « hardware » d'avoir VSync sur CB1 est la modification DIY
RGB→TAPE, qui ne fait pas partie d'un Oric d'usine et qu'aucun acheteur
moderne ne possèdera.

➡️ **Reste l'option A (Timer 1 free-run), c'est elle qu'il faut adopter.**

### Option C — Compteur de frames via NMI/IRQ ROM

La ROM Oric installe déjà un handler IRQ qui s'exécute à chaque T1 (sa propre
config). Hooker `$02FA` ou polluer un compteur en zéro-page incrementé par la
ROM. Plus fragile, dépend de la ROM exacte (BASIC 1.0 vs 1.1). À éviter.

## 6. Tests à refaire après correction

1. `asteroids.tap` fast-load Phosphoric 1.16.11+ : doit dépasser l'écran titre
   et accepter SPACE.
2. `asteroids.tap` Oricutron WIP : doit dépasser l'écran titre.
3. `asteroids.dsk` sur vrai Oric‑1 (revision PAL) : doit jouer normalement
   à ~25 fps.

## 7. Côté Phosphoric — ce qui a changé

- **v1.16.10 et antérieures** : Phosphoric pulsait CB1 à chaque trame
  (équivalent du `vsynchack` d'Oricutron). Non conforme au câblage usine —
  c'était un héritage par convention.
- **v1.16.11** (commit `f98f828`) : CB1 plus jamais piloté par l'émulateur.
  État idle high. Phosphoric s'aligne sur le câblage usine documenté par les
  sources Oric.
- **v1.16.12** (commit `2f02b64`) : fix indépendant — suppression de
  `SDL_RENDERER_PRESENTVSYNC` qui causait un faux « ne répond pas » Mutter
  quand la fenêtre restait statique (cas du programme bloqué dans
  `frame_wait` actuel).

À noter : Phosphoric ne pilote pas (encore) CB1 sur le signal cassette
pendant un CLOAD non‑patché — le fast‑load (`-f`) court‑circuite la
routine ROM via patches PC, donc le tape‑bit‑sampling temps réel n'est pas
sur le chemin critique. Implémenter le signal cassette CB1 reste un TODO
(cf. `docs/AGILE_PLAN.md` T‑319) mais sans impact sur Asteroids.

## 8. Contacts

- Émulateur Phosphoric : `/home/bmarty/Oric1`, commit version
  `EMU_VERSION 1.16.12-alpha`
- Code asteroids : `/home/bmarty/Oric asteroids`
- bmarty <bmarty@mailo.com>

## 9. Sources et références

Cette note a été mise à jour le **2026-05-13** après vérification croisée
sur sources primaires de la communauté Oric. Référez-vous à ces liens en
priorité pour toute question hardware :

- [Hardware Programming on the Oric (oric.free.fr)](http://oric.free.fr/programming.html)
  — référence Fabrice Frances, pinout 6522 explicite
- [ORIC 1/ATMOS Unofficial ULA Guide](http://oric.free.fr/HARDWARE/ula.html)
  — signaux internes de l'ULA et leurs sorties
- [ULA Deconstruction 1 — signal11.org.uk](https://oric.signal11.org.uk/html/ula1.htm)
  — analyse rétro-ingénierie de l'ULA
- [SERVICE MANUAL FOR THE ORIC‑1 and ORIC ATMOS (PDF, 48k atmos)](http://www.48katmos.freeuk.com/servman.pdf)
  — schémas officiels d'usine
- [Up to date Oric‑1/Atmos Schematic — defence-force.org forum](https://forum.defence-force.org/viewtopic.php?t=1959)
  — pointeurs vers schémas redessinés par Godzil
- [VIA — twilighte.oric.org](http://twilighte.oric.org/twinew/via.htm)
  — détail du câblage VIA 6522 sur Oric
- [Defence Force Wiki — Oric tape encoding](https://wiki.defence-force.org/doku.php?id=oric:hardware:tape_encoding)
  — protocole cassette, CB1 = entrée bit numérisé
- [Connecting a Commodore tape drive to the Oric‑1 (Marko Mäkelä)](https://www.ktverkko.fi/~msmakela/8bit/c2n-oric/index.en.html)
  — détail circuit cassette côté CB1
- [Tynemouth Software — Oric Atmos Repair](http://blog.tynemouthsoftware.co.uk/2022/11/oric-atmos-repair.html)
  — détails circuiterie audio/cassette
- [MOS Technology 6522 — Wikipedia](https://en.wikipedia.org/wiki/MOS_Technology_6522)
  — référence générale du chip

Sources émulateurs (pour comparaison) :

- **Oricutron** (`~/oricutron/tape.c:1414‑1423`) — option `vsynchack`
  off‑by‑default, émule la modif RGB→TAPE
- **Oricutron** (`~/oricutron/ula.c:295‑312`) — commentaire waveform de la
  modif (12µs delay + 260µs pulse négative)
- **Phosphoric** (`src/main.c:852‑858`) — depuis 1.16.11, CB1 idle

---

**Résumé exécutif corrigé (2026-05-13 v2)** :

> Sur Oric d'usine, **CB1 = entrée signal cassette**, *pas* VSync.
> Le « `frame_wait` sur CB1 » de la Phase 9 d'Asteroids ne tourne que sur
> émulateurs ayant le `vsynchack` activé (ou Phosphoric ≤ 1.16.10 qui le
> faisait par défaut). La seule synchro trame portable Oric vrai + tous
> émulateurs sérieux est **Timer 1 du VIA en free‑run à 20 ms PAL**.
> Implémentation : ~10 lignes de diff dans `game.c` (Option A §5).
