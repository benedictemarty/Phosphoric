# 0004 — Changing expansion cards restarts the process

- **Status**: accepted (2.11.0)

## Context

The F1 menu lets the user change the expansion cards (Microdisc, Jasmin, LOCI,
ACIA…). A card is wired at startup: served ROM, overlay, peripheral on the bus,
open serial transport, and for the co-simulated LOCI threads and a running
firmware. Resetting all of that inside the same process would reuse global
state and host resources.

## Decision

"Apply and restart" replaces the process (`execv`; `_execv` on Windows) with the
emulator restarted with the original command line, minus the card options, plus
those of the chosen cards and `--no-config-cards` (`src/cards.c`,
`cards_build_argv`, `cards_exec`). Cards are described by a data registry
(`k_cards[]`) that the menu displays as is.

## Consequences

- It is a real cold restart: the Oric memory is lost (the menu says so).
- The web build cannot restart itself: its cards are read-only.
- A card added to the registry appears in the menu with no other change.
