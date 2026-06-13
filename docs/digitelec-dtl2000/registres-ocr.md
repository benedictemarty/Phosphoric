# DTL 2000 — Registres extraits par OCR (source primaire)

> Extrait par OCR (Tesseract `fra`, 300 dpi) des 3 PDF Digitelec depuis
> le mirror apple2.org.za. **Tous trois sont la version Apple II / RS232**,
> PAS la version Oric. Mais les chips sont identiques (PIA EF6821 + ACIA
> EF6850) et le découpage des 6 octets est offset-pour-offset le même que
> l'Oric `#3F8`–`#3FD`. Les valeurs ci-dessous sont donc **directement
> transposables** à l'Oric (seule l'adresse de base change).

## Sources OCR

| PDF | Pages | Version | Contenu utile |
|-----|-------|---------|---------------|
| `prog_v23.pdf` | 7 | Apple II | **Valeurs de registres exactes** (POKE/PEEK) |
| `notice.pdf` | 5 | Apple II | Mise en route, touches Minitel, logiciel COM |
| `manuel.pdf` | 10 | RS232/V24 | Micro-interrupteurs + circuits 108/105/106/109 |

## Mapping des 6 octets (Apple `$C0n8` → Oric `#3F8`)

| Off | Apple | Oric | Composant | Registre |
|-----|-------|------|-----------|----------|
| 0 | `$C0n8` | `#3F8` | PIA 6821 | Port A (OR) / DDRA — selon CRA bit2 |
| 1 | `$C0n9` | `#3F9` | PIA 6821 | Contrôle A (CRA) |
| 2 | `$C0nA` | `#3FA` | PIA 6821 | Port B — **NON UTILISÉ** par DTL V23 |
| 3 | `$C0nB` | `#3FB` | PIA 6821 | Contrôle B — **NON UTILISÉ** |
| 4 | `$C0nC` | `#3FC` | ACIA 6850 | Contrôle (W) / État (R) |
| 5 | `$C0nD` | `#3FD` | ACIA 6850 | Tx (W) / Rx (R) data |

(« n » Apple = numéro de slot + 8. Confirme le §4.1 du doc de contexte.)

## Initialisation — mode ASYMÉTRIQUE (V23 Appel, terminal Minitel/Télétel)

```basic
POKE #3F9, 0      : REM CRA=0  -> accès DDRA
POKE #3F8, 244    : REM DDRA = $F4 = %11110100 (entrées: b0,b1,b3 ; sorties: b2,b4-b7)
POKE #3FC, 3      : REM ACIA master reset
POKE #3F9, 4      : REM CRA=4 (CR2=1) -> accès Port A (OR)
POKE #3F8, 212    : REM OR = $D4 (ligne ouverte = déconnecté)
POKE #3FC, 73     : REM ACIA contrôle = $49 = 7 bits + parité paire + 1 stop, ÷16
```

## Initialisation — mode SYMÉTRIQUE (V23 Half-Duplex 1200, Oric↔Oric)

```basic
POKE #3F9, 0      : REM CRA=0 -> DDRA
POKE #3F8, 244    : REM DDRA = $F4
POKE #3FC, 3      : REM ACIA master reset   (OCR avait interverti 3/4 lignes 30-40)
POKE #3F9, 4      : REM CRA=4 -> OR
POKE #3F8, 196    : REM OR = $C4 (ligne ouverte, mode symétrique)
POKE #3FC, 85     : REM ACIA contrôle = $55 = 8 bits sans parité 1 stop, ÷16
```

## PIA Port A (OR, `#3F8`) — sémantique des bits confirmée

- **bit 2 = connexion ligne** (circuit 108 / "fermeture de la ligne") :
  - `0` → ligne fermée = **connecté**
  - `1` → ligne ouverte = **déconnecté**
