# Demande à l'équipe Phosphoric — capture déclenchée par un ÉTAT mémoire

**Date** : 2026-07-18
**Demandeur** : projet SCUMM-Oric (banc de tests headless)
**Type** : demande de fonctionnalité (feature request)
**Priorité** : haute pour le CI SCUMM (débloque un refactor bloqué)

---

## 1. Le besoin, mesuré

Le banc de tests de SCUMM-Oric utilise Phosphoric en headless pour valider le
rendu et l'état du jeu. **Toutes les captures sont déclenchées par un nombre de
cycles fixe** :

- `--screenshot-at C:FILE`
- `--dump-ram-at C:FILE`
- `--screenshot-text-at C:FILE`

Or le cycle auquel un état de jeu donné apparaît **dépend de la taille et du
layout du binaire testé**. Dès qu'on modifie le code (même un refactor neutre
qui déplace des variables en mémoire), la timeline se décale : la capture au
cycle N montre une **autre frame** → le test échoue alors que le comportement
est correct. C'est une fragilité documentée côté SCUMM (« §S306 »).

Conséquence concrète et actuelle : un refactor propre (unifier l'état d'acteur
en une source unique) est **bloqué**, car déplacer des tables casse des tests
byte-exact/screenshot — non par changement de comportement, mais par décalage
de timing. Les contournements possibles côté SCUMM sont tous mauvais :
- laisser des « trous » BSS pour figer les adresses (dette qui s'accumule) ;
- choisir des adresses fixes à la main (= deviner le plan mémoire).

## 2. Ce qui manque dans Phosphoric

**Aucune capture déclenchée par un ÉTAT** (une condition mémoire), seulement par
le temps (cycles). Il n'existe pas d'équivalent « capture quand `RAM[$XXXX] == V` ».

## 3. La demande

Ajouter des variantes **state-triggered** des captures existantes :

```
--screenshot-when   ADDR:VAL:FILE   # capture image quand RAM[ADDR] devient == VAL
--dump-ram-when     ADDR:VAL:FILE   # dump 64K   quand RAM[ADDR] devient == VAL
--screenshot-text-when ADDR:VAL:FILE
```

- `ADDR` en hexa (ex. `9C55`), `VAL` octet (ex. `07` ou `0x07`).
- **Front montant** : déclenche la 1re fois que la condition passe vraie
  (comme `screenshot_at_done` empêche le re-déclenchement).
- **Filet de sécurité** : `--cycles N` reste la borne max ; si la condition
  n'est jamais vraie avant N cycles, comportement au choix de l'équipe
  (idéal : exit non-zéro + message « condition jamais atteinte », pour que le
  test échoue franchement plutôt que silencieusement).
- Option utile plus tard : valeur 16 bits (`ADDR:VAL16`) et comparateurs
  (`>=`), mais le `== octet` couvre l'essentiel.

## 4. Pourquoi c'est le bon remède (et pas un contournement)

La capture devient déterministe sur l'**état du jeu** (« quand la room 7 est
chargée », `RAM[current_room] == 7`) et **indépendante du timing/layout**. Le
refactor SCUMM peut alors déplacer ce qu'il veut en mémoire sans casser un
seul test — plus besoin de trous BSS ni d'adresses devinées. C'est aligné avec
la règle SCUMM « mesurer, ne pas inventer » : on capture sur une **valeur
mesurée**, pas sur une adresse ou un cycle supposés.

## 5. Points d'ancrage dans le code (pour chiffrer)

Le mécanisme existant `screenshot-at` est un bon patron — il suffit de dupliquer
en remplaçant « cycle atteint » par « condition mémoire vraie » :

- Déclaration de l'option : `include/cli/cli_options.h:63`
  (`{"screenshot-at", required_argument, 0, OPT_SCREENSHOT_AT}`).
- Aide : `src/main.c:323`.
- Init des champs : `src/main.c:1463-1464`
  (`emu->screenshot_at_cycles = -1; emu->screenshot_at_file = NULL;`).
- Déclenchement par cycle : `src/main.c:2861-2866`
  (`if (!screenshot_at_done && emu->screenshot_at_cycles >= 0 && total_executed >= ...) { emu_export_image(...); }`).

La variante `-when` ajouterait des champs `screenshot_when_addr/val/file` et,
dans la même boucle, un test `bus_read(ADDR) == VAL` (via l'accès mémoire déjà
utilisé par `--dump-ram-at`) au lieu de la comparaison de cycles.

## 6. Impact attendu côté SCUMM

- Migration des ~13 tests layout-sensibles vers `-when` (déclencheur = valeur
  d'état stable), régénération unique et VÉRIFIÉE de leurs références.
- Suppression des « trous » BSS ; refactor d'état d'acteur débloqué.
- CI robuste aux changements de layout à l'avenir.

Merci — dispo pour préciser les cas de test ou fournir un binaire SCUMM de
répro (un simple `load_room #7` puis capture sur `current_room == 7`).
