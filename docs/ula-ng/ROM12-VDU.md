# BASIC 1.2 NG — vecteur de sortie RAM (style BBC) pour le VDU ULA-NG

> **Statut : DESIGN / EXPLORATION — non implémenté.** Branche
> `feature/ula-ng-rom12-vdu`, hors `main`. Piste pour donner à l'Oric le
> vecteur de sortie revectorable qu'il n'a pas, via une ROM « BASIC 1.2 »
> patchée. Prérequis lu : `VDU.md` (§4, conclusion sur le hook OSWRCH).

## 1. Pourquoi une ROM 1.2

L'ergonomie visée — `PRINT CHR$(…)` alimente `NG_VDU` **sans POKE**, façon BBC —
est impossible proprement sur l'Oric d'origine (voir `VDU.md` §4) :
- **pas de vecteur de sortie par caractère** (l'`OSWRCH`/`WRCVEC` du BBC n'existe pas) ;
- le hook imprimante `$02F1`/`$023E` **ne se déclenche pas** (flag remis à 0 par
  l'IRQ 50 Hz ; capture Centronics au niveau matériel) et serait de toute façon
  **~1000× trop lent** (poignée de main) pour les données en masse.

La solution propre et **rapide** : patcher une ROM (« 1.2 ») qui **ajoute** un
vecteur de sortie en RAM (`NGVEC`). La routine de sortie y passe par `JMP (NGVEC)` ;
par défaut `NGVEC` → sortie normale (aucune différence) ; un programme le
revectore vers son handler (`STA $0357` → VDU) — **dynamique, pleine vitesse,
style BBC**. La gestion des collisions de codes de contrôle devient l'affaire du
**handler** (tout renvoyer / filtrer / échappement), plus de la ROM.

## 2. Points de patch identifiés (désassemblage `basic11b.rom`, Atmos)

- **Sortie caractère BASIC** : `$CCD9`. Début :
  `BIT $2E ($24 $2E) / BMI $CD10 ($30 $33) / PHA / CMP #$20 …`.
  Contient déjà `BIT $02F1 / JSR ($023E)` (redirection imprimante conditionnelle).
- **Vecteurs RAM existants** (initialisés au boot) : `$023E: JMP $F5C1`
  (imprimante), `$0241: JMP $F865`, `$0244: JMP $EE22` (IRQ). → il existe une
  routine ROM d'init de ces vecteurs au boot : **candidate pour initialiser
  `NGVEC`** aussi.
- **Ne PAS reutiliser `$023E`** : c'est l'imprimante ; le détourner enverrait
  *tout* l'affichage au port imprimante. Ajouter un **`NGVEC` dédié neuf**.
- **RAM libre** : `$0400-$04FF` = `$55` au boot (jamais écrite par la ROM) →
  emplacement sûr pour un handler installé par le programme. `NGVEC` lui-même :
  choisir 2 octets libres en page 2 (à confirmer inutilisés).
- **Interdits** : ne toucher **aucune** adresse des patches cassette de
  l'émulateur (CLOAD/CSAVE, auto-détection 1.0/1.1) — cf. `rom_patches_t`
  (`include/emulator.h`) et `src/io/cassette.c`.

## 3. Plan de patch (chirurgical)

1. **Détour de l'entrée** `$CCD9` → `JMP stub` (stub en espace libre ROM).
2. **stub** : `JMP (NGVEC)` (indirection revectorable).
3. **`NGVEC` par défaut** → routine qui rejoue les octets déplacés
   (`BIT $2E`) puis `JMP $CCDB` (poursuite de la routine d'origine). = sortie
   normale inchangée.
4. **Init au boot** : étendre la routine d'init des vecteurs RAM pour poser
   `NGVEC` = défaut (sinon indirection sur RAM non initialisée = crash).
5. Recalcul d'un éventuel checksum ROM si le boot le vérifie (à vérifier).

## 4. Livrables prévus

- `tools/make_basic12ng.py` — applique le patch à `roms/basic11b.rom` →
  `roms/basic12ng.rom` (reproductible, diff vérifiable, octet par octet).
- `roms/basic12ng.rom` (dérivée — voir §6 copyright).
- Démo `demos/ula-ng/ng_vdu_wrcvec.bas` : installe un handler en `$0400`
  (`STA $0357 : <chaîne sortie normale>`), pose `NGVEC` dessus, puis
  `PRINT CHR$(22);CHR$(1)` pilote le VDU **vite et sans POKE**.
- Tests : boot intact, `CLOAD` OK, auto-détection OK, le revectorage fonctionne
  (garde headless).

## 5. Périmètre de départ suggéré (quand on reprendra)

Ordre recommandé (le plus sûr d'abord) :
1. **Prouver le vecteur seul** : patch `NGVEC` + revectorage démontré (ex.
   passer la sortie en MAJUSCULES), boot/CLOAD intacts. Aucune dépendance VDU.
2. Handler VDU côté démo (`STA $0357`) — le VDU reste piloté par du logiciel.
3. (Option) handler + activation intégrés à la ROM (« BASIC 1.2 sait parler au
   VDU ») — patch plus gros, à ne faire qu'après 1 & 2 sûrs.

## 6. Réserve copyright

La ROM Oric BASIC est **sous copyright** (Tangerine/Oric). `basic12ng.rom` est
une **œuvre dérivée** : même statut que la ROM d'origine déjà présente dans le
dépôt. Usage personnel. Le dépôt versionne le **script de patch** et le **diff**,
la redistribution de la ROM binaire relève du même cadre que `basic11b.rom`.

## 7. Références

- `docs/ula-ng/VDU.md` (§4 conclusion OSWRCH).
- BBC Micro MOS — `WRCVEC` ($020E) / `OSWRCH`.
- Désassemblage : `$CCD9` (sortie car.), `$023E`/`$0244` (vecteurs RAM), init boot.
