# Repousser les limites de l'Oric : cinq extensions expérimentales pour la carte LOCI

*Comment un émulateur cycle-exact sert de banc d'essai matériel — avec l'ABI, les
registres et les schémas.*

---

> **À propos des photos.** Les clichés de la carte LOCI ci-dessous appartiennent à
> leurs auteurs (RAXISS / sodiumlb). Liens fournis à titre d'illustration et
> d'attribution — **à remplacer idéalement par tes propres photos** (tu possèdes une
> carte) ou par des images dont tu as l'autorisation de republication.
>
> - Carte LOCI, vues produit : <https://www.raxiss.com/article/id/38-LOCI>
>   (ex. `https://www.raxiss.com/images/resized/800x600-loci02g.jpg`)
> - Page revendeur / photos : <https://www.tindie.com/products/8bitclub/loci-oric-bus-expansion-port-and-floppy-emulator/>
> - Fil de développement (nombreuses photos in situ) :
>   <https://forum.defence-force.org/viewtopic.php?t=2593>

---

## 1. Le contexte matériel

L'**Oric-1** et l'**Atmos** (1983) reposent sur un **MOS 6502** à 1 MHz, 64 Ko
d'espace d'adressage dont le haut (`$C000-$FFFF`) est occupé par la **ROM BASIC**.
Pas de multiplication câblée, pas de banques mémoire, pas de stockage moderne :
tout passe par le **bus d'extension** en fond de machine.

La carte **LOCI** (*Lovely Oric Computer Interface*) de **sodiumlb** se branche sur
ce bus. Techniquement, c'est un dérivé du *Picocomputer 6502* (RP6502) : un
microcontrôleur **RP2040/RP2350** joue le rôle d'un **MIA** (*Media Interface
Adapter*) qui s'expose à l'Oric comme un périphérique d'entrées/sorties. Il émule
lecteurs de disquettes et cassettes, gère une carte SD, l'USB HID, et un **modem
Wi-Fi**. Deux surfaces d'adressage nous intéressent ici :

```
         Espace d'adressage Oric (64 Ko)
   $0000 ┌──────────────────────────────┐
         │ RAM                          │
   $0300 ├──────────────────────────────┤
         │ VIA 6522        $0300-$030F  │
   $0310 │ Microdisc WD1793 $0310-$031F │
         ├──────────────────────────────┤
   $0380 │ ACIA 6551 (LOCI) $0380-$0383 │ ← console série / modem
         ├──────────────────────────────┤
   $03A0 │ MIA (LOCI)      $03A0-$03BF  │ ← fenêtre API 32 octets
         ├──────────────────────────────┤
   $C000 │ ROM BASIC       $C000-$FFFF  │ ← cible de l'overlay de banque
   $FFFF └──────────────────────────────┘
```

L'émulateur **Phosphoric** (cycle-exact, C11) reproduit fidèlement cette carte, ce
qui autorise une démarche rare : **écrire la spec d'une extension, la coder, et la
valider par des tests déterministes — avant de toucher au fer à souder**. Détail
qui compte : l'auteur possède une carte LOCI, mais pas d'Oric. Le banc logiciel
n'est pas un luxe, c'est la seule salle d'essai.

## 2. L'ABI *fastcall* — comment le 6502 appelle la carte

Tout repose sur une **fenêtre de 32 registres** en `$03A0-$03BF` (le MIA). Voici la
carte réelle des registres utilisés par l'ABI (noms fidèles au firmware) :

```
 Offset  Adresse  Registre          Rôle
 ------  -------  ----------------  ------------------------------------------
  $00    $03A0    CONS_FLAGS        bit7 = TX libre, bit6 = RX prêt
  $01    $03A1    CONS_TX           écriture console (UART)
  $02    $03A2    CONS_CHAR         lecture console (consomme l'octet)
  $0C    $03AC    API_STACK         pointeur de xstack (pile d'arguments)
  $0D    $03AD    API_ERRNO_LO      errno bas
  $0E    $03AE    API_ERRNO_HI      errno haut
  $0F    $03AF    API_OP            ← ÉCRIRE ICI déclenche l'opération
  $12    $03B2    BUSY              bit7 = carte occupée
  $14    $03B4    API_A             valeur de retour A
  $16    $03B6    API_X             valeur de retour X
  $18    $03B8    API_SREG          retour 16 bits (SREG)
```

