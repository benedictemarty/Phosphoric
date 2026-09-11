# Phosphoric 2.0 : une machine cadencée au cycle — et l'annonce qu'on retire

*Note technique publique, 2026-09-11 (2.0.0-beta.1). Version française ; les
noms de tests et d'options sont ceux du dépôt.*

## Ce qu'on avait dit, et pourquoi c'était faux

Jusqu'à la 1.120.0-alpha, Phosphoric se présentait comme « cycle-accurate ».
Ce n'était pas vrai au sens que les émulateurs de référence donnent à ce mot.
Le cœur 6502 exécutait une instruction d'un bloc : les accès bus sortaient dans
le bon ordre et le total de cycles par opcode était juste, mais les cycles
internes étaient **rattrapés par bourrage en fin d'instruction**, les accès
factices du NMOS n'existaient pas, et les interruptions étaient prises aux
frontières d'instruction. Le VIA recevait des paquets de cycles, l'ULA rendait
une ligne entière d'un coup, le PSG tournait au taux d'échantillonnage audio.

C'est un niveau honorable — nous l'avons nommé **N2, « ordonné au cycle bus »** —
mais ce n'est pas « exact au cycle ». Nous avons retiré la formulation, écrit une
[échelle vérifiable](../ACCURACY.md) (N1 à N4, chaque niveau adossé au test qui
le prouve), et un garde-fou automatique (`make test-docs-claims`) qui refuse tout
« cycle-accurate » non qualifié dans les documents de vitrine. Puis nous avons
fait la V2.

## Ce que la V2 a changé

**Un oracle d'abord.** Avant de toucher au cœur, `make test-cycle` rejoue les
vecteurs SingleStepTests/65x02 — 10 000 cas par opcode, avec la trace bus
attendue cycle par cycle — et note quatre propriétés séparément. Le point de
départ, mesuré et publié : **44,26 %** de séquences bus exactes. L'oracle a
d'ailleurs révélé cinq défauts logiques du cœur, corrigés avant même de
commencer.

**Un cœur micro-séquencé.** Chaque instruction devient un plan de micro-opérations,
une par cycle, chacune faisant exactement son accès bus — y compris les accès
factices (index page zéro, traversée de page, écriture-retour des RMW, lectures
de pile mortes). Résultat : **100,00 %** sur 2 440 000 cas. Les interruptions
sont échantillonnées au **cycle pénultième**, ce qui donne gratuitement le
drapeau I retardé de `CLI`/`SEI`/`PLP` et le détournement d'un `BRK` par une NMI.
Les deux cœurs partagent le même calcul (drapeaux, BCD, opcodes illégaux) :
seul l'ordonnancement diffère. L'ancien reste disponible (`--cpu-legacy`).

**Une horloge maître.** `emu_cycle()` fait avancer **toute** la machine d'un
cycle, dans un ordre figé : φ1 l'ULA fetche sa cellule, φ2 le CPU fait son
accès bus, puis les périphériques avancent d'un cycle — jamais d'un paquet. La
boucle principale ne calcule plus rien : elle demande.

**Les composants, un par un.** Le VIA compte au cycle et son Timer 1 a retrouvé
sa période **N+2** (l'ancien modèle donnait N : 20 % d'erreur pour N=10). L'ULA
fetche **une cellule de 6 pixels par cycle**, à l'instant où le faisceau la lit :
une écriture en milieu de ligne n'atteint que les cellules pas encore balayées —
les splits raster deviennent possibles. Le PSG tourne à `horloge/8` (125 kHz) et
sa sortie est intégrée : un ton programmé au-dessus de Nyquist s'atténue au lieu
de replier, et l'**enveloppe, deux fois trop lente depuis toujours**, est à la
bonne vitesse. L'étage de sortie analogique a été relevé sur le schéma officiel.
Le WD1793 signale `LOST DATA` et la protection en écriture. La cassette au
signal a retrouvé sa **parité impaire**.

## Ce que ça change de visible

- **Les splits raster** : un programme qui change l'encre ou le mode en milieu de
  ligne obtient ce que le matériel donnerait, pas une ligne uniforme.
- **Le son** : enveloppes à la bonne durée, plus de sifflement fantôme sur les
  tons très aigus, signal centré (continu bloqué comme sur la carte).
- **Les IRQ** : une interruption ne peut plus être prise *avant* l'instruction en
  cours ; `SEI` ne protège pas l'instruction qui le suit d'une IRQ déjà pendante.
- **Les registres à lecture destructive** : un `POKE` BASIC vers le registre de
  données d'un ACIA **perd un octet**, parce que `STA (zp),Y` fait une lecture
  factice avant d'écrire — sur l'émulateur comme sur la machine. Phosphoric
  était plus permissif que le matériel ; il ne l'est plus.
- **Les savestates** : un état pris en pleine trame reprend exactement où il
  s'est arrêté (position du balayage, timers, échantillon d'interruption).

## Ce qu'on a appris en route

Deux défauts n'auraient jamais été vus sans changer de méthode.

L'enveloppe du PSG était déclarée conforme « par recalcul » — un recalcul qui
supposait 16 états au lieu de 32. Une hypothèse fausse est invisible à la
relecture ; elle ne se voit qu'en **mesurant le signal**. Les tests audio
mesurent désormais fréquences et durées au lieu de comparer des octets.

Le second est plus instructif encore. Un branchement non pris prenait sa
décision **un cycle trop tard**, dans une micro-op sans accès bus. Le compteur
du CPU restait juste, donc l'oracle était vert à 100 %. Mais l'horloge maître
avait été appelée une fois de plus : l'ULA avançait d'un cycle que ni le CPU ni
le VIA n'avaient vécu — environ 410 fois par trame sur la ROM BASIC, soit une
trame de dérive par seconde entre l'image et le reste de la machine. Un oracle
qui ne regarde que le CPU ne prouve pas la synchronisation de la machine. Ce
qui l'a révélé : un test de déterminisme des savestates, qui exigeait que les
arrêts raster tombent exactement au même cycle.

## Ce qu'on ne dit pas

« Phosphoric est exact au cycle » — non. Le FDC reste cadencé par des délais
forfaitaires sur une image plate (pas de flux MFM, donc pas de vraie perte
d'octet ni de CRC). Le cycle exact où l'ULA fetche la colonne 0 n'est pas
calibré contre du matériel réel : la *structure* est exacte, le calage
horizontal absolu est une convention (`--ula-fetch-offset`). Le demi-cycle du
one-shot du VIA n'est pas représenté. Le chargement de cassette par défaut
reste le patch ROM, par choix : même contenu chargé, 2,4× moins de cycles.

La formulation exacte autorisée, et le test qui ferait tomber chacune de ses
lignes, sont dans [docs/ACCURACY.md](../ACCURACY.md).

## Coût

Le passage au cycle a coûté, sur la machine de référence à pleine vitesse,
491 → 611 µs par trame émulée : **3 % du budget de 20 ms**. `make test-bench`
refuse désormais tout dépassement de 5 %.
