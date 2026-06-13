# Digitelec DTL 2000 — documentation source

Dossier d'archives pour l'émulation fidèle du modem **Digitelec DTL 2000**
(carte V23, PIA 6821 + ACIA 6850, memory-mapped `$03F8-$03FD`).
Voir l'implémentation : `src/io/dtl2000.c`, `include/io/dtl2000.h`,
tests `tests/unit/test_dtl2000.c`, exemple `examples/dtl2000-test.bas`.

## Contenu

| Fichier | Description |
|---------|-------------|
| [`registres-ocr.md`](registres-ocr.md) | **Référence figée** des registres (PIA + ACIA), valeurs bit-à-bit, modes, conflit page 3 — consolidée depuis l'OCR |
| [`contexte.md`](contexte.md) | Document de contexte initial (presse + manuels d'époque), avec les points qui étaient « à confirmer » |
| [`ocr/`](ocr/) | OCR brut (Tesseract `fra`, 300 dpi) des 22 pages des manuels d'époque |

## Provenance

Les 3 manuels d'époque (scans image non océrisables) ont été récupérés depuis
le mirror **apple2.org.za** (Apple II Documentation Project), puis rasterisés
(300 dpi, niveaux de gris) et océrisés avec **Tesseract `fra`** :

- `prog_v23-*.txt` — « Programmation carte DTL V23 » (7 pages) → **valeurs de
  registres exactes** (POKE/PEEK), c'est la source primaire des constantes.
- `notice-*.txt` — « Notice d'utilisation » (5 pages) → mise en route, touches
  Minitel simulées, logiciel de communication inter-machines.
- `manuel-*.txt` — manuel RS232/V24 générique (10 pages) → micro-interrupteurs
  + correspondance circuits 108/105/106/109 (DTR/RTS/CTS/DCD).

> ⚠️ Les 3 PDF sont la variante **Apple II / RS232**, *pas* Oric. Les chips sont
> identiques (PIA EF6821 + ACIA EF6850) et le découpage des 6 octets est
> offset-pour-offset le même que l'Oric `$03F8-$03FD` : les valeurs transposent
> directement (seule l'adresse de base change, Apple `$C0n8` → Oric `$03F8`).

## Non inclus (volontairement)

- Les **PDF** sources (~19 Mo) et les **PNG** 300 dpi : trop volumineux pour le
  dépôt et toujours disponibles en ligne (URLs dans `contexte.md` §12).
- Aucun **logiciel Oric d'origine** du DTL 2000 n'a pu être localisé (la cassette
  livrée n'est pas archivée publiquement ; Loritel/Tortosa pilotent le Minitel
  via RS-232, pas la carte `$03F8`). D'où le programme de test maison
  `examples/dtl2000-test.bas`, dérivé des séquences POKE/PEEK de l'OCR.