Le protocole d'appel (*fastcall*) tient en quatre temps :

```
   6502 (Oric)                         MIA (LOCI, µC)
   ───────────                         ──────────────
   1. push args ──► xstack ($03AC)
   2. set A/X (paramètres directs)
   3. write op ──► API_OP ($03AF) ───► déclenche le handler
                                       ├─ BUSY=1
      poll BUSY ($03B2) ◄──────────────┤  exécute
                                       └─ BUSY=0, remplit API_A/X/SREG, ERRNO
   4. read API_A/API_X ($03B4/$03B6) ◄─ résultat
```

Chaque valeur d'opcode encore libre est un **point d'entrée** pour une nouvelle
fonction. Les opérations standard vont de `$01` à `$98` (horloge, `open`/`read`/
`lseek`, répertoires, montage d'images, TAP…). C'est dans les opcodes **inutilisés**
que se logent nos cinq extensions :

```
  $A7  SET_BANK        banque commutable 16 Ko      (--loci-bank)
  $A8  STREAM_BANK     streamer d'assets            (--loci-bank)
  $A9  MATH            coprocesseur arithmétique    (--loci-coproc)
  $AA  ACIA_RELIABLE   mode ACIA fiable (seqlock)   (mode opt-in)
       + acia_stat_checked : handshake RX lossless  (--loci-acia-rx-nag)
```

> **Garde-fou by design.** Sans le drapeau d'activation correspondant, l'opcode
> renvoie `ENOSYS` (errno 13) — exactement comme un firmware non patché. Le logiciel
> Oric peut donc **détecter** la présence de l'extension et retomber sur ses propres
> routines. Le comportement par défaut de la LOCI reste **strictement** celui du
> matériel d'origine.

## 3. Coprocesseur arithmétique — `$A9`

**Le problème.** Le 6502 n'a ni multiplication, ni division, ni flottant câblés.
Chaque opération est une routine logicielle : lente, volumineuse, coûteuse en
cycles. Sur une machine à 1 MHz, une multiplication 16×16 se compte en centaines de
cycles.

**L'idée.** Déléguer le calcul au microcontrôleur de la carte, bien plus rapide, via
l'ABI *fastcall* existante — un seul opcode `$A9`, le sous-code d'opération dans
`API_A`, les opérandes sur la xstack, le résultat dans `API_A`/`SREG`.

```
   ; exemple conceptuel : A×B via le coprocesseur
   LDA #<op_mul  : STA API_A     ; sous-code d'opération
   ... push A, B sur la xstack ($03AC)
   LDA #$A9      : STA API_OF     ; déclenche MATH
   ; poll BUSY, puis lire le résultat 32 bits dans SREG
```

**L'implémentation.** Un fichier **isolé**, `src/io/loci_math.c` (`op_math`), branché
sur le dispatch. Entiers, flottants, opérations vectorielles. Gaté par
`--loci-coproc`. Couverture : **23 tests déterministes** (vecteurs entiers, flottants,
cas limites). Zéro aléatoire — mêmes entrées, mêmes sorties, condition d'un banc
reproductible.

## 4. Mode ACIA fiable — `$AA` (seqlock + ACK)

**Le problème.** L'**ACIA 6551** réel a un travers connu : si le 6502 ne lit pas le
registre de données à temps, l'octet reçu est **écrasé** par le suivant. À haut
débit — un modem Wi-Fi, par exemple — on perd des octets, et le lien devient
inexploitable. C'est fidèle au silicium, mais handicapant.

**La solution : un seqlock.** Un compteur de séquence de réception et un accusé
côté 6502. L'octet n'est **consommé** qu'une fois **acquitté** — jamais perdu, même
si la lecture tarde ou rate.

```
     Réception classique 6551 (destructive)
     ─────────────────────────────────────
     RX octet1 ──► RDR   (6502 n'a pas lu…)
     RX octet2 ──► RDR   ✗ octet1 ÉCRASÉ, perdu

     Mode fiable $AA (seqlock + ACK)
     ───────────────────────────────
        RXSEQ $0384  (compteur, incrémenté à chaque octet présenté)
        RXACK $0385  (accusé écrit par le 6502)

     RX octet1 ─► présente, RXSEQ++          consommé := (RXACK == RXSEQ)
     6502 lit octet1, écrit RXACK = RXSEQ ─► octet1 acquitté → avance
     RX octet2 ─► présente seulement si acquitté  ✓ aucun octet perdu
```

Le canal d'**émission** reste inchangé (toujours fiable). État porté par `loci_t`
(`acia_reliable`, `acia_rx_seq`, `acia_rx_presented`), opcode `$AA`, activation via
`API_A` bit0. Couverture : **+7 tests** (`test-loci-acia-miss`, 13 → 20) — DATA non
destructif, consommation *ACK-gated*, seqlock multi-octets **dans l'ordre**, et
surtout **survie à un raté de lecture**.

### 4bis. Handshake RX *lossless* — `acia_stat_checked` (`--loci-acia-rx-nag`)

Raffinement adjacent, calqué sur le firmware réel (`feature/acia-rx-lossless`). Sur
le vrai LOCI, l'`/IRQ` de l'ACIA est un signal de **niveau**, pas une impulsion. On
modélise ce niveau par un « nag » : tant que l'octet n'a pas été lu (`stat_checked`
faux), l'interruption est **ré-émise** périodiquement (défaut : tous les 1000
cycles), puis **se tait** dès que le 6502 a consulté le registre d'état.

