# Ajouter une carte d'extension

Une carte d'extension (synthèse vocale, MIDI, modem, contrôleur…) s'ajoute en
**un fichier et une ligne** :

1. écrire `src/cards/card_<id>.c` ;
2. ajouter `X(<id>, <tick>)` à `CARD_MODULE_LIST` dans `include/cards_list.h`.

Rien d'autre : le Makefile compile `src/cards/card_*.c` par motif, et le cœur dérive
de la liste les options de lancement, l'aide, le menu F1, `phosphoric.cfg`, la table
du bus d'E/S, les ticks, les sauvegardes d'état et le mixage audio. La carte
d'exemple [`docs/examples/card_demo.c`](examples/card_demo.c) est le modèle à copier ;
`make test-card-template` l'ajoute à une copie de l'arbre et vérifie qu'elle
fonctionne de bout en bout.

Décision : [ADR 0006](adr/0006-cartes-en-modules.md). Plan et mesures :
[`docs/specs/CARD_MODULES.md`](specs/CARD_MODULES.md). Contrat :
[`include/card_module.h`](../include/card_module.h).

## Le descripteur

Le fichier définit un `const card_module_t card_<id>` :

| Champ | Rôle |
|---|---|
| `descs`, `ndescs` | Fiche(s) du menu F1 (`card_desc_t`, `include/cards.h`) : id, nom, rôle, groupe exclusif, option d'activation, paramètres expliqués. Une puce peut porter plusieurs cartes (Mageco et ORICON). |
| `opts`, `nopts` | Options de lancement : nom sans `--`, `no_argument` / `required_argument` / `optional_argument`, fonction qui range la valeur dans la configuration de la carte. |
| `helps`, `nhelps` | Blocs d'aide (`card_help_t`), texte exact affiché par `--help`. |
| `cfg_size`, `cfg_defaults` | Configuration propre à la carte, allouée et mise aux valeurs par défaut par le cœur. |
| `init` | Appelé au démarrage, carte présente ou non (état de repos, interruptions câblées). |
| `stage`, `setup` | Mise en route, à une étape donnée (`card_stage_t`) : lit la configuration de la carte et celle du cœur, vérifie les conflits, allume la carte (`emu->card_on[CARD_IDX_<id>] = true`). Renvoie 0, ou 1 après avoir journalisé l'erreur. |
| `teardown` | Fermeture (transports, fichiers). |
| `bus` | Contrat d'E/S existant (`io_device_t`, `include/io/io_device.h`) : `claims`, `read`, `write`, `peek` (lecture sans effet de bord), `save`/`load` d'une section `.ost`, `tick`. |

## Présence, état, tick

- **Présence** : `emu->card_on[CARD_IDX_<id>]`, posé par `setup`. `claims`, `save` et
  le tick le testent.
- **État** : privé au module, dans une variable `static` (il n'y a qu'une machine par
  processus). `emulator_t` n'a pas à le connaître.
- **Tick** : avec `X(<id>, 1)`, le cœur appelle `card_<id>_tick(emu, cycles)` à chaque
  cycle CPU si la carte est présente. L'appel est **direct**, généré depuis la liste
  (`include/card_ticks.h`) : une table de pointeurs coûtait 5 % d'instructions.
  L'ordre de la liste est l'ordre des ticks. Sans tick : `X(<id>, 0)`.

## Son

Une carte qui produit du son enregistre, dans `setup`, une source mono :
`audio_add_source(fn, ctx)` (`include/audio/audio.h`). Elle est mixée au PSG par le
callback SDL comme par la capture (`--audio-wav`, AVI, cast). Renvoyer `false` signifie
« rien produit » (pas de mixage).

## Sauvegardes d'état

`bus->save_tag` (4 octets, ex. `"DMO\0"`) nomme la section ; `save` renvoie `false`
quand la carte est absente, pour que le `.ost` reste inchangé sans elle. Préférer une
sérialisation champ par champ à l'écriture brute d'une structure qui contient des
pointeurs (adresses différentes à chaque lancement : `.ost` non reproductible).

## Menu F1 et `phosphoric.cfg`

La fiche suffit : le menu liste la carte, ses paramètres et leur explication ; la
configuration la mémorise sous `carte.<id>=oui` et `<id>.<clé>=valeur` (la clé est
celle du paramètre dans la fiche). Changer de cartes relance l'émulateur (ADR 0004).

Types de paramètres (`card_param_kind_t`) : fichier, dossier, texte, adresse
hexadécimale, oui/non, et **choix** (`CARD_P_CHOICE`, 2.21.0) : `def` donne les
valeurs possibles séparées par `|`, la première étant la valeur par défaut
(`"aucun|simulé|réel"`) ; Entrée passe à la suivante. Un paramètre sans option
(`cli` à `NULL`) est traduit à part dans `cards_build_argv` : c'est le cas du
modem de la carte LOCI, qui devient `--serial picowifi` ou
`--serial com:115200,8,N,1,<port>`.

## Ancres : garder une place

Sans ancre (champs `*_before` à `NULL`), une carte va **en fin** de table d'options,
d'aide, de menu et de bus. Les cartes historiques gardent leur place exacte grâce à
des ancres : `opts_before` (option qu'elles précèdent dans la table getopt), le
`before` de chaque bloc d'aide, `desc_before` (fiche du menu), `bus_before`
(périphérique du bus). Une ancre peut nommer une autre carte. L'ordre du bus est
aussi celui de la priorité des accès et de l'ordre des sections `.ost`.

## Vérifier

- `make test-card-template` (le modèle) et `make tests-strict`.
- Comportement inchangé pour qui n'utilise pas la carte : `tools/cli_golden.sh` entre
  le binaire d'avant et celui d'après (aide, messages, fichiers produits).
- Performance : `tools/instr_count.sh BINAIRE [options]` compte les instructions
  exécutées (stable, contrairement au temps sur une machine bridée).
- Ajouter à `tests/cli_golden/cases.txt` quelques lignes qui exercent la carte.

## Cartes actuelles

`microdisc`, `jasmin`, `loci`, `acia`, `dtl2000`, `mageco` (+ ORICON), `sp0256`,
`mea8000`, `ula_ng`. La carte `loci` a trois modes (2.26.0 : les fiches
`loci_emu` et `loci_hw` sont devenues ses modes `firmware` et `réelle`) ; un
paramètre propre à un mode n'est montré, relancé et enregistré que dans ce mode
(`k_loci_mode_params`, `cards_param_applies`). Pour certaines, une partie reste au
cœur parce que d'autres parties de l'émulateur la lisent : options, état ou tick de
Microdisc, Jasmin et LOCI ; état de l'ACIA et de l'ULA-NG (détail dans
`docs/specs/CARD_MODULES.md`).
