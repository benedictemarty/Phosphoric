# Mon journal — démarche & méthodologie sur Phosphoric

> Fichier tenu **en temps réel** : il décrit comment je travaille sur l'émulateur
> Phosphoric (investigation, implémentation, tests), les **difficultés** rencontrées
> et comment je les tranche. Objectif : que tu puisses comprendre et reproduire ma
> démarche.
>
> Règle de conduite que j'applique ici : **mesurer, ne pas inventer, ne pas supposer.**
> Quand je ne sais pas, je le dis ; quand j'affirme, c'est que je l'ai mesuré.

---

## Principes de méthode (constants)

1. **Lire avant d'écrire.** Je localise le code existant (grep + lecture ciblée)
   avant de proposer quoi que ce soit. Je ne décris jamais une fonctionnalité sans
   avoir vu la fonction dans le source.
2. **Mesurer plutôt que supposer.** Une hypothèse (« l'écran devrait afficher X »)
   n'a de valeur qu'une fois vérifiée par une exécution réelle (dump mémoire,
   screenshot, test).
3. **Isoler la cause avant d'accuser un composant.** Un échec de test ou de build
   est d'abord suspecté d'être un artefact (build incrémental, objets stale) avant
   d'être imputé au code métier. Je le prouve par `git stash` / `make clean`.
4. **Distinguer faux positifs et vrais problèmes.** Les diagnostics du linter LSP
   isolé (chemins d'include non résolus) ne sont pas des erreurs de compilation ;
   seul le build réel `make` fait foi.
5. **Tout tracer.** Tests + CHANGELOG + fichiers de suivi à chaque modification,
   conformément aux règles du projet.

---

## Session en cours — Ajout d'un « screenshot texte »

### 1. La demande
« Existe-t-il un screenshot texte ? » puis « implémente les deux » :
- **A** — exporter l'écran comme **image ANSI true-color** (pixels → couleurs de fond
  ANSI dans un terminal) ;
- **B** — exporter le **contenu texte réel** de l'écran (les caractères affichés).

### 2. Ce que j'ai trouvé en explorant (mesuré, pas supposé)
- `video_export_ascii()` **existait déjà** dans `src/video/export.c` mais :
  - n'était **pas** branché au CLI (`--help` ne le mentionnait pas) ;
  - n'était appelé **que** par les tests ;
  - produit une **image ANSI** (chaque pixel = un espace coloré), pas un dump texte.
- Le **contenu texte réel** (option B) n'existait pas.
- Les helpers `textmode_*` (`src/video/textmode.c`, base écran `$BB80`, 40×28)
  existent mais sont **orphelins** (aucun header, aucun appelant) → je ne m'appuie
  pas dessus, je mets la logique dans le module d'export.

### 3. Conception (minimale, testable, sans surface inutile)
Deux nouvelles fonctions dans `export.c` / `export.h` :
- `video_export_ascii_file(vid, filename, sx, sy)` — wrapper fichier de l'existant (A) ;
- `video_export_screen_text(memory, fp)` — lit `$BB80` (40×28), décode chaque octet
  par `octet & 0x7F` (masque le bit vidéo inverse), remplace les codes de contrôle
  (< 0x20) par un espace, rtrim des espaces de fin (B).

Deux options CLI de sortie, calquées sur `--screenshot` :
- `--screenshot-text FILE` (B) ;
- `--screenshot-ansi FILE` (A).

### 4. Difficultés rencontrées et comment je les ai tranchées

| # | Difficulté | Diagnostic | Résolution |
|---|-----------|-----------|-----------|
| 1 | Build headless en échec de **link** (`SDL_*` indéfinis) | Objets `.o` d'un build SDL2 antérieur mélangés avec un link headless | `make clean` puis rebuild propre |
| 2 | Diagnostics **clang** « file not found / unknown type » en rafale | Faux positifs : le LSP isolé n'a pas `-Iinclude` | Ignorés ; **le build réel `make` est l'autorité** |
| 3 | **Dump texte quasi vide** à 2 000 000 cycles | Mesure du contenu brut `$BB80` = `$FF` partout → écran pas encore rempli, pas un bug de décodage | Mesuré à 3M/5M/8M cycles → écran de boot stable et lisible |
| 4 | Le `©` d'« © 1983 TANGERINE » sort en `` ` `` | Charset ORIC : code `0x60`. Mon décodage suppose l'ASCII standard | **Limite assumée et documentée** (approximation ASCII), pas un défaut caché |
| 5 | `test-control-dispatch` : **7 échecs** dans `make tests` | Suspicion d'artefact plutôt que régression | Prouvé par la mesure : `make clean && make tests` (séquence canonique) → **0 échec**. L'échec n'apparaît **que** si l'on précède d'un `make SDL2=0` (mélange d'objets headless/SDL2). Reproduit **à l'identique sur HEAD propre** (`git stash` + même séquence → 19/7) → **artefact préexistant du Makefile, PAS mon code ni une régression** |

