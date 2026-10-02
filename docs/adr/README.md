# Architecture decision records (ADR)

One file per structuring decision: the context, what was decided, what it costs.
A superseded decision is not deleted: its record is marked "superseded" and
points to the next one.

| No. | Decision | Status |
|-----|----------|--------|
| [0001](0001-makefile-seul-build.md) | The Makefile is the only build system | accepted (2.1.3) |
| [0002](0002-file-de-commandes.md) | External commands go through a queue drained by the emulator loop | accepted (1.52) |
| [0003](0003-ordre-des-ticks.md) | Explicit per-cycle tick order, distinct from the I/O dispatch order | accepted (2.4.0) |
| [0004](0004-cartes-par-relance.md) | Changing expansion cards restarts the process | accepted (2.11.0) |
| [0005](0005-miroirs-fr-en.md) | Two mirror branches, French and English, differing only in comments | accepted (2.1.4) |
