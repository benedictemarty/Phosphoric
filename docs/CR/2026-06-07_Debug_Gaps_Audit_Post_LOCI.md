# Audit gaps debug post-LOCI (sprints 34y → 34c)

**Date** : 2026-06-07
**Référence** : 1.16.66-alpha (commit `6f31840`)
**Auteur** : audit senior staff
**Base de comparaison** : 5 gaps fermés 34s→34w (symboles, disasm paginé, breakpoints conditionnels, memory edit, TUI ncurses 6 panes)

---

## TL;DR

L'ajout massif de LOCI (~2 780 LOC, 4 TU) et la consolidation Microdisc/ACIA/PSG n'a **aucune contrepartie côté debugger** : 0 commande REPL pour `loci`, `fdc`, `acia`, `tape`, `mcp40`. Le disassembleur résout les symboles sur `PC` mais **pas sur les opérandes** (régression d'usabilité de la feature livrée en 34s). Le trace CPU (`--trace`) ignore aussi les symboles. Aucune capacité « rewind » ni screen-tracker. 1 bug parser symboles déjà connu (préfixe `0x` Format A) confirmé sur smoke test ; aucun autre bug parser détecté sur 6 lignes de tests croisés.

---

## Section 1 — Gaps identifiés

### P0-A : Disasm/trace ne résolvent pas les symboles d'opérande
- **Fichier:ligne** : `src/cpu/cpu6502.c:163-165` (cpu_disassemble), `src/utils/trace.c` (consommateur)
- **Scénario** : symbol table chargé avec `$E7AE CLOAD_HANDLER`. Commande `d E000 5` :
  ```
  $E002: 20 E2 00  JSR $00E2          ; 6 cyc
  ```
  Aucun symbole résolu sur la cible JSR/JMP/branch. Seul le label de la ligne courante (`addr` lui-même) est imprimé via `show_disassembly` → `symbol_lookup(addr)` (debugger.c:327). Idem `--trace` : sortie `00000002  F891  9A  TXS …` sans nom. Conséquence : la feature « symboles » livrée 34s est à moitié exploitable — le sens même de charger un `.lab` est de lire `JSR <CLOAD>`.
- **Effort** : S — modifier `addr_mode_fmt` pour accepter un callback `(uint16_t)->const char*` optionnel, ou wrapper dans `show_disassembly` qui post-process la chaîne (regex `$XXXX` → name).

### P0-B : Zéro introspection LOCI alors que c'est 2 780 LOC ajoutées
- **Fichier:ligne** : `src/debugger.c:464-465` (help) — uniquement `via`, `psg`. Aucune commande `loci`.
- **Scénario** : `--loci --loci-flash /tmp` actif. Le debugger ne permet pas de voir : registre op courant (`$03A0`), status (`$03B1`), file handle table (10 entrées dans `loci_fs.c`), mounts SD-IMG, dernière erreur errno, position read/write courante. Quand le boot Sedoric V4.0 stage 1 échoue, il faut relire les logs `--verbose` au lieu d'un snapshot. Gap critique pour la maintenance de tout le sprint 34a*.
- **Effort** : M — `loci_dump_state(loci_t*, FILE*)` + handler `loci` dans REPL (~120 LOC).

### P1-C : Aucune commande `fdc` / `disk` pour Microdisc WD1793
- **Fichier:ligne** : `src/io/microdisc.c` (191 LOC), aucune API `microdisc_get_state` exposée ; `src/debugger.c` n'inclut pas microdisc.h.
- **Scénario** : un boot DSK pend en commande `SEEK`. Impossible de lire en live track/sector/status WD1793, current drive, DRQ/INTRQ. On doit relancer avec `--trace` et grep.
- **Effort** : S — `printf` des 8 registres FDC + `md->status` + 4 drives mount info, ~50 LOC.

### P1-D : Aucune commande `acia` pour 6551
- **Fichier:ligne** : `src/io/acia6551.c` (571 LOC), aucun hook debugger.
- **Scénario** : `--serial modem:host:port --serial-irq-on-rdrf`. Pour diagnostiquer un blocage TX/RX (Minitel, V23), il faut `--serial-trace FILE` qui est post-mortem. Pas de snapshot live des registres status/control/cmd ni des FIFO TX/RX.
- **Effort** : S — affichage 4 registres + état FIFO + signaux RTS/DCD, ~40 LOC.

### P1-E : Pas de monitor cassette (position bytes/secondes)
- **Fichier:ligne** : `src/io/cassette.c` (aucun grep `position|tape_pos|tap_pos` ne matche → l'état interne n'est pas exposé).
- **Scénario** : tape CSAVE long en cours. Aucune visibilité sur byte courant écrit, durée écoulée, prochain header. Oricutron expose drive bar.
- **Effort** : XS — ajouter compteur déjà présent dans cassette_t + commande `tape`.

### P2-F : Pas de rewind / single-step inverse
- **Fichier:ligne** : N/A — savestate.c ne propose pas d'API ring-buffer in-memory.
- **Scénario** : après un step over qui déraille (JSR vers IRQ qui blat la stack), impossible de revenir 1 instruction. Standard sur Mesen, Bizhawk, et historiquement absent d'Oricutron mais demandé sur les forums.
- **Effort** : M — ring buffer de ~32 savestates compressés (RLE sur RAM) en mémoire, opcode `u` (undo) dans REPL.

### P2-G : Pas de screen tracker (cycle ↔ raster line)
- **Fichier:ligne** : N/A — `--screenshot-at C:FILE` permet un snap mais pas de breakpoint « break quand raster = 100 ».
- **Scénario** : debug d'un effet HIRES qui crashe sur une ligne précise. On veut `break raster 142`. Aujourd'hui on tape-key + screenshot.
- **Effort** : M — extension du moteur breakpoint pour type RASTER, hook dans la boucle frame.

### Bugs latents NON trouvés sur smoke-test
J'ai testé Format A (`$XXXX NAME`), Format B (`NAME = $XXXX`), Format C (`al C XXXX .NAME`), Format D (`NAME EQU $XXXX`), noms avec `.` et `_`, hex sans préfixe. Tous fonctionnent. Seul le bug `0xXXXX` préfix Format A (déjà identifié hors audit) reste ouvert. Aucune autre régression silencieuse détectée dans les 5 features 34s→34w.

---

## Section 2 — Recommandations sprint 34d (par ratio valeur/effort)

| # | Tâche | Valeur | Effort | Ratio |
|---|-------|--------|--------|-------|
| 1 | **P0-A** : résolution symbole sur opérande disasm + trace | Très élevée — réactive la feature 34s pour son usage principal | S | **★★★★★** |
| 2 | **P0-B** : commande REPL `loci` (dump op courant + handles + mounts + errno) | Élevée — couvre 2 780 LOC sans debug actuel, prochains sprints LOCI en bénéficient | M | **★★★★☆** |
| 3 | **P1-C** : commande REPL `fdc` (registres WD1793 + drives) | Élevée — investigation boot Sedoric/DSK régulière | S | **★★★★☆** |
| 4 | P1-D : commande `acia` | Moyenne — utile pour les sprints serial mais usage moins fréquent | S | ★★★☆☆ |
| 5 | P1-E : commande `tape` | Moyenne | XS | ★★★☆☆ |
| 6 | P2-F/G : rewind + raster break | Élevée mais complexité | M+M | ★★☆☆☆ |

**Top 3 reco sprint 34d** : P0-A + P0-B + P1-C (≈ S+M+S, ~250 LOC, gain immédiat tangible sur les sessions de debug LOCI/DSK).

---

## Section 3 — Ce qui est OK (no-action)

- **Parser symboles** : robuste sur 5 cas testés (sauf bug `0x` connu hors scope).
- **Memory edit `m addr = V1 V2 ...`** : présent (`debugger.c:600+`), fonctionne.
- **Breakpoints conditionnels** (`b addr if EXPR`) : présent (`debugger.c:670+`).
- **Disasm paginé** (`d +` / `d -` historique) : présent.
- **TUI 6 panes** : pas régressé (build `TUI=1` OK).
- **`--trace-irq`** : implémenté (main.c:2315-2330).
- **VIA / PSG dump REPL** : présents et corrects.
- **Format `.sym` / `.lab` / `.sym65`** : load OK, juste pas exploité côté disasm (gap P0-A).
