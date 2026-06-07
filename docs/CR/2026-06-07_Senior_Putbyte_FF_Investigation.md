# Tâche #32 — Hypothèse `FF` confirmée empiriquement

**Suite à** : ta review v1.16.45 (`docs/CR/2026-06-07_Senior_BASIC10_CSAVE_*`), section §5 "Pourquoi `FF FF` en tête de header (capture 34aq)".
**Date** : 2026-06-07
**Verdict** : **ton hypothèse était juste byte-pour-byte.** Ma capture initiale était fidèle. Les `FF` viennent de la ROM qui dumpe la ZP non-initialisée.

---

## 1. Test discriminant exécuté

Instrumentation temporaire dans `tape_patches()` (branche
`investigate-putbyte-ff`, non mergée) :

1. À `writefileheader_entry` Atmos (`$E607`) : dump des 9 octets ZP du
   staging buffer `$02A8..$02B0`.
2. À `putbyte_entry` Atmos (`$E65E`) : log `(count, A, X, Y)` pour les
   12 premiers appels.

Scénario : ORIC Atmos cold-boot → `10 PRINT "HI"` → `CSAVE "T1"`.

---

## 2. Résultats bruts

```
INVESTIG_FF: WFH entry buffer dump @$02A8: FF 01 05 0E 05 00 00 FF FF
INVESTIG_FF: putbyte #1 A=$24 X=$11 Y=$01     ← sync (immediate, pas ZP)
INVESTIG_FF: putbyte #2 A=$FF X=$09 Y=$01
INVESTIG_FF: putbyte #3 A=$FF X=$08 Y=$01
INVESTIG_FF: putbyte #4 A=$00 X=$07 Y=$01
INVESTIG_FF: putbyte #5 A=$00 X=$06 Y=$01
INVESTIG_FF: putbyte #6 A=$05 X=$05 Y=$01
INVESTIG_FF: putbyte #7 A=$0E X=$04 Y=$01
INVESTIG_FF: putbyte #8 A=$05 X=$03 Y=$01
INVESTIG_FF: putbyte #9 A=$01 X=$02 Y=$01
INVESTIG_FF: putbyte #10 A=$FF X=$01 Y=$01     ← null sep mais ZP=$FF
INVESTIG_FF: putbyte #11 A=$54 X=$00 Y=$01     ← 'T' (filename)
INVESTIG_FF: putbyte #12 A=$31 X=$01 Y=$01     ← '1'
```

---

## 3. Corrélation 1:1 buffer ↔ putbyte

`WriteFileHeader` lit en reverse avec `LDX #9` / `LDA $02A7,X` / `DEX`.
Donc X=9 lit `$02B0`, X=8 lit `$02AF`, etc. jusqu'à X=1 lit `$02A8`.

| putbyte # | X | ZP addr | ZP byte | A captured | Match |
|-----------|---|---------|---------|-------------|-------|
| #2 | 9 | `$02B0` | `FF` | `FF` | ✅ |
| #3 | 8 | `$02AF` | `FF` | `FF` | ✅ |
| #4 | 7 | `$02AE` | `00` | `00` | ✅ |
| #5 | 6 | `$02AD` | `00` | `00` | ✅ |
| #6 | 5 | `$02AC` | `05` | `05` | ✅ |
| #7 | 4 | `$02AB` | `0E` | `0E` | ✅ |
| #8 | 3 | `$02AA` | `05` | `05` | ✅ |
| #9 | 2 | `$02A9` | `01` | `01` | ✅ |
| #10 | 1 | `$02A8` | `FF` | `FF` | ✅ |

**Corrélation parfaite, sur les 9 octets.** Chaque putbyte reçoit dans
`A` exactement ce que contient l'adresse ZP correspondante.

---

## 4. Mapping fonctionnel

| ZP addr | Rôle "officiel" | Valeur observée | Init par BASIC ? |
|---------|----------------|-----------------|------------------|
| `$02A8` | null sep | `FF` | **non** (laissé tel quel) |
| `$02A9` | start_lo | `01` | oui (= `$01` de `$0501`) |
| `$02AA` | start_hi | `05` | oui (= `$05` de `$0501`) |
| `$02AB` | end_lo | `0E` | oui |
| `$02AC` | end_hi | `05` | oui |
| `$02AD` | auto-flag | `00` | oui (CSAVE sans `,A` → 0) |
| `$02AE` | type | `00` | oui (BASIC program) |
| `$02AF` | padding[1] | `FF` | **non** |
| `$02B0` | padding[0] | `FF` | **non** |

