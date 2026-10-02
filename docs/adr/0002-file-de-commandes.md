# 0002 — External commands go through a queue drained by the emulator loop

- **Status**: accepted (sprints 92-95, 1.51 → 1.54)

## Context

The emulator runs CPU, VIA and video in a single thread, one frame after the
other. External commands — `--control` (stdin), HTTP API (a server thread), F1
menu — change the machine state (reset, loading a floppy, saving a state…).
Executed from another thread, in the middle of an instruction, they would
corrupt that state.

## Decision

A single interpreter, `control_dispatch()` (`src/control.c`), for every command
source. Producers on another thread (HTTP server) go through `control_queue`
(`include/control_queue.h`): `control_queue_submit()` enqueues a line and waits
for the reply; the emulator loop calls `control_queue_drain()` once per frame and
runs each command on its own thread, at a frame boundary.

## Consequences

- An HTTP command waits at most one frame (20 ms) plus its execution.
- A single consumer: the emulator loop. If it stops without draining the queue,
  producers stay blocked (see ROADMAP: intermittent `test-httpapi` hang at
  shutdown, pre-existing, to be analysed).
- Commands are split by theme since 2.12.0 (`control_cmd_*.c`);
  `control_dispatch()` remains the single entry point.
