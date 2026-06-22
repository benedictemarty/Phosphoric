# Record / replay déterministe des entrées (movie « TAS »)

Phosphoric peut **enregistrer** les entrées clavier d'une session et les
**rejouer** à l'identique. La seule entrée non déterministe de l'émulation est
la matrice clavier (8 octets) ; en la capturant par frame et en la rejouant
bit-à-bit, une session se reproduit exactement.

Cas d'usage : tool-assisted runs (TAS), reproduction de bugs, et **régression
CI** (enregistrer une fois → rejouer headless → comparer une capture d'écran de
référence).

## Utilisation

```bash
# Enregistrer (interactif, SDL)
./oric1-emu -r roms/basic11b.rom --record session.phm

# Rejouer (le clavier live est ignoré)
./oric1-emu -r roms/basic11b.rom --replay session.phm

# Régression CI : rejouer headless puis capturer l'écran final
./oric1-emu -r roms/basic11b.rom -n --replay session.phm --screenshot out.ppm
```

En mode headless, le rejeu **sort automatiquement** une fois le movie épuisé.

## Garantie de déterminisme

Le **rejeu est bit-déterministe** : rejouer le même movie produit toujours la
même sortie (prouvé : deux rejeus → captures d'écran byte-identiques). C'est ce
qui rend la régression CI fiable — on commite un movie + une capture de
référence, et la CI rejoue + compare.

Le déterminisme repose sur : même ROM/modèle, init RAM fixe (compatible
Oricutron), CPU cycle-exact. Le movie stocke le modèle (`model 0|1`) et un
avertissement est émis si le modèle de rejeu diffère.

## Format de fichier (texte, diffable)

```
PHOSPHORIC-MOVIE 1
model 1
F 0 ff ff ff ff ff ff ff ff
F 1 ff ff ff ff ff f7 ff ff
F 5 ff fb ff ff ff ff ff ff
```

- `F <frame> <m0..m7>` : index de frame + 8 octets de la matrice (hex, active-low).
- **Changements seulement** : une ligne n'est écrite que lorsque la matrice
  change ; le rejeu conserve le dernier état entre deux changements.

## Portée et limite

- **Exact** pour l'entrée interactive SDL : le clavier est sondé une fois par
  frame, donc la matrice est constante pendant chaque frame — l'échantillonnage
  par frame est sans perte.
- **`--type-keys`** injecte des touches en milieu de frame (granularité cycle).
  Un movie issu d'un run `--type-keys` rejoue de façon **déterministe**, mais
  pas nécessairement à l'identique du run `--type-keys` live (l'injection
  sous-frame est quantifiée à la frame). Pour des entrées scriptées
  reproductibles, préférez enregistrer puis rejouer le movie.
