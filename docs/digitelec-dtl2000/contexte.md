# Modem Digitelec DTL 2000 — Référence technique

> Document de contexte pour Claude Code.
> Cible : émulation (Phosphoric), pilote/driver, et écosystème télématique Oric / PAVI.
> Statut des informations : **sourcé presse + manuels d'époque**. Les détails au bit
> près de la variante **Oric** sont à confirmer sur les scans (voir §10), car le PDF
> de programmation est un scan image non extractible.

---

## 1. TL;DR

- Modem télématique français de **Digitelec**, ~1985, ~1 490 FF (config de base).
- Gamme : **DTL 2000**, **DTL 2000+**, **DTL 2100**, **DTL 3000**.
- Architecture **modulaire en cartes** : fond de panier (alim + accès ligne) +
  carte de liaison ordinateur + carte modem.
- Sur **Oric** : interface **memory-mapped** de `#3F8` à `#3FD`, via nappe vers
  connecteur **34 points**. (Vu par la machine comme une plage d'octets, pas une
  liaison série classique.)
- Chips clés : **PIA Motorola EF6821**, **ACIA Motorola EF6850**, circuit modem
  **Thomson EFCIS EFB 7510** (carte V23).
- Modes : **V23** (1200/75 bauds, émulation Minitel/Télétel), **V21** (300 bauds
  full duplex, Transpac), **V23 half-duplex 1200** (Oric↔Oric), **Bell 103** (selon
  variante).
- Le `+` (DTL 2000+) ajoute le **V23 réponse full duplex** → permet un usage serveur.

---

## 2. Identité et variantes

| Modèle      | V21 300 | V23 1200/75 | V23 réponse | Bell 103 | Réponse auto | Interrupteur | Agréé PTT |
|-------------|:-------:|:-----------:|:-----------:|:--------:|:------------:|:------------:|:---------:|
| DTL 2000    | option  | oui         | non         | non      | non          | non          | oui       |
| DTL 2000+   | option  | oui         | **oui**     | non      | non          | non          | oui       |
| DTL 2100    | oui     | oui         | oui         | oui      | oui          | oui          | oui       |
| DTL 3000    | oui     | oui         | oui (univ.) | oui      | oui          | oui          | oui       |

> Tableau reconstitué à partir du comparatif *Microstrad* (voir Sources). À recouper
> avec les notices pour les valeurs exactes par révision.

Différence pratique majeure : seul le **2000+** (et au-dessus) sait faire **V23
réponse full duplex**, condition pour qu'un Oric se comporte en *serveur* Minitel
plutôt qu'en simple terminal.

---

## 3. Architecture matérielle

Trois fonctions, trois composants :

1. **Modulation/démodulation** → circuit **modem** (carte V23 autour du
   **Thomson EFCIS EFB 7510**).
2. **E/S parallèle + pilotage du modem** → **PIA EF6821** (Peripheral Interface
   Adapter, 2 ports A/B avec registres de direction DDR, de données OR, de contrôle CR).
3. **Conversion série/parallèle des données** → **ACIA EF6850** (Asynchronous
   Communications Interface Adapter, registre contrôle/état + registre data).

Chaîne logique :

```
CPU (Oric)  <->  PIA 6821  <->  [pilotage]  MODEM (EFB 7510)  <->  ligne téléphonique
            <->  ACIA 6850 <->  [données série]  ^
```

La programmation du modem passe **par le PIA** ; les données utiles transitent
**par l'ACIA**.

---

## 4. Cartographie mémoire

### 4.1 Oric (cible principale)