Trois positions (`$02A8`, `$02AF`, `$02B0`) sont laissées intactes par
la ROM avant l'appel à `WriteFileHeader`. Si elles contenaient `00` au
cold-boot d'un Oric réel (RAM init à zéro), ces bytes seraient
`00 00 00` sur tape. Sur Phosphoric, l'init RAM utilise un pattern
mixte (`128×$00 + 128×$FF` par page, conformité Oricutron) → les
adresses `$02A8/$02AF/$02B0` tombent dans la moitié `FF`.

---

## 5. Implications

### Pour ton point §5 de la review v1.16.45

Tu écrivais :
> Ta capture était fidèle. C'est la ROM qui a émis `FF` parce que les
> octets réservés du buffer de staging n'étaient pas remis à zéro sur
> ton émulateur. Le "capture peu fiable" qui t'a poussé vers la
> reconstruction n'était peut-être pas un bug de capture du tout.

**Confirmé byte-pour-byte.** Ce n'était pas un bug de capture. C'est
le comportement réel de la ROM BASIC 1.1 Atmos. Sur un Oric réel
cold-boot avec RAM à zéro, ces bytes seraient `00`. Sur Phosphoric,
ils sortent `FF` à cause du pattern d'init RAM Oricutron-compat.

### Conséquence pour la stratégie TAP

Le choix entre **reconstruction** (sprints 34aq/34as/34at) et
**capture style Oricutron** dépend de la sémantique qu'on veut :

| | Reconstruction (actuel) | Capture Oricutron-style |
|---|---|---|
| Bytes émis | Layout standard `00 00 00 C7|00 ...` | Exactement ce que la ROM émet |
| Fidélité hardware | Faible (on normalise) | Maximale |
| Prédictibilité TAP | Haute | Dépend de l'état RAM (FF ou 00) |
| Couplage ROM-version | Faible (header buffer abstrait) | Aucun (purement data-driven) |
| Compatibilité CLOAD | Validée (round-trip OK) | À valider |
| LOC | ~150 (sprint 34aq actuel) | ~50 (juste un `fputc(cpu.A)`) |

À mon sens, **garder la reconstruction** est le bon choix pour
Phosphoric :

- Les TAPs produits sont byte-compatibles avec le format Oric canonique
  (vs AIGLE.TAP), pas avec des artefacts d'init RAM.
- Le snapshot à `writefileheader_entry` (34at) couvre BASIC + machine-code.
- Les ZP `$02A8/$02AF/$02B0` non-initialisées → reconstruction force `00`,
  ce qui correspond au comportement Oric réel cold-boot.

Mais l'option capture reste sur la table si tu préfères. Le sprint 34at
fix laisse les deux chemins viables.

### Conséquence pour `init_ram`

Question secondaire : Phosphoric pourrait initialiser les ZP critiques
(`$02A8`, `$02AF`, `$02B0`) à `00` lors du boot pour matcher un cold-start
Oric. Patch trivial, mais sémantique fragile (pourquoi ces 3 et pas
d'autres ?). Le sénior pourrait préférer "laisse la RAM telle qu'elle",
auquel cas la reconstruction reste la bonne approche.

---

## 6. Décision attendue

J'ai bien compris ton implicit : "**SI le test discriminant montre
A==FF aux positions pad et que la ZP elle-même est `FF`, alors ta
capture était saine — pas besoin de migrer vers Oricutron-style, le
bug initial n'en était pas un.**"

Le test montre ça exactement. Donc l'argument pour migrer vers la
capture (réduction LOC, simplicité) reste valable mais **n'est plus
forcé** par un bug. C'est un choix architectural.

Mon vote : **garder la reconstruction**, livrer le sprint 34at tel
quel, fermer la dette tâche #32 avec ce CR.

Si tu valides, je merge le tout sur main (déjà fait pour 34at, ce CR
sera commit suivant).

---

## 7. Instrumentation retirée

Le commit `investigate-putbyte-ff` contient les `log_info` temporaires.
Avant merge je rebase pour ne garder que le CR (les logs sont déjà
retirés du code).

— Fin du CR tâche #32
