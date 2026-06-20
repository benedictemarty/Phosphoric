# Validation clavier — détection des touches (ORIC-1 / Atmos)

Outils de détection et de validation automatisée du clavier Oric sous Phosphoric,
couvrant **toutes les touches et combinaisons** en mode **ORIC-1 (BASIC 1.0)** et
**Atmos (BASIC 1.1)**.

## Fichiers

| Fichier | Rôle |
|---|---|
| `keydetect.bas` | Programme BASIC **interactif** : affiche le code décimal de chaque touche pressée (`KEY$`). Outil de démonstration/diagnostic. |
| `keytest.bas` | Programme BASIC de **capture** : `POKE` le code ASCII de chaque touche détectée dans un buffer à `#2000`. Utilisé par le harnais. |
| `validate_keys.sh` | Harnais de **validation automatisée** : injecte une séquence de touches via `--type-keys`, dump la RAM, compare au vecteur attendu, pour les deux ROMs. |

## Usage

```bash
# Démonstration interactive (affiche les codes à l'écran)
make tools                                   # build bas2tap
./bas2tap tools/keytest/keydetect.bas -o /tmp/kd.tap --auto-run
./oric1-emu -r roms/basic11b.rom -t /tmp/kd.tap -f   # puis RUN, tapez des touches

# Validation automatisée complète (ORIC-1 + Atmos)
./tools/keytest/validate_keys.sh
```

## Principe du harnais

`keytest.bas` tourne dans l'émulateur :

```basic
10 A=#2000
20 K$=KEY$ : IF K$="" THEN 20      : REM attend une touche
30 POKE A,ASC(K$) : A=A+1          : REM capture le code dans le buffer
40 K$=KEY$ : IF K$<>"" THEN 40     : REM attend le relâchement
50 GOTO 20
```

Le harnais tape `RUN`, injecte la séquence de test, puis dump la RAM et compare
les octets de `#2000…` au vecteur attendu.

## Couverture validée (86 touches/combos × 2 ROMs = 172 vérifications, 0 erreur)

| Catégorie | Détail | Code(s) |
|---|---|---|
| Lettres | A–Z (CAPS actif → majuscules) | `$41`–`$5A` |
| Chiffres | 0–9 | `$30`–`$39` |
| Symboles | `! " # $ % & + - = . , /` | `$21`–`$2F` |
| Espace | SPACE | `$20` |
| RETURN | `\n` | `$0D` |
| ESC | `\e` | `$1B` |
| Flèches | `\u \d \l \r` (haut/bas/gauche/droite) | `$0B $0A $08 $09` |
| CTRL+lettre | `\Ca`…`\Cz` (sauf C) | `$01`–`$1A` |
| FUNCT+touche | `\Fx` | **touche de base** (le ROM BASIC ignore FUNCT) |
| SHIFT G/D | `\Lx` / `\Rx` | shift+chiffre=symbole, shift+lettre=majuscule |

## Comportements notables découverts (validés empiriquement)

- **CTRL+C = BREAK** : interrompt le programme BASIC. Non testable via `KEY$`
  dans une boucle ; validé séparément sur le LOCI File Manager (`keyb_char`=`$03`).
- **FUNCT ignoré par le ROM BASIC** : `FUNCT+1`→`'1'`, `FUNCT+A`→`'A'`. FUNCT
  n'a de sens que pour les logiciels lisant la matrice (ex. LOCI File Manager,
  où `FUNCT+1`→`KEY_F1`). Position matérielle FUNCT = **col 5 / row 4**.
- **SHIFT gauche et droit** : positions matérielles **distinctes**
  (LSHIFT col 4 / RSHIFT col 7, vérifié sur LFMV2 `keyb_matrix[4]` vs `[7]`),
  mais **équivalentes pour le ROM** (`\L1`=`\R1`=`'!'`).
- **`shift+2` = `'@'`** (et non `'"'`) sur le clavier Oric.
- **Flèches** : UP=`$0B`, DOWN=`$0A`, LEFT=`$08`, RIGHT=`$09`.
- **ORIC-1 et Atmos identiques** sur tout le jeu testé (`KEY$` présent et
  cohérent dans BASIC 1.0 comme 1.1).

## Limitations connues

- Deux **combos-modificateur consécutifs à même touche de base** (ex. `\L1\R1`)
  peuvent fusionner : le debounce clavier du ROM filtre la relâche d'une trame
  (~20 ms) comme du bruit. Contournement : intercaler `\pN` ou utiliser des
  touches de base distinctes.
- **DEL/BACKSPACE** n'a pas d'échappement `--type-keys` dédié (non couvert ici).
