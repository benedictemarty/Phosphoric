# 0003 — Explicit per-cycle tick order, distinct from the I/O dispatch order

- **Status**: accepted (2.4.0, sprint D of the architecture plan)

## Context

On every CPU cycle, peripherals advance one step (`cpu_cycle_tick`,
`src/main.c`): VIA first (it drives the timers and the interrupt), then the
tape, then the peripherals of the I/O bus. The `io_bus[]` table
(`src/io/io_bus.c`) gives the **dispatch** order of accesses ($0300-$03FF):
which peripheral answers an address. These two orders differ historically, and
the tick order has observable effects.

## Decision

The tick order is a separate table, `io_bus_tick_order[]` (Microdisc, Jasmin,
LOCI, ACIA, DTL 2000, Mageco, SP0256, MEA8000), preceded by VIA and tape in
`cpu_cycle_tick`. `io_bus_tick()` walks it, as an unrolled loop with
`__builtin_expect` (a generic loop cost +22 % per frame).

## Consequences

- Unifying the two orders would first require proving byte-for-byte
  equivalence (corpus, save states, `cli_golden`).
- A new timed peripheral is added to `io_bus_tick_order[]`, at the position that
  keeps the measured behaviour.
