# Cadres de pile GOSUB / FOR — ORIC BASIC 1.1 (Atmos)

> Format des cadres poussés par les instructions `FOR` et `GOSUB` sur la pile
> matérielle 6502 (`$0100-$01FF`). **Toutes les valeurs de ce document ont été
> extraites empiriquement de l'émulateur Phosphoric**, jamais devinées : chaque
> champ est confirmé par variation contrôlée du programme BASIC et diff des dumps
> RAM (méthode différentielle). ROM de référence : `roms/basic11b.rom`
> (BASIC 1.1, ORIC Atmos, auto-détecté).

## Méthode de dérivation (reproductible)

Le programme est **tapé par le tokeniseur de la ROM elle-même** (via
`--type-keys-when`), ce qui garantit des tokens authentiques — on ne dépend pas
de l'outil `bas2tap` (dont le tokeniseur C est incomplet, cf. note en bas). Une
sentinelle `POKE #2FF,#AA` placée **dans** le sous-programme est capturée par
`--dump-ram-when` : au moment du dump, les cadres `GOSUB` **et** `FOR` sont
encore vivants sur la pile (capture avant le `RETURN`).

```bash
PROG='500 FORI=1TO9\n510 GOSUB600\n520 NEXTI\n530 END\n600 POKE767,170\n610 RETURN\nRUN\n'
./oric1-emu-web -r roms/basic11b.rom --headless \
  --type-keys-when BC9A:52:"$PROG" \
  --dump-ram-when 2FF:AA:/tmp/stk.bin -c 30000000
```

Le cadre se localise en **scannant le marqueur** (`$8D` pour FOR, `$9B` pour
GOSUB) dans la page pile — robuste, indépendant de la valeur de SP au moment du
dump.

Deux garde-fous indispensables dans cet environnement :

1. **Toujours** `--headless` + une condition de sortie déterministe
   (`-c N`, `--dump-ram-at`, `--dump-ram-when`). Un lancement en mode SDL/temps
   réel réel échoue silencieusement sans display — ce n'est pas une instabilité
   de l'émulateur.
2. Pour observer un pointeur texte qui **franchit une page** (lever l'ambiguïté
   d'endianness), numéroter la boucle FOR **haut** (p. ex. `500`) avec des
   lignes de remplissage **basses** : sinon la ROM trie la boucle en tête de
   programme et son texte ne quitte jamais la page `$05`.

## Cadre FOR — 18 octets

Marqueur `$8D` (token `FOR`) à l'adresse **basse** du cadre (poussé en dernier).
Offsets comptés depuis le marqueur, vers les adresses croissantes :

| Offset | Taille | Champ | Ordre octets | Confirmation empirique |
|-------:|:------:|-------|:------------:|------------------------|
| +0      | 1 | Marqueur `$8D` (token `FOR`) | —             | constant sur tous les runs |
| +1..2   | 2 | Pointeur variable de boucle (adresse **valeur** de la variable = descripteur VARTAB + 2) | little-endian | suit VARTAB+2 : `$0539` → `$0548` → `$0651` |
| +3..7   | 5 | Valeur du **STEP** (flottant MS 5 octets, **magnitude seule**) | flottant MS | `1.0`→`2.0` : `81 80 00 00 00` → `82 80 00 00 00` |
| +8      | 1 | **Signe du STEP** | —             | `$01` (positif) → `$FF` (négatif) |
| +9..13  | 5 | Valeur limite **TO** (flottant MS 5 octets) | flottant MS | `9.0`→`7.0` : `84 10 00 00 00` → `83 60 00 00 00` |
| +14..15 | 2 | **Numéro de ligne** du `FOR` | little-endian | `10`→`15`→`500` : `0A 00` / `F4 01` |
| +16..17 | 2 | **Pointeur texte** (position du `$00` terminal de l'instruction FOR) | **big-endian** | `$050B`→`$0513`→`$0623` |

Points non triviaux, impossibles à deviner :

- **Endianness mixte dans la même trame.** Le pointeur variable et le numéro de
  ligne sont stockés *little-endian*, mais le pointeur texte est stocké
  **big-endian** (poids fort à l'adresse basse) — reflet de l'ordre de push de
  la ROM.
- **Le flottant STEP ne porte pas son signe.** La magnitude est stockée en
  +3..7 et le signe vit dans l'octet **+8** (drapeau ±1 façon Microsoft BASIC).
  `STEP 1` et `STEP -1` donnent le même flottant `81 80 00 00 00`, seul +8 change
  (`$01` vs `$FF`).

### Exemple annoté

Programme `500 FORI=1TO9 : GOSUB600 : NEXTI` (ligne 500, texte en page `$06`),
capture à la sentinelle. Cadre FOR trouvé à `$01ED` :

```
$01ED: 8D                 marqueur FOR
$01EE: 51 06              varptr  = $0651  (valeur de I, VARTAB+2)   [LE]
$01F0: 81 80 00 00 00     STEP    = 1.0    (magnitude)
$01F5: 01                 signe   = +
$01F6: 84 10 00 00 00     TO      = 9.0
$01FB: F4 01              ligne   = $01F4 = 500                       [LE]
$01FD: 06 23              textptr = $0623  (fin de l'instruction FOR) [BE]
```

## Cadre GOSUB — 5 octets

Marqueur `$9B` (token `GOSUB`) à l'adresse basse du cadre :

| Offset | Taille | Champ | Ordre octets | Confirmation empirique |
|-------:|:------:|-------|:------------:|------------------------|
| +0    | 1 | Marqueur `$9B` (token `GOSUB`) | —             | constant |
| +1..2 | 2 | **Numéro de ligne** du `GOSUB` | little-endian | `20`→`25`→`510` : `14 00` / `FE 01` |
| +3..4 | 2 | **Pointeur texte** de reprise (dans la ligne du GOSUB) | little-endian | `$0511`→`$0519`→`$0629` |

Contrairement au cadre FOR, le pointeur texte du GOSUB est stocké
**little-endian** — asymétrie réelle observée, non postulée.

### Exemple annoté

Même capture, cadre GOSUB à `$01E6` (ligne 510, cible 600) :

```
$01E6: 9B                 marqueur GOSUB
$01E7: FE 01              ligne   = $01FE = 510                       [LE]
$01E9: 29 06              textptr = $0629  (reprise dans la ligne)    [LE]
```

## Format des flottants (rappel Microsoft BASIC 5 octets)

`exposant biaisé` (biais `$80`), puis 4 octets de mantisse (bit de poids fort
implicite = 1). Valeur ≈ `(1 + mantisse/2^32) × 2^(exposant-129)`. Exemples
vérifiés dans les cadres ci-dessus :

| Octets | Valeur |
|--------|:------:|
| `81 80 00 00 00` | 1.0 |
| `82 80 00 00 00` | 2.0 |
| `83 60 00 00 00` | 7.0 |
| `84 10 00 00 00` | 9.0 |

## Note — outil `bas2tap`

L'outil C `tools/bas2tap.c` (via `tap_from_basic` → `basic_tokenize_line`) n'est
**pas** un stub, mais son tokeniseur est **incomplet** : au moins le cas
« mot-clé collé à une lettre » (`FORI`) laisse le mot-clé en ASCII brut au lieu
d'émettre le token, produisant un `.tap` qui ne s'exécute pas correctement — tout
en renvoyant « Conversion successful! ». Dette technique identifiée ; **ne pas**
s'appuyer dessus pour générer des programmes de test. La méthode fiable est de
faire tokeniser par la ROM via `--type-keys` (cf. ci-dessus).