```
   RDRF=1 (octet dispo) ──┐
                          │  nag: deassert+assert /IRQ tous les 1000 cyc
   /IRQ  ▁▔▁▔▁▔▁▔▁▔▁▔▁▔▁▔ │  tant que  RDRF && !stat_checked && RX-IRQ activée
                          │
   6502 lit STATUS ───────┘  stat_checked = true  ──►  /IRQ silencieux
```

Sans `--loci-acia-rx-nag`, l'ACIA reste **strictement** un 6551 (pas de nag).
Couverture dans `test-loci-acia-miss` : nag **observé avant** acquittement, **silence
après**, buffer vide ⇒ aucune IRQ.

## 5. Banque commutable de 16 Ko — `$A7` (`--loci-bank`)

**Le problème.** Comment donner plus de mémoire à une machine dont l'espace est
saturé par la ROM ?

**La solution.** Superposer temporairement 16 Ko de la RAM de la carte (*xram*) dans
la fenêtre `$C000-$FFFF`, là où siège la ROM.

```
        $A7 désactivé                 $A7 EN | SEL=n
   $C000 ┌───────────┐          $C000 ┌───────────────┐
         │ ROM BASIC │   ─────►        │ xram[n*0x4000]│  overlay (lecture)
   $FFFF └───────────┘          $FFFF  └───────────────┘
                                       └─ ROM intacte DESSOUS (non écrasée)
   base xram = SEL * 0x4000 ; SEL clampé à 0..3 (comme mia_set_bank)
```

