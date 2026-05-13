# Note technique — Asteroids Oric‑1 : VSync via CB1 ≠ hardware réel

**Date** : 2026-05-13
**De** : équipe Phosphoric (bmarty)
**Pour** : équipe Asteroids Oric‑1
**Objet** : `frame_wait()` repose sur un câblage CB1 = VSync inexistant sur le hardware Oric

---

## TL;DR

Votre `frame_wait()` (`src/game.c:289-300`) polle `IFR bit 4` (CB1) en supposant
que la broche est pulsée à chaque trame par l'ULA. **Ce n'est pas le cas sur le
vrai Oric** : CB1 n'est pas câblé à la VSync. Phosphoric ≤ 1.16.10 émulait
cette convention par commodité, ce qui masquait le bug. À partir de
**Phosphoric 1.16.11** (2026-05-13), Phosphoric est aligné sur le hardware
réel et le programme **boucle à l'infini** comme dans Oricutron WIP `f79d5d4`.

Action requise côté Asteroids : remplacer la synchro CB1 par Timer 1 du VIA en
mode continu, ou par lecture mémoire d'un bit ULA.

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

## 3. Réalité hardware Oric‑1 / Atmos

Sur l'Oric, la broche CB1 du VIA 6522 :

- **N'est pas connectée** au signal VSync de l'ULA.
- **N'est pas pulsée** par l'ULA à chaque trame.

La VSync de l'ULA est exposée par :

1. **Lecture mémoire d'un bit ULA** (variable selon révision Oric‑1 / Atmos —
   à vérifier sur le schéma précis ; cf. notes de Fabrice Frances sur
   `oric.free.fr`). C'est la **méthode propre, sans dérive**.
2. **Polling du compteur de scan-line** dans l'ULA si exposé.

À noter : le propre `asteroids-oric1-48k-guide.md` du projet mentionne
explicitement cette voie dans sa section synchro Phase 9 (« Vrai signal VSync
issu de l'ULA, lisible via un bit du port matériel … C'est la méthode propre,
sans dérive, à privilégier dès que l'on a confirmé l'adresse exacte selon la
révision Oric‑1 ciblée. »). Le code actuel utilise un raccourci qui se révèle
non portable.

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

### Option B — Lecture bit VSync de l'ULA (la plus propre)

Confirmer sur schéma Oric‑1 / Atmos l'adresse mémoire et le bit exposant la
VSync ULA. Polling direct :

```c
#define ULA_STATUS  (*(volatile unsigned char*)0x????)
#define VSYNC_BIT   0x??

static void frame_wait(void)
{
    unsigned char i;
    for (i = 0; i < VSYNCS_PER_FRAME; i++) {
        /* attendre flanc montant */
        while ( (ULA_STATUS & VSYNC_BIT)) { }
        while (!(ULA_STATUS & VSYNC_BIT)) { }
    }
}
```

**Avantage** : zéro dérive, anti-tearing parfait.
**Inconvénient** : Phosphoric ne modélise pas encore ce bit ULA (à ajouter si
vous prenez cette voie — nous sommes preneurs de la spec exacte).

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

- v1.16.10 : CB1 pulsé à VSync (non conforme).
- **v1.16.11** : CB1 plus jamais piloté par l'émulateur — état idle high.
- Si vous voulez VSync hardware-accurate côté Phosphoric, ouvrir une issue
  pour qu'on expose le bit ULA dédié.

## 8. Contacts

- Émulateur Phosphoric : `/home/bmarty/Oric1`, commit version
  `EMU_VERSION 1.16.11-alpha`
- Code asteroids : `/home/bmarty/Oric asteroids`
- bmarty <bmarty@mailo.com>

---

**Résumé exécutif** : votre méthode VSync = CB1 est un raccourci qui ne
marche que sur émulateurs « complaisants ». Pour atteindre le hardware réel
et tous les émulateurs sérieux, basculez sur Timer 1 continu (10 lignes de
diff) ou attendons ensemble la spec du bit ULA pour la voie noble.
