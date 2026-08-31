# Un mot à l'équipe Phosphoric — ACIA `$0380` : le symptôme tient, la cause racine est révisée

**Date** : 2026-09-01
**De** : bmarty <bmarty@mailo.com>
**Objet** : votre modèle reproduit fidèlement le **symptôme** du bug ACIA `$0380` ; l'auteur du
firmware (forum t=2926) confirme le **mécanisme PIO/staging** mais situe la **cause racine** dans la
signalisation d'adresse, **pas** dans le timing. Mise au point honnête.

---

## En deux mots

L'auteur du firmware LOCI (**Sodiumlightbaby**) a répondu sur le forum defence-force
([`t=2926`](https://forum.defence-force.com/viewtopic.php?t=2926)) au sujet du bug de lecture ACIA
`$0380` avec un modem PicoWiFi. Ses explications (citations vérifiées ci-dessous) **confirment le
mécanisme et le comportement** que Phosphoric modélise — mais **corrigent la cause racine** que le
rapport initial (et une version antérieure de cette note) attribuait au timing.

## Citations vérifiées (t=2926)

> « the actual reading is **fully done by PIO** and the same for all accesses. **There is no read
> serve in `act_loop`.** »
>
> « **Data must be in the right place in LOCI memory before the read cycle begins**, and data ready
> is not signalled before this has happened. »
>
> « `acia_read()` is only handling the **side-effects** — the read happened, so clear the status. »
>
> Cause réelle selon l'auteur : **« missing signalling to the PIO that it should answer for this
> address »** (bug de *mapping / address-response*), corrigé dans la **branche issue-17** — **pas**
> un effet de timing. Il **n'endosse pas** l'explication `-Os`/`-O2`.

## Ce que le modèle Phosphoric reproduit (symptôme) — confirmé

| Point amont (vérifié) | Modèle Phosphoric |
|---|---|
| Lecture entièrement par PIO ; la donnée doit être **stagée avant le cycle**. | `--loci-serve-timing SERVE[,LATCH]` : *read clean iff `tior+SERVE ≤ LATCH`* = « la donnée est-elle en place à temps ». |
| Lecture DATA **destructive** ; `acia_read()` ne fait que les effets de bord. | `test_data_miss_is_open_bus_and_destructive` : raté = **open-bus** (résidu de bus, ni VIA ni `0xFF` — les `$36`/`$76` du rapport) + RX consommé. STAT/CMD/CTRL idempotents (`test_status_miss_is_idempotent`). |
| Écriture fiable. | `test_write_always_reaches_acia`. |

## Cause racine — la correction honnête

Le rapport initial concluait « `-Os` corrige / `-O2` rate ⇒ effet **timing/staging** ». **L'auteur
ne confirme pas cette lecture** : pour lui la donnée n'arrive pas parce que **le PIO n'était pas
signalé pour répondre à `$0380`** (bug de *mapping d'adresse*, corrigé branche issue-17), et non à
cause d'une marge de timing. Le lien `-Os`/`-O2` observé par le rapporteur est donc, au mieux, un
**effet de bord incident** du changement de build, pas la cause.

**Conséquence pour Phosphoric (à ne pas masquer) :** notre modèle `SERVE/LATCH` reproduit
fidèlement le **symptôme** (donnée absente à temps → open-bus → octet perdu), mais il le modélise
comme une **course de timing** alors que la cause amont est un **défaut de signalisation/mapping**.
C'est un **modèle comportemental du symptôme, pas un modèle de la cause**. À ne pas présenter comme
« recoupe exactement le diagnostic amont ».

## Ce que ça nous a quand même permis de faire

Le modèle comportemental (staging manqué → open-bus + RX destructif) a suffi à **concevoir et
prouver de bout en bout**, sans Oric réel, un mode ACIA « fiable » opt-in (opcode `$AA` : RX non
destructif + seqlock + ACK par écriture, registres `$0384`/`$0385`) :

- `test_loci_acia_miss` : **20/20** ;
- e2e comparatif « HELLO » sous ratés injectés (1 tour/2) : 6551 **standard corrompt** le `'E'`
  (RX destructif → octet parasite du bus), mode **fiable `$AA` intact** (`HELLO`, zéro perte).

Quelle que soit la cause racine amont, un pilote qui **relit + ACK** (seqlock) survit à une donnée
absente à un cycle — c'est ça qu'on voulait valider, et ça tient.

## Réserves (assumées)

1. Validé **Phosphoric**, pas sur **Oric physique** (carte LOCI, pas d'Oric). « Fidèle au symptôme »
   ≠ « validé silicium ».
2. Notre `SERVE/LATCH` + `--loci-mia-window` sont des paramètres de **modèle comportemental**, pas
   des mesures ; et la **cause amont est un mapping**, pas un timing (cf. ci-dessus).

Merci pour ce banc — il a fait le travail sur le symptôme et le design ; la cause racine, elle,
appartient au firmware (issue-17).

— bmarty