**Le point clé : overlay non destructif.** La banque prend priorité en lecture (et
pour l'inspection mémoire) **sans jamais écraser** le tableau ROM. Une désactivation
restaure la machine **octet pour octet**. L'ancienne approche prototype par
`memcpy` + sauvegarde a été abandonnée au profit de cette superposition propre
(`memory_set_loci_bank()` dans `memory.c`).

**Double mode via `reset`.** L'activation par `$A7 EN` déclenche un **reset** : le
6502 relit son vecteur `$FFFC` **depuis la banque** (on peut donc *booter* du code de
banque). Le hot-swap (utilisé par le streamer `$A8`) bascule au contraire **sans
reset CPU** — prérequis absolu du double-buffering. Couverture : `test-loci`
**170/170** (+4 : enable/état, disable, gaté OFF → `ENOSYS`, clamp SEL 15→3) et un
`test-loci-bank-e2e` bout-en-bout.

## 6. Streamer d'assets — `$A8`

**L'idée.** Une fois la banque en place, y **déverser des données depuis un
fichier** (flash ou carte SD) en **un seul fastcall** : `lseek(SEEK_SET)` + `read`
→ banque 16 Ko, avec mapping optionnel en `$C000-$FFFF`. Un logiciel Oric peut alors
**dépasser les 48 Ko utiles** : overlays, décors, niveaux à la demande.

```
   Double-buffering (dépasser 48 Ko sans reset)
   ─────────────────────────────────────────────
   $A8 MAP=0 SEL=1 ─► précharge la banque 1 (invisible)   ┐ pendant
   (le 6502 continue d'exécuter/afficher la banque 0)      ┘ ce temps
   $A8 MAP=1 SEL=1 ─► bascule banque 1 en $C000 (hot-swap, PC intact)
```

Détails : `API_A` bit7 = `MAP`, bits3:0 = `SEL` (**0..3 valides ; >3 = `EINVAL`,
pas de clamp** contrairement à `$A7`). Arguments sur la xstack LIFO (`len, dst, off,
fd`), écriture bornée à la taille de banque, retour `AX` = octets lus. Deux chemins
de lecture : fichier hôte **et** image SD. Réutilise l'opt-in `--loci-bank`.

## 7. Le modèle de *tearing* — la question des deux cœurs

L'extension la plus subtile, et la plus honnête intellectuellement.

**La question.** Quand on bascule une banque **pendant** qu'un cycle de bus est en
cours, que latche le 6502 ? Sur l'émulateur mono-thread, le swap est **atomique** :
invisible, la question ne se pose pas. Mais le matériel réel a **deux cœurs** ; un
swap concomitant d'un accès mémoire peut produire un *tearing*.

**La réponse : modéliser explicitement le pire cas.** Avec `--loci-bank-tearing`,
un hot-swap `$A8 MAP` concomitant d'une **course de bus PHI2 perdue** fait latcher
l'**open-bus** au 6502 sur la **première lecture** de la fenêtre (comportement
one-shot), puis la banque prend le relais.

```
   Cycle bus PHI2  ─┬─ course gagnée ─► banque servie proprement (atomique)
                    │
                    └─ course PERDUE ─► 1re lecture = OPEN-BUS (valeur latchée)
                                        puis  ─► banque   (one-shot consommé)
   (réutilise loci_mia_serve_lost_sampled + jitter seedé, déterministe)
```

Le test associé est **auto-diagnostiquant** : il vérifie d'abord la **précondition**
(la course est bien perdue), puis que le modèle **arme** le drapeau de *tearing*,
puis que le one-shot est **consommé**. Un build incomplet échoue désormais sur une
assertion claire plutôt qu'un cryptique `torn != pat[0]`. Déterministe (jitter 0) :
trois builds propres, résultats identiques. `test-loci-bank-e2e` **8/8**.

## 8. Ce que l'exercice apprend

Cinq extensions, cinq additions **minimales** à une ABI existante, **zéro
régression** sur le comportement par défaut. Chacune est réversible (drapeau à
l'appui), gatée `ENOSYS` sans son opt-in, et adossée à des tests **déterministes**.

Le vrai enseignement tient peut-être là : sur une machine de 1983, la difficulté
n'est pas d'imaginer des fonctions modernes, mais de les **greffer sans trahir** le
comportement d'origine — et de le **prouver** avant de sortir le fer à souder.

L'émulateur cycle-exact cesse d'être un simple musée jouable : il devient un **banc
de prototypage matériel**. On y écrit la spec, on y code l'extension, on y passe les
tests… et le silicium n'arrive qu'en dernier, cahier de recette déjà rempli.

---

*Les cinq extensions vivent sur la branche `experiment/loci-coproc-acia-reliable`
de Phosphoric — opt-in, réversibles, hors version stable. À tester, critiquer,
améliorer.*

### Sources & liens

- Firmware / matériel LOCI (sodiumlb) : <https://github.com/sodiumlb/loci-firmware>,
  <https://github.com/sodiumlb/loci-hardware>, <https://github.com/sodiumlb/loci-rom>
- Manuel utilisateur (FR) : <https://github.com/sodiumlb/loci-hardware/wiki/LOCI-Mode-d'emploi>
  et <https://ceo.oric.org/loci-mode-demploi/>
- Revendeur & photos : <https://www.raxiss.com/article/id/38-LOCI>,
  <https://www.tindie.com/products/8bitclub/loci-oric-bus-expansion-port-and-floppy-emulator/>
- Fil de développement : <https://forum.defence-force.org/viewtopic.php?t=2593>
- Émulateur Phosphoric : <https://github.com/benedictemarty/Phosphoric>
