# Étude de faisabilité — Interface MIDI Mageco pour Oric

- **Statut** : ✅ **IMPLÉMENTÉ — niveaux A + B** livrés au Sprint 61 (v1.21.25 → v1.21.26-alpha)
- **Auteur** : bmarty
- **Date** : 2026-06-23
- **Source** : fil forum Defence-Force [« Mageco MIDI interface » (t=2525)](https://forum.defence-force.org/viewtopic.php?t=2525)
- **Décision** : GO — sprint réalisé (`src/io/mageco.c`, `--mageco`, `make test-midi`)

> **Réalisation niveau A** (v1.21.25) : carte MC6850 à $03FE + backend MIDI fichier
> + tests. Code : `src/io/mageco.c`, `include/io/mageco.h`. CLI : `--mageco
> file:in[:out]|loopback|tcp|pty` (+ `--mageco-addr`). 13 tests + e2e (`90 3C 7F`).
>
> **Réalisation niveau B** (v1.21.26) : backend **MIDI temps réel** multi-plateforme
> (`SERIAL_BACKEND_MIDI`, `src/io/serial_backend.c`). CLI : `--mageco midi[:TARGET]`,
> build optionnel `MIDI=1`. Branche **ALSA** (Linux, lie `-lasound`) : port nommé
> « Phosphoric MIDI » connectable via `aconnect` à FluidSynth/DAW, validé temps réel
> (`aseqdump` reçoit le Note On du BASIC). Branches **CoreMIDI (macOS)** / **WinMM
> (Windows)** écrites selon les API mais non vérifiées sur cet hôte Linux.
>
> **Réalisation extensions** (v1.21.27) : lecteur **Standard MIDI File** (`src/io/smf.c`,
> format 0/1, carte de tempo) + transport **`smf:FILE[:loop]`** rejouant un `.mid`
> dans l'Oric en MIDI IN cadencé (`CLOCK_MONOTONIC`). 6 tests (`make test-smf`),
> validé temps réel.

---

## 1. Contexte

L'interface **Mageco MIDI** est une extension matérielle Oric des années 1980, dont
Dbug a relancé la reconstruction (PCB modernes, projet modulaire « Oric-Con » avec
shield MIDI). Côté logiciel, un lecteur de fichiers `.mid` standard existe (Fabrice).

Caractéristiques matérielles relevées dans le fil :

| Élément | Détail |
|---|---|
| Cœur série | **Motorola MC6850 ACIA** |
| Adresses I/O | **#3FE / #3FF** (RS=0 control/status, RS=1 data) |
| Décodage | portes 7411 (AND), 7404 (NOT) |
| Isolation entrée | optocoupleur **TIL 111** (MIDI IN uniquement) |
| Connecteurs | 2× DIN-5 (MIDI IN / OUT) |
| Horloge | cristaux dédiés (`xtal1`/`xtal2`) → débit MIDI **31250 bauds**, 8N1 |

Le fil ne documente pas les bits de contrôle exacts ni la gestion d'IRQ : c'est du
6850 standard, ce que nous savons déjà modéliser.

---

## 2. Ce que Phosphoric possède déjà (forte réutilisation)

L'étude du code montre que **l'essentiel du cœur existe**, ce qui change radicalement
le coût estimé :

1. **Modèle MC6850 autonome et réutilisable** — `src/io/acia6850.c` /
   `include/io/acia6850.h`. Logique UART pure (registres control/status/data, RDRF,
   TDRE, IRQ via callback `irq_out`, FE/OVRN/PE, DCD/CTS). C'est *exactement* le chip
   de la carte Mageco. Déjà éprouvé par le Digitelec DTL 2000.
   - Note : ce modèle laisse volontairement le débit et le transport d'octets à l'hôte
     (« the clock is external, divided /1, /16 or /64 »). Le MIDI 31250 bauds se règle
     donc côté cadence hôte, pas dans le chip — aucune modification du cœur 6850 requise.

2. **Routage I/O configurable** — `main.c` route déjà un ACIA sur une base d'adresse
   paramétrable (`--acia-addr`, `emu->acia_base_addr`, défaut $031C / $0380 LOCI). Le
   cas #3FE/#3FF est juste une nouvelle base dans la zone $0300-$03FF.

3. **Abstraction de backend série** — `serial_backend.h` (enum `SERIAL_BACKEND_*`,
   struct `serial_backend_s`) avec déjà `FILE` (replay/capture), `PTY`, `COM`, `TCP`…
   Le « transport transparent » de `main.c` est mutualisé entre `--serial` et
   `--dtl2000` : on peut y greffer un backend MIDI sans dupliquer la plomberie.

**Conclusion partielle** : on n'a *pas* à écrire de nouveau cœur ACIA. Le travail réel
se concentre sur **(a) le câblage à #3FE/#3FF** et **(b) un backend MIDI**.

---

## 3. Travail restant (les vrais coûts)

### 3.1 Câblage de la carte à #3FE/#3FF
- Nouveau « device » `--mageco` (ou `--midi`) instanciant un `acia6850_t` à la base
  $03FE.
- Étendre le routage I/O de `main.c` (read/write callbacks) pour cette base, en gérant
  le **conflit d'adresses** : #3FE/#3FF est dans le miroir VIA $0300-$03FF actuel
  (`memory.c:94`). Il faut donner la priorité à la carte quand elle est active, comme
  c'est déjà fait pour l'ACIA 6551 vs Microdisc.
- **Risque connu et documenté dans le fil** : « #3FE and #3FF can rise
  incompatibilities with other extensions ». À refléter dans `COMPATIBILITY.md`.

### 3.2 Backend MIDI (le vrai morceau)
Trois niveaux d'ambition, à choisir :

| Niveau | Description | Effort |
|---|---|---|
| **A. Capture/replay fichier** | TX → octets MIDI bruts dans un `.syx`/`.mid`, RX ← fichier. Réutilise `SERIAL_BACKEND_FILE`. Pas de son temps réel. | Faible |
| **B. Port MIDI hôte (ALSA seq / CoreMIDI)** | TX/RX branchés sur un vrai port MIDI système → pilote un synthé/DAW réel. | Moyen (dépendance optionnelle, comme SDL2) |
| **C. Synthé interne** | Rendu audio du flux MIDI dans l'émulateur (General MIDI). | Élevé, hors périmètre raisonnable |

Recommandation : viser **A** d'abord (livrable testable sans dépendance), garder **B**
en option de build (`MIDI=1`, façon `CAST=1`).

### 3.3 Timing
MIDI = 31250 bauds, 8N1, ~320 µs/octet (≈ 320 cycles CPU à 1 MHz). Le pas de timing
existant (agrégation par instruction, `main.c:1679`) s'applique tel quel ; il suffit
de fixer la cadence d'octet à 31250 bauds pour cette base.

### 3.4 Tests & doc (obligatoires, méthode agile)
- Nouvelle suite `make test-midi` (modèle : `test-serial`) : reset 6850, écriture
  control, TX d'un octet → backend, RX → RDRF+IRQ, master reset, conflit d'adresses.
- Mise à jour `CHANGELOG`, `VERSION_TRACKING`, `CIRRUS_OS`, `ROADMAP`, `EMU_VERSION`,
  `COMPATIBILITY.md`, README (section `--mageco`/`--midi`).

---

## 4. Estimation (niveau A, capture fichier)

| Lot | Description | Charge |
|---|---|---|
| S1 | Device `--mageco`, instanciation 6850 à $03FE, routage I/O + priorité d'adresse | ~150 LOC |
| S2 | Backend MIDI fichier (réutilise FILE), cadence 31250 bauds | ~120 LOC |
| S3 | Suite `make test-midi` + doc agile complète | ~200 LOC tests |

≈ **450–500 LOC** pour un MVP testable. Le niveau B (port ALSA réel) ajoute ~200 LOC
et une dépendance optionnelle.

---

## 5. Risques

- **Conflit d'adresses #3FE/#3FF** avec le miroir VIA et d'autres extensions
  (explicitement signalé par Dbug). Gérable par priorité conditionnelle, à tester.
- **Absence de logiciel de validation côté émulé** : il faut un programme Oric qui
  pilote la carte (le lecteur `.mid` de Fabrice) pour une validation de bout en bout ;
  sinon les tests restent unitaires.
- **Niveau B** introduit une dépendance plateforme (ALSA/CoreMIDI) → garder optionnel.

---

## 6. Recommandation

**Faisable, et à coût modéré** grâce au modèle MC6850 déjà autonome et au routage ACIA
paramétrable. Proposition : un sprint MVP « niveau A » (carte à #3FE/#3FF + backend
fichier MIDI + tests), le port MIDI hôte temps réel (« niveau B ») étant un sprint
suivant optionnel. Décision go/no-go à valider avant ouverture du backlog.

---

## 7. Équivalence émulateur ↔ Oric réel + carte Mageco

Point central pour l'utilisateur : **du point de vue des données MIDI, l'émulateur
et un vrai Oric équipé de la carte sont équivalents**, parce que le MIDI est une
norme universelle. Un programme Oric écrit dans le 6850, qui émet des octets MIDI
à 31250 bauds — que la puce soit physique ou émulée, **le flux d'octets est
identique**. Seule la couche de transport physique diffère.

### Oric physique + carte Mageco

```
Oric réel ──6850──► prise DIN-5 MIDI OUT ──câble MIDI──► [interface USB-MIDI] ──USB──► PC
```

- Signal électrique MIDI réel (boucle de courant, optoisolée par le TIL 111 en
  entrée) sur les prises DIN-5.
- Pour relier au PC : **interface USB-MIDI** matérielle obligatoire.
- Côté PC, **le même logiciel** qu'avec l'émulateur (FluidSynth, DAW, `aconnect`…).
  Le PC ne distingue pas un Oric réel de l'émulateur.

### Tableau comparatif

| | Oric réel + Mageco | Émulateur Phosphoric |
|---|---|---|
| Flux MIDI / données | ✅ identique (norme MIDI) | ✅ identique |
| Transport vers le PC | câbles DIN-5 + **interface USB-MIDI** | **port virtuel** (`file:`/loopback/tcp/pty) — aucun câble |
| Isolation / électronique | optocoupleur réel, risque de bruit | sans objet (logique pure) |
| Conflit d'adresses $03FE/$03FF | **risque réel** si autre extension branchée | priorité de routage logiciel (averti si Microdisc) |
| Timing 31250 bauds | natif (vrai 1 MHz) | émulé fidèlement (320 cycles/octet) |

### Conséquence pratique

Comme l'émulation reproduit fidèlement le 6850 à $03FE/$03FF et le timing 31250
bauds, **un logiciel MIDI Oric développé/testé sur l'émulateur tournera tel quel
sur un Oric réel équipé de la carte**, et réciproquement. La preuve empirique est
le test e2e : un `POKE` BASIC d'un Note On produit, dans la capture fichier, les
octets `90 3C 7F` exacts — soit précisément ce qu'un vrai Oric+Mageco mettrait sur
le fil MIDI OUT.

### Ce qu'un PC permet de faire (par niveau)

- **Niveau A livré (fichier)** : capturer le flux MIDI émis par l'Oric dans un
  `.mid`/`.syx` (lisible par tout lecteur/DAW), ou injecter un fichier en MIDI IN.
  Idéal pour développer/déboguer sans matériel, archiver, faire de la non-régression.
- **Niveau B (à venir, port hôte temps réel)** : l'Oric émulé pilote un synthé
  logiciel (FluidSynth + soundfont GM) ou une DAW, ou un clavier MIDI joue *vers*
  l'Oric, via le séquenceur ALSA (`aconnect`, `a2jmidid`) / CoreMIDI / loopMIDI.

---

## Références code
- `include/io/acia6850.h`, `src/io/acia6850.c` — cœur MC6850 réutilisable
- `src/io/dtl2000.c` — exemple d'intégration d'un 6850 + backend
- `include/io/serial_backend.h` — abstraction de transport (enum `SERIAL_BACKEND_*`)
- `src/main.c:606-614`, `:762-771` — routage ACIA avec priorité d'adresse
- `src/memory/memory.c:93-94` — miroir I/O $0300-$03FF (zone du conflit)
