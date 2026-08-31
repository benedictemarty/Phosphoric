# Un mot à l'équipe Phosphoric — le modèle ACIA `$0380` du LOCI a tenu

**Date** : 2026-08-31
**De** : bmarty <bmarty@mailo.com>
**Objet** : votre modèle de course PHI2 (`--loci-serve-timing`) reproduit *exactement* le
diagnostic amont du firmware LOCI — merci.

---

## En deux mots

L'auteur du firmware LOCI (**Sodiumlightbaby**) a répondu sur le forum defence-force
([`t=2926`](https://forum.defence-force.com/viewtopic.php?t=2926)) au sujet du bug de lecture
ACIA `$0380` avec un modem PicoWiFi. Sa réponse **valide, mot pour mot, le modèle que vous avez
implémenté dans Phosphoric**. Je voulais vous le signaler, parce que ce n'est pas si fréquent qu'un
modèle d'émulateur soit confirmé par la source matérielle *après coup*.

## Ce que dit l'amont, et ce que votre modèle prédisait déjà

| Diagnostic firmware (Sodium, forum t=2926) | Modèle Phosphoric correspondant |
|---|---|
| *« the actual reading is fully done by PIO … there is no read serve in `act_loop` »* — le CPU ne sert pas la lecture, la donnée doit être **présente en mémoire avant le cycle**. | `--loci-serve-timing SERVE[,LATCH]` : *read clean iff `tior+SERVE ≤ LATCH`*. C'est **précisément** « la donnée est-elle stagée à temps ». |
| Le build `-Os` marche, `-O2` rate — question de **placement/latence du code**, pas de logique. | `test_phase_reproduces_build_os_vs_o2` : `serve=26 ≤ 27` → OK, `serve=36 > 27` → KO. Bluffant de fidélité. |
| Lecture DATA **destructive** → un raté = octet perdu = « modem injoignable ». | `test_data_miss_is_open_bus_and_destructive` : raté = **open-bus** (dernier octet du bus, ni VIA ni `0xFF`) + RX consommé en aveugle. |
| Écriture toujours fiable. | `test_write_always_reaches_acia`. |

Votre choix de rendre le raté **open-bus déterministe** (et non un `0xFF` ou une valeur VIA
arbitraire) colle exactement à ce qu'on observe : les fameux `$36`/`$76` du rapport sont bien du
résidu de bus, pas une lecture VIA.

## Ce que ça nous a permis de faire

Grâce au modèle (`--loci-serve-timing` + `--loci-serve-jitter` + les registres du mode fiable
`$0384`/`$0385`), on a pu **concevoir et prouver de bout en bout**, sans matériel Oric réel, un
mode ACIA « fiable » opt-in (opcode `$AA` : RX non destructif + seqlock + ACK par écriture) :

- `test_loci_acia_miss` : **20/20** ;
- e2e comparatif « HELLO » sous ratés injectés (1 tour/2 en course perdue) :
  - `test_standard_corrupts_hello_under_miss` : le 6551 **standard corrompt** — un raté sur le
    `'E'` le **perd** (RX destructif) et l'**open-bus** le remplace → `H · <octet parasite> · LLO`
    (≠ `HELLO`) ;
  - `test_reliable_hello_intact_under_misses` : le mode **fiable `$AA` reste intact** — seqlock +
    ACK rejouent chaque octet raté → `HELLO` reçu **sans perte**.

Autrement dit : votre banc a servi de **substitut fidèle au silicium** pour valider un choix de
design firmware. C'est exactement l'usage qu'on espérait d'un émulateur cycle-accurate, et il a
tenu la promesse.

## Une seule réserve, honnête

Tout ceci reste **validé Phosphoric**, pas encore sur **Oric physique** (je dispose d'une carte
LOCI mais pas d'un Oric). Donc « modèle fidèle au diagnostic amont » ≠ « validé silicium ». Rien à
corriger de votre côté : c'est juste la limite que je tiens à ne pas masquer.

## Petit point ouvert (sans urgence)

La `LATCH` par défaut (27 sous-ticks PHI2/30) et la fenêtre `--loci-mia-window` sont des
paramètres de **modèle**, pas des mesures matérielles. Le jour où quelqu'un capture le `tior` réel
sur une vraie carte + modem, il serait précieux de recaler ces constantes. En l'état, elles
reproduisent parfaitement le comportement *qualitatif* (`-Os` OK / `-O2` KO), ce qui suffit pour le
design.

Merci pour ce banc — franchement, il a fait le travail.

— bmarty