### 5. Validation par la mesure
- `make test-video` : **16/16** (dont 2 nouveaux tests : `test_ascii_export_file`,
  `test_screen_text_export`).
- Exécution réelle Atmos à 3M cycles, `--screenshot-text` :
  ```
                                      CAPS
    ORIC EXTENDED BASIC V1.1
    ` 1983 TANGERINE

     37631 BYTES FREE
  ```
- `--screenshot-ansi` : ~264 Ko, séquences `ESC[48;2;R;G;Bm` présentes, structure de
  l'écran visible dans le terminal.
- Suite complète `make clean && make tests` (séquence canonique) : **0 échec**.
- Découverte annexe (mesurée) : un `make SDL2=0` suivi d'un `make tests` fait échouer
  `test-control-dispatch` (7/26) par mélange d'objets ; **artefact du Makefile
  préexistant** (reproduit sur HEAD propre), sans lien avec cette fonctionnalité.

### 6. Ce que je ne sais pas / limites honnêtes
- Le dump texte **suppose le jeu de caractères standard ORIC** ; un charset
  redéfini par un programme ne sera pas résolu (les octets restent interprétés en
  ASCII). C'est documenté dans l'en-tête de la fonction.
- Le dump lit toujours les 28 lignes de `$BB80`, quel que soit le mode (TEXT/HIRES) :
  en HIRES seules les 3 dernières lignes texte sont réellement à l'écran, mais le
  buffer `$BB80` est lu tel quel.
- ~~Les options sont de sortie, pas encore « à un cycle donné ».~~ **Comblé** :
  voir l'itération 2 ci-dessous.

### 7. Itération 2 — variantes « à un cycle donné » (v1.93.0-alpha)
La limite notée au point 6 (« pas d'équivalent `--screenshot-at` ») est **comblée** :
- `--screenshot-text-at C:FILE` et `--screenshot-ansi-at C:FILE`, calqués sur le
  `--screenshot-at` existant, **réutilisant** le helper `cli_split_cycles_file()` déjà
  mutualisé (v1.91.1) — pas de duplication du parsing `CYCLES:FILE`.
- **Méthode** : je suis parti du code exact de `--screenshot-at` (parsing + bloc dans la
  boucle) et je l'ai décliné, pour garantir un comportement d'erreur **identique**
  (format sans `:` → fatal rc 1, même message via `optname`).
- **Mesuré** : `--screenshot-text-at 3000000:FILE` écrit au cycle 3015456 le même écran
  Atmos lisible ; malformés → rc 1 vérifié **sans pipe** (un `| grep` aurait masqué le
  vrai code retour — piège évité).
- **Tests** : filet CLI `test-cli-parsing` **29/29** (+4 cas : 2 malformés fatals + 2
  contrôles positifs « fichier non vide »), sur le modèle des cas `--screenshot-at`.

---

_Dernière mise à jour : itération 2 (screenshot texte/ANSI à un cycle donné, v1.93.0)._
