# L'étage de sortie audio de l'ORIC — ce que dit le schéma

**Relevé le** : 2026-09-11 (V2-E5, 2.0.0-alpha.5)
**Source** : schéma Oric-1 / Atmos reconstitué sous KiCad par Manoël Trapier
(986 Studio), feuille *PSG 1 Keyboard* (`audio.sch`), révision **Issue 6.1** —
[OricSchematics.pdf](https://www.986-studio.com/wp-content/uploads/2014/09/OricSchematics.pdf)
Complément : fil [« Oric's 8912 volume scale »](https://forum.defence-force.com/viewtopic.php?t=1357)
du forum Defence Force (mesure du mixage).

Ce document existe parce que deux « déviations assumées » du PSG reposaient sur
une incertitude qu'on pouvait lever en lisant le schéma. L'une était **fausse**,
l'autre **sans objet**. Une troisième reste ouverte, et on dit pourquoi.

## Le circuit

```
   AY-3-8912 (IC4)
   CH_C (1) ──┬───────────────── R2 ──┬──┬──────── C4 ──── SOUND_OUT ──► R33 ──► LM386 (+)
   CH_B (4) ──┤                 4,7 k │  │                                22 k
   CH_A (5) ──┘                       │  │
              └── R4 ── AGND        C5│  │R3
                  1 k              10 n│  │470
                                     AGND AGND
```

| Référence | Valeur | Rôle |
|-----------|--------|------|
| `R4` | 1 kΩ | charge commune : les **trois sorties sont reliées ensemble** dessus |
| `R2` | 4,7 kΩ | résistance série du diviseur de sortie |
| `R3` | 470 Ω | shunt du diviseur |
| `C5` | 10 nF | passe-bas, en parallèle sur `R3` |
| `C4` | *ambiguë* (notée « 2k2 ») | couplage vers l'ampli — bloque le continu |
| `R33` | 22 kΩ | polarisation de l'entrée + du LM386 |
| `IC2` | LM386 | ampli de puissance (gain 20 par défaut) |
| `C1` | 220 µF | couplage vers le haut-parleur `SP1` |

## Ce qu'on en tire, et ce qu'on en fait

### 1. Le mixage moyenne les canaux — notre `somme / 3` est correcte

Les trois sorties ne passent pas par des résistances de sommation séparées :
elles sont **directement reliées** au même point, chargé par `R4`. Deux sorties
au repos « tirent » donc sur celle qui est active. La mesure rapportée sur le
forum Defence Force le dit sans ambiguïté : un canal seul à 1 V donne **≈ 0,33 V**
au point de mixage, pas 1 V.

C'est exactement ce que fait le modèle : chaque canal traverse la table de volume
(non linéaire, mesurée), puis la somme est divisée par 3. La déviation
« le vrai AY somme des courants, donc la somme n'est pas linéaire » était
**infondée**. Verrouillé par `test_ay_parallel_mixing_averages_channels`.

### 2. Le passe-bas coupe à 37 kHz — hors sujet à 44,1 kHz

`C5` voit `R2 ∥ R3` = 427 Ω, d'où :

```
f_c = 1 / (2π · 427 Ω · 10 nF) ≈ 37,2 kHz
```

C'est **au-dessus de Nyquist** (22,05 kHz) : ce filtre ne façonne rien dans la
bande qu'on peut restituer. Et depuis que le PSG est cadencé au matériel avec
intégration par échantillon (V2-E5), l'ultrasonique est déjà traité. Rien à
modéliser : la déviation « pas de filtre passe-bas analogique » est **sans objet**.

Le diviseur `R2`/`R3` atténue de **−20,8 dB**, mais c'est du **gain**, repris par
le LM386 (×20) : ça ne change pas la forme du signal.

### 3. Le couplage bloque le continu — et c'est la seule certitude sur `C4`

Le signal du PSG est **unipolaire** : il va de 0 à +max, jamais en dessous.
Mesuré avant correction, trois canaux à plein volume donnaient un continu de
**+8188** — la moitié de l'amplitude. Aucun haut-parleur ne restitue ça, et
`C4` l'empêche de sortir.

L'émulateur bloque donc le continu (estimateur en virgule fixe, coupure
**≈ 1,7 Hz**, inaudible). Mesuré après : continu **+15**, signal **symétrique**
(−8232 / +8252 au lieu de 0 / +16383).

**Ce qu'on ne fait pas, et pourquoi.** La valeur de `C4` n'est pas déterminable
sur ce document : elle y est notée « **2k2** », sans unité, alors que tous les
autres condensateurs de la feuille portent la leur (`10n`, `47n`, `220uF`,
`10uF`). Les deux lectures plausibles donnent des circuits radicalement
différents :

| Hypothèse | `f_c` avec `R33 ∥ Z_in(LM386)` ≈ 15,4 kΩ | Conséquence |
|-----------|------------------------------------------|-------------|
| `C4` = 2,2 nF | ≈ **4,7 kHz** | passe-haut sévère : le son perd tous ses graves |
| `C4` = 2,2 µF | ≈ **4,7 Hz** | simple blocage du continu |

Un écart d'un facteur mille sur le résultat audible. On ne tranche pas au jugé :
seul l'effet **certain** (le blocage du continu) est modélisé. Une mesure sur
machine réelle, ou une photo lisible du PCB, lèverait le doute — et c'est la
seule chose qui le lèvera.

### 4. L'ampli et le haut-parleur ne sont pas modélisés

Le LM386 et le petit haut-parleur `SP1` ont leur propre réponse, non mesurée. Ce
que l'émulateur restitue est le **signal électrique** — ce qu'attend quiconque
écoute au casque ou sur des enceintes. Modéliser la réponse du haut-parleur
interne serait un autre travail, et il demanderait des mesures.

## Résumé

| Question ouverte avant | Après lecture du schéma |
|------------------------|--------------------------|
| Le mixage est-il non linéaire ? | **Non** : parallèle → moyenne. Le modèle était déjà juste. |
| Manque-t-il un passe-bas analogique ? | **Sans objet** : il coupe à 37 kHz. |
| Faut-il bloquer le continu ? | **Oui**, `C4` le fait sur la machine. Corrigé. |
| Quelle est la coupure exacte du couplage ? | **Indéterminée** sur ce document (facteur 1000 d'incertitude). Non modélisée. |
