# Mémo équipe — Rétractation du bug report WD1793 « track+1 » + outillage FDC_TRACE (v1.16.84-alpha)

**Date** : 2026-06-12
**Sprint** : 38
**Audience** : équipe Phosphoric
**TL;DR** : le bug report externe « WD1793 lit la piste N+1 après un scroll VRAM »
(SCUMM-Oric, 2026-05-19) est **rétracté — Phosphoric n'a jamais eu ce bug**.
L'enquête a livré un nouvel outil de diagnostic (`FDC_TRACE=1`) et révélé une
**vraie** régression de contrat sur `--dump-ram-at` (48 Ko au lieu de 64 Ko),
corrigée. Deux leçons process en §5.

---

## 1. Le bug rapporté

Le projet SCUMM-Oric rapportait (rapport formel + bisection) : après une
routine de scroll VRAM pure (~130k cycles, zéro accès I/O), tout
`fdc_load_room` lisait la piste N+1. Ni Force Interrupt ($D0) ni RESTORE ne
« corrigeaient ». L'hypothèse pointait notre machine à états WD1793
(timeout moteur, STEP parasite cycle-based).

Le rapport était crédible : symptôme reproductible, bisection propre,
données byte-exact à l'appui (LOAD_BUF = contenu piste 3 vérifié 5300/5300).

## 2. La méthode : trace FDC avec PC 6502

Plutôt que d'auditer la machine à états à l'aveugle, on a instrumenté :

```
FDC_TRACE=1 ./oric1-emu --headless ... 2>trace.log
```

Sortie (nouveau, sprint 38) :

```
[FDC] PC=055F cyc=8692214 write $0313 = 02      ← qui écrit, quand, quoi
[FDC] seek target=2 (c_track=0 track_reg=0 data=02)
[FDC] READ c_track=2 sector=1 side=0 ok
```

- **main.c** (`io_write_callback`) : chaque écriture $0310-$031F avec PC + cycle ;
- **storage/disk.c** : chaque SEEK (cible + état interne) et chaque READ
  (piste/secteur/side, found/NOT_FOUND) ;
- coût nul si la variable d'env est absente (`getenv` évalué une fois).

## 3. Verdict : le 6502 ne nous parlait plus

La trace du scénario incriminé montrait le premier chargement (piste 3,
21 secteurs, nominal)… puis **plus rien**. Aucune commande FDC après les
scrolls. Le « mauvais contenu » était le résidu du chargement précédent.

Cause racine côté SCUMM-Oric : leur harnais de test comptait sa boucle de
scroll dans le registre X, clobbré par une routine appelée en `jsr` →
boucle infinie → le second chargement n'était jamais exécuté (variable
témoin `cam_x` = 170 au lieu de 20 dans le dump RAM). Compteur déplacé en
mémoire → seek piste 2 + 9 READ corrects, byte-exact 2273/2273.

Rétractation formelle archivée côté rapporteur :
`SCUMM/docs/phosphoric_bug_report_wd1793_scroll.md`.

**Phosphoric est hors de cause. Aucune modification de l'émulation WD1793
n'a été nécessaire — et aucune ne devait l'être.**

## 4. La vraie trouvaille : régression de contrat `--dump-ram-at`

En route, les validations memdump du rapporteur cassaient avec « dump trop
court (49152, attendu 65536) ». Chaîne causale :

1. **Origine** : `fwrite(emu->memory.ram, 1, 0x10000, f)` sur un tableau de
   49152 octets — *buffer over-read* (UB) qui dumpait « par chance » 64 Ko
   en lisant `rom[]` adjacent dans la struct. Les consommateurs se sont
   construits sur ce comportement.
2. **b2af997** (2026-05-14, passe cppcheck) : l'overflow est tronqué à 48 Ko.
   Fix mémoire **correct**, mais le contrat documenté (`--help` : « Dump
   64KB RAM ») n'a pas été mis à jour ni les consommateurs vérifiés.
3. **Un mois d'invisibilité** : le binaire déployé n'avait pas été rebuilé
   depuis — la régression dormait dans HEAD sans être livrée.
4. **Sprint 38** : contrat 64 Ko restauré proprement — $0000-$BFFF = RAM
   brute, $C000-$FFFF = **vue CPU bankée** (BASIC ROM / overlay Microdisc /
   upper RAM), lue sans effet de bord (la page I/O n'est pas traversée).
   Le dump interactif **F7** est aligné sur le même contrat.

## 5. Leçons process

1. **Binaire déployé ≠ HEAD.** La régression dump a vécu un mois parce que
   `oric1-emu` n'était pas rebuilé après chaque commit. Proposition :
   rebuild systématique en fin de sprint (ou CI qui produit le binaire), et
   les suites externes (SCUMM-Oric) tournent contre ce binaire-là.
2. **Un fix de sécurité mémoire peut casser un contrat.** Quand on corrige
   un overflow détecté par un outil, vérifier ce que le code *voulait*
   faire (ici : dumper 64 Ko) et mettre `--help`/doc/consommateurs en
   cohérence — pas seulement faire taire l'outil.
3. **Exiger une trace pour tout bug report FDC.** Politique proposée :
   tout rapport externe sur le sous-système disque doit joindre une sortie
   `FDC_TRACE=1`. Ça aurait tué celui-ci en dix minutes au lieu de trois
   semaines de fausse piste côté rapporteur.

## 6. Backlog ouvert

- [ ] `test_renderer_init_headless` échoue en profil `SDL2=1` (mode 0x3 vs
  0x1 attendu) — pré-existant, indépendant du sprint 38, tracé au ROADMAP.

## 7. Références

| Quoi | Où |
|------|-----|
| Trace FDC | `src/storage/disk.c` (`fdc_trace_enabled`, SEEK, READ), `src/main.c` (écritures + PC) |
| Fix dump 64 Ko | `src/main.c` (`--dump-ram-at` + F7) |
| Commits | `f7f988c` (sprint 38), `b2af997` (origine régression, 2026-05-14) |
| Rétractation côté rapporteur | `SCUMM/docs/phosphoric_bug_report_wd1793_scroll.md` |
| Version | 1.16.84-alpha — CHANGELOG / ROADMAP / VERSION_TRACKING à jour |