- **bit 4 = sélection mode** : asymétrique (`1`, valeurs $Dx) vs symétrique (`0`, valeurs $Cx).
- Valeurs concrètes :
  | Action | Asymétrique | Symétrique |
  |--------|-------------|------------|
  | Connecter (ligne fermée) | `208` ($D0) | `192` ($C0) |
  | Déconnecter (ligne ouverte) | `212` ($D4) | `196` ($C4) |
- **Numérotation par impulsions** : alterner ouvert(`212`)/fermé(`208`).
  Une impulsion = ouverture ~66 ms + fermeture ~33 ms ; n impulsions = chiffre n
  (le « 0 » = 10 impulsions). ~1 s ligne fermée entre deux trains.

## ACIA `#3FC` — écriture (contrôle)

| Valeur | Hex | Effet |
|--------|-----|-------|
| 3  | $03 | Master reset (obligatoire avant config) |
| 73 | $49 | Config V23 asym (7E1, ÷16) **sans émission** (RTS haut, CR6/bit6=1) |
| 9  | $09 | **Démarrer émission porteuse** (RTS bas, bit6=0) |
| 85 | $55 | Config symétrique (8N1, ÷16) **sans émission** |
| 21 | $15 | Démarrer émission (mode symétrique) |

Le **bit 6** porte l'ordre d'émission porteuse (= circuit 105 RTS) :
`0` = émet, `1` = silence.

## ACIA `#3FC` — lecture (état)

| Bit | Signification | Actif |
|-----|---------------|-------|
| 0 | Caractère reçu (RDRF) — remis à 0 par lecture de `#3FD` | `1` = dispo |
| 1 | Caractère émis effectivement parti (TDRE) | `1` = prêt à écrire |
| 2 | Porteuse du modem distant présente (DCD / circuit 109) | **`0` = présente** (actif bas) |
| 3 | Modem prêt à émettre (CTS / circuit 106) | **`0` = prêt** (actif bas) |

> ⚠️ Détection porteuse (bit 2) : après lecture de `#3FC`, faire une **lecture
> "blanche" de `#3FD`** pour réinitialiser le bit 2.

## ACIA `#3FD` — données

- **Lecture** : dernier caractère reçu (lire quand état bit0=1).
- **Écriture** : caractère à émettre (écrire quand état bit1=1).
- 7 bits ASCII en V23 Minitel (bit7 toujours à 1) ; 8 bits en symétrique.

## Modes & micro-interrupteurs (manuel RS232, sélecteurs 3-4-5)

| Mode | int3 | int4 | int5 | Débit Tx/Rx |
|------|:----:|:----:|:----:|-------------|
| V23 Appel    | 0 | 1 | 1 | 75 / 1200 |
| V23 Réponse  | 1 | 1 | 1 | 1200 / 75 (serveur) |
| V21 Appel    | 0 | 0 | 1 | 300 / 300 |
| V21 Réponse  | 1 | 0 | 1 | 300 / 300 |
| V23 Half Dpx | 0 | 1 | 0 | 1200 / 1200 |

> Carte **DTL V23** seule : uniquement **V23 Appel** + **V23 Half Duplex**.
> V23/V21 Réponse (= serveur) nécessite la carte **DTL PLUS**.

## Correspondances circuits V24 ↔ bits (cohérence émulation)

| Circuit V24 | Rôle | Bit Phosphoric |
|-------------|------|----------------|
| 108 (DTR) | connexion ligne | PIA OR bit 2 |
| 105 (RTS) | demande émission porteuse | ACIA ctrl bit 6 |
| 106 (CTS) | prêt à émettre | ACIA état bit 3 (actif bas) |
| 109 (DCD) | détection porteuse reçue | ACIA état bit 2 (actif bas) |

## Note bouclage (mode symétrique)

En V23 symétrique, un **bouclage interne** fait que les voyants Détection +
Réception s'allument pendant l'émission : l'émetteur reçoit simultanément ce
qu'il envoie (utile pour contrôle de transmission). À modéliser si fidélité.
