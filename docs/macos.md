# Build sur macOS

Phosphoric compile et tourne nativement sur macOS (Intel et Apple Silicon) via
les Command Line Tools d'Apple (clang) et SDL2 installé par Homebrew.

> **Statut** : le code est **portable macOS** (les dépendances Linux-spécifiques
> sont désormais gardées par plateforme) mais, à la date de rédaction, la
> compilation n'a été **vérifiée que sur Linux** (environnement de développement).
> Un premier build sur un vrai Mac reste à confirmer — remontez tout écart.

## Dépendances

```bash
# Command Line Tools (fournit clang, make, git)
xcode-select --install

# Homebrew : https://brew.sh
brew install sdl2 pkg-config      # affichage/audio/clavier
brew install openssl@3            # optionnel : PicoWiFi TLS (PICOTLS)
```

`gcc` sur macOS est un alias de **clang** (via les Command Line Tools) : le
Makefile fonctionne tel quel. Pour être explicite : `make CC=clang`.

## Compilation

```bash
make                     # build standard avec SDL2 (Homebrew)
make SDL2=0              # build headless (sans SDL2)
make tests               # suite de tests complète
```

Le Makefile détecte SDL2 via `pkg-config`, avec repli sur **`sdl2-config`**
(livré par `brew install sdl2`) si `PKG_CONFIG_PATH` ne pointe pas sur le keg —
robuste sur Apple Silicon (Homebrew dans `/opt/homebrew`).

## Options spécifiques macOS

| Build | Remarque macOS |
|-------|----------------|
| `make MIDI=1` | MIDI temps réel via **CoreMIDI** (frameworks liés automatiquement). Le backend CoreMIDI est écrit selon l'API documentée mais reste à valider sur Mac. |
| `make CAST=1` | Chromecast MJPEG ; nécessite OpenSSL (`brew install openssl@3`, exporter `PKG_CONFIG_PATH` vers son `lib/pkgconfig`). |
| `--serial com:…` | Port série réel via **termios** (POSIX) — désormais activé sur macOS comme sur Linux. |
| `--serial pty` | Pseudo-terminal via `openpty()` (`<util.h>` sur macOS). |

## Détails de portabilité

Les adaptations qui rendent le build macOS possible :

- **PTY** : `openpty()` est déclaré dans `<util.h>` sur macOS/BSD (et non
  `<pty.h>` comme sur Linux) — inclusion conditionnelle dans
  `src/io/serial_backend.c`.
- **COM série** : `HAS_COM` couvre maintenant `__APPLE__` (termios POSIX complet
  sur macOS), plus seulement Linux.
- **`MSG_NOSIGNAL`** : absent sur macOS/BSD ; repli sur `0` dans
  `include/utils/oscompat.h`. SIGPIPE étant ignoré au niveau du process
  (`oscompat_ignore_sigpipe()`), écrire sur une socket morte renvoie `EPIPE`
  au lieu de tuer l'émulateur.
- **SDL2** : détection `pkg-config` → repli `sdl2-config` dans le `Makefile`.

## Non couvert (backends Linux-only)

Le backend `--serial com:` s'appuie sur les constantes de baud POSIX standard ;
les débits non standard (au-delà de `B230400`) ne sont pas exposés. Aucune autre
fonctionnalité n'est désactivée sur macOS.