Interface **memory-mapped**, plage **`#3F8`–`#3FD`** — **confirmée** par le banc
d'essai *L'Ordinateur Individuel n°69* (« l'Oric voit le modem comme une série
d'octets de #3F8 à #3FD »). Les 6 octets correspondent exactement à un **PIA 6821
(4 registres) + un ACIA 6850 (2 registres)**, et le découpage se calque
offset-pour-offset sur la version CPC (§4.2) :

| Adresse Oric | Composant | Registre |
|--------------|-----------|----------|
| `#3F8`       | PIA 6821  | Port A — données / DDRA (selon CRA bit 2) |
| `#3F9`       | PIA 6821  | Contrôle A (CRA) |
| `#3FA`       | PIA 6821  | Port B — données / DDRB |
| `#3FB`       | PIA 6821  | Contrôle B (CRB) |
| `#3FC`       | ACIA 6850 | Contrôle (écriture) / État (lecture) |
| `#3FD`       | ACIA 6850 | Transmission / Réception (data) |

**Niveau de confiance :**
- Plage `#3F8`–`#3FD` → **confirmée** (source primaire d'époque).
- Découpage des 6 registres → **quasi-certain** : cohérent avec le nombre de
  registres des deux chips, avec l'ordre PRA/CRA/PRB/CRB observé sur les cartes
  Oric à 6821 (cf. analyse forum CEO d'une interface 6821 : `#380`=A/DDRA,
  `#381`=contrôle A, `#382`=B/DDRB, `#383`=contrôle B), et identique à la version CPC.
- **Signification bit à bit** des registres OR (PIA) et de la config ACIA pour la
  variante *Oric* → **non sourcée textuellement** (présente seulement dans la notice
  scannée « Pour en savoir plus » / le PDF « Programmation carte DTL V23 », images
  non océrisées). → reste **à extraire (§10)**.

> ⚠️ **Conflit d'adresses page 3.** `#3F8`–`#3FD` est dans la moitié haute de la
> page 3, partagée avec l'électronique disque : un **Jasmin** occupe précisément
> `03F8`–`03FF` (sélection face, reset FDC, overlay RAM, ROMDIS, sélection lecteur).
> Le décodage des cartes page-3 étant souvent grossier (répétition d'adresses), le
> Digitelec DTL 2000 et un lecteur de disquettes **ne cohabitent pas proprement**
> sans précaution. À garder en tête côté émulation et côté matériel réel.

### 4.2 Amstrad CPC (référence croisée, bien documentée)

Source fiable et détaillée (classeur Weka). **Mêmes chips**, adresses différentes —
sert de modèle pour déduire/valider le comportement Oric.

| Adresse CPC | Composant | Rôle |
|-------------|-----------|------|
| `&F8F8`     | PIA 6821  | Registre double DDR/OR (port A) |
| `&F8F9`     | PIA 6821  | Registre de contrôle CR (seul **CR2** utilisé) |
| `&F8FC`     | ACIA 6850 | Contrôle (écriture) / État (lecture) |
| `&F8FD`     | ACIA 6850 | Transmission / Réception (data) |

---

## 5. Programmation — PIA EF6821

- Registre de **contrôle** : seul **CR2** sert. `CR2=0` → adresse le **DDR**
  (direction). `CR2=1` → adresse le **OR** (données).
- **DDR** (direction) : 1 bit par ligne, 1 = sortie, 0 = entrée. Config DTL 2000+ :
  **toutes les lignes en sortie sauf la ligne 0 en entrée** → `DDR = &FE`.
- **OR** (données) : pilote les états du modem (sélection mode, connexion ligne…).

Valeurs CPC connues (à transposer aux adresses Oric) :

```basic
OUT &F8F9,&00   : REM CR2=0  -> sélection DDR
OUT &F8F8,&FE   : REM lignes 1..7 en sortie, ligne 0 en entrée
OUT &F8F9,&04   : REM CR2=1  -> sélection OR
OUT &F8F8,&00   : REM init modem
```

Note comportementale : à l'**ordre de connexion**, le modem attend ~**45 s** puis se
déconnecte automatiquement si la porteuse distante est perdue. Pour relancer une
connexion : mettre **OR2 à 1** puis de nouveau **à 0**.

---

## 6. Programmation — ACIA EF6850

- Registre **double** contrôle/état :
  - **écriture** = registre de **contrôle** (format, horloge, mode émission).
  - **lecture** = registre d'**état** (porteuse présente, donnée reçue, buffer
    d'émission vide…).
- **Init obligatoire** avant toute config : écrire `&03` (master reset).
- Bits d'état utiles : **bit 0 = donnée reçue (RDRF)**, **bit 1 = registre
  d'émission vide (TDRE)**.
- Format Minitel/Vidéotex : **7 bits, parité paire, 1 stop**.

Exemple de boucle d'échange (logique, CPC) :

```basic
X = INP(&F8FC)                 : REM lire le registre d'état ACIA
IF (X AND 1) = 1 THEN GOSUB lire_caractere   : REM bit0 : un octet est arrivé
' ... pour émettre, attendre TDRE :
WAIT: X = INP(&F8FC) : IF (X AND 2) <> 2 THEN GOTO WAIT
OUT &F8FD, Z                   : REM Z = code ASCII à émettre
```

Lecture d'un octet reçu : `Y = INP(&F8FD)`.

---

## 7. Séquences type

### 7.1 Initialisation générique (CPC, à transposer)

```basic
10 OUT &F8F9,&00 : REM SELECTION DDR (PIA)
20 OUT &F8F8,&FE : REM OR1..OR7 en sortie, OR0 en entrée
30 OUT &F8F9,&04 : REM SELECTION OR
40 OUT &F8F8,&00 : REM INIT MODEM
50 OUT &F8FC,&03 : REM INIT (reset) ACIA
60 OUT &F8FC,&40 : REM pas d'emission
```

> Astuce d'époque : intercaler de courtes boucles d'attente (`FOR I=1 TO 1:NEXT`)
> entre instructions, le modem ayant besoin de « digérer » les écritures.

### 7.2 Émulation Minitel (V23 appel full duplex 75/1200)

```basic
70  OUT &F8F8,&BC : REM config modem  (OR : 10111100 ; OR2=1 -> pas encore connecté)
80  OUT &F8FC,&49 : REM config ACIA   (V23, 7 bits parité paire 1 stop, pas d'emission)
90  REM *** attente porteuse ***
100 OUT &F8F8,&B8 : REM connexion ligne (OR2 -> 0 : 10111000)
110 OUT &F8FC,&09 : REM ordre d'envoyer la porteuse (CR6 -> 0 : 00001001)
```

Côté terminal Minitel, correspondances clavier d'origine (logiciel cassette) :
`RETURN`→ENVOI, flèche gauche→RETOUR, flèche droite→SUITE, `CTRL-D` ×2→CONNEXION/FIN.

---

## 8. Modes de fonctionnement

| Mode                   | Émission | Réception | Usage typique |
|------------------------|----------|-----------|---------------|
| V23 appel full duplex  | 75 bauds | 1200 bauds| Terminal Minitel / Télétel (annuaire électronique, serveurs) |
| V23 réponse full duplex| 1200     | 75        | **Serveur** Minitel (DTL 2000+ requis) |
| V23 half duplex        | 1200     | 1200      | Liaison Oric↔Oric (un émet, l'autre reçoit, alternance) |
| V21 full duplex        | 300      | 300       | Transpac, modem↔modem généraliste |
| Bell 103               | 300      | 300       | Compatibilité US (variantes 2100/3000) |

Limite serveur (sans `+`) : la carte V23 simple ne gère pas le *answer mode* en
asymétrique → pas de dialogue interactif bidirectionnel. Le 2000+ lève cette limite.

---

## 9. Logiciels et disquettes d'origine

- **Cassette livrée** : face 1 = émulateur terminal Minitel (texte seul, **pas** de
  semi-graphique Vidéotex ni couleur dans le logiciel d'origine) avec **composeur de
  numéro** intégré (le `-` insère une tempo de 5 s) ; face 2 = comms **Oric↔Oric**
  (1200 bauds, half-duplex, via POKE/DOKE selon qu'on transfère du BASIC ou une
  zone mémoire).
- **Images disque** archivées (.dsk, 140 Ko) : `Digitelec DTL 2000 Disk.dsk`,
  `Digitelec DTL 2000 Plus Disk.dsk` (voir Sources).

---

## 10. Points à confirmer sur les scans

Plage et découpage des registres : **résolus** (cf. §4.1). Restent à vérifier dans
**« Programmation carte DTL V23.pdf »** et la notice **« Pour en savoir plus »**
(8 pages) — non extractibles automatiquement (scans image) :

1. Signification **bit à bit** du registre OR du PIA pour la version Oric
   (sélection de mode, OR2 = connexion ligne, etc.).
2. Valeurs d'init ACIA pour l'Oric (l'horloge/diviseur peut différer du CPC).
3. Procédure de **détection de sonnerie** (entrée ligne 0 du PIA ?) pour usage serveur.
4. Brochage précis du **connecteur 34 points** Oric ↔ carte de liaison.

> Action suggérée : OCR des deux PDF (Tesseract `fra`) pour lever ces points et
> figer un tableau de registres définitif.

---

## 11. Notes d'implémentation

### Pour l'émulation (Phosphoric)
- Modéliser une **périphérique memory-mapped** sur `#3F8`–`#3FD` exposant deux
  composants : un **PIA 6821** et un **ACIA 6850** (réutiliser, si dispo, des modèles
  6821/6850 génériques).
- L'**ACIA** porte la sémantique série : flags RDRF (bit 0) / TDRE (bit 1) côté état,
  diviseur d'horloge → débit. Émuler V23 = débits asymétriques 75/1200 (le timing
  « réel » n'a d'intérêt que pour la fidélité ; un modèle octet-par-octet suffit
  fonctionnellement).
- Le **PIA** porte le contrôle modem : connexion ligne (OR2), sélection de mode,
  détection sonnerie. Brancher ces bits sur un « backend » virtuel
  (boucle locale, pont TCP, ou passerelle Vidéotex).
- **Décodage page 3** : reproduire (ou au moins documenter) le conflit `#3F8`–`#3FF`
  avec l'électronique disque (Jasmin) — utile pour diagnostiquer les cas où modem et
  lecteur sont déclarés en même temps. Voir l'encadré §4.1.

### Pour PAVI / Minitel
- Le DTL 2000 en **V23 réponse** côté serveur correspond exactement au rôle d'un
  serveur Télétel : 75 bauds montant (client), 1200 descendant (serveur) — utile si
  un jour PAVI veut parler à du vrai matériel Oric via passerelle modem.
- Attention : le logiciel d'origine ne décode **pas** le semi-graphique Vidéotex.
  Un terminal moderne (ou PAVI côté serveur) doit gérer le jeu G0/G1, l'attribut
  série, etc., indépendamment du modem.

---

## 12. Sources

**Manuels / disquettes scannés (Apple II Documentation Project)**
- Programmation carte DTL V23 (PDF, scan) —
  `https://mirrors.apple2.org.za/Apple%20II%20Documentation%20Project/Peripherals/Modems/Digitelec%20DTL%202000/Manuals/Programmation%20carte%20DTL%20V23.pdf`
- Manuel de l'utilisateur (PDF) —
  `https://mirrors.apple2.org.za/Apple%20II%20Documentation%20Project/Peripherals/Modems/Digitelec%20DTL%202000/Manuals/Digitelec%20DTL%202000%20Manuel%20de%20l%27utilisateurpdf.pdf`
- Notice d'utilisation (PDF) —
  `https://mirrors.apple2.org.za/Apple%20II%20Documentation%20Project/Peripherals/Modems/Digitelec%20DTL%202000/Manuals/Digitelec%20DTL%202000%20Notice%20d%27utilisationpdf.pdf`
- Images disque (.dsk) —
  `https://mirrors.apple2.org.za/Apple%20II%20Documentation%20Project/Peripherals/Modems/Digitelec%20DTL%202000/Disk%20Images/`

**Programmation (registres, chips)**
- Classeurs Weka, « 8/5.3.1 Le modem Digitelec DTL 2000 » —
  `https://cpcrulez.fr/codingBOOK_weka_08531.htm`

**Bancs d'essai / presse**
- L'Ordinateur Individuel n°69 (+ CPC Revue, Amstrad Mag n°5, Hebdogiciel) —
  `https://cpcrulez.fr/hardware-modem-modem_digitelec_DTL_2000.htm`
- Théoric, « L'Atmos à cœur ouvert » —
  `https://docplayer.fr/111169416-L-atmos-a-coeur-ouvert.html`
- Microstrad n°5 (texte intégral, comparatif gamme) —
  `https://archive.org/stream/microstrad05/Microstrad05_djvu.txt`
- Microstrad, « Les modems Digitelec DTL » —
  `https://cpcrulez.fr/hardware-modem-modem_digitelec_DTL_MS.htm`

**Référence**
- CPCWiki, Digitelec DTL 2000/2100 Modem —
  `https://www.cpcwiki.eu/index.php/Digitelec_DTL_2000/2100_Modem`

**Contexte Oric**
- CEO Oric, « Logiciels de communication Oric » —
  `https://ceo.oric.org/community/applications/logiciels-de-communication-oric/`
- CEO Oric, « Interface Oric inconnue avec un 6821 » (corrobore l'ordre des registres
  PRA/CRA/PRB/CRB et le décodage grossier en page 3) —
  `https://ceo.oric.org/community/peripheriques/interface-oric-inconnue-avec-un-6821/`

---

*Avertissement : adresses Oric `#3F8`–`#3FD` **confirmées** et découpage des 6 registres
**quasi-certain** (cf. §4.1). Les **valeurs de bits** des séquences §5–§7 sont documentées
pour la version Amstrad CPC (chips identiques) et restent à valider au bit près sur l'Oric
via §10.*
