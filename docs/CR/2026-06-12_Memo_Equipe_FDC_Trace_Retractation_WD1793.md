# Team memo — Retraction of the WD1793 "track+1" bug report + FDC_TRACE tooling (v1.16.84-alpha)

**Date**: 2026-06-12
**Sprint**: 38
**Audience**: Phosphoric team
**TL;DR**: the external bug report "WD1793 reads track N+1 after a VRAM scroll"
(SCUMM-Oric, 2026-05-19) is **retracted — Phosphoric never had this bug**.
The investigation delivered a new diagnostic tool (`FDC_TRACE=1`) and revealed a
**real** contract regression in `--dump-ram-at` (48 KB instead of 64 KB),
now fixed. Two process lessons in §5.

---

## 1. The reported bug

The SCUMM-Oric project reported (formal report + bisection): after a pure VRAM
scroll routine (~130k cycles, zero I/O access), every `fdc_load_room` read
track N+1. Neither Force Interrupt ($D0) nor RESTORE "fixed" it. The hypothesis
pointed at our WD1793 state machine (motor timeout, spurious cycle-based
STEP).

The report was credible: reproducible symptom, clean bisection,
byte-exact data to back it up (LOAD_BUF = track 3 contents verified 5300/5300).

## 2. The method: FDC trace with the 6502 PC

Rather than auditing the state machine blindly, we instrumented it:

```
FDC_TRACE=1 ./oric1-emu --headless ... 2>trace.log
```

Output (new, sprint 38):

```
[FDC] PC=055F cyc=8692214 write $0313 = 02      ← who writes, when, what
[FDC] seek target=2 (c_track=0 track_reg=0 data=02)
[FDC] READ c_track=2 sector=1 side=0 ok
```

- **main.c** (`io_write_callback`): every $0310-$031F write with PC + cycle;
- **storage/disk.c**: every SEEK (target + internal state) and every READ
  (track/sector/side, found/NOT_FOUND);
- zero cost if the environment variable is absent (`getenv` evaluated once).

## 3. Verdict: the 6502 had stopped talking to us

The trace of the incriminated scenario showed the first load (track 3,
21 sectors, nominal)… then **nothing more**. No FDC command after the
scrolls. The "wrong content" was the leftover of the previous load.

Root cause on the SCUMM-Oric side: their test harness counted its scroll loop
in the X register, clobbered by a routine called with `jsr` →
infinite loop → the second load was never executed (witness variable
`cam_x` = 170 instead of 20 in the RAM dump). Counter moved to
memory → seek to track 2 + 9 correct READs, byte-exact 2273/2273.

Formal retraction archived on the reporter's side:
`SCUMM/docs/phosphoric_bug_report_wd1793_scroll.md`.

**Phosphoric is cleared. No change to the WD1793 emulation
was needed — and none should have been.**

## 4. The real finding: `--dump-ram-at` contract regression

Along the way, the reporter's memdump validations were failing with "dump too
short (49152, expected 65536)". Causal chain:

1. **Origin**: `fwrite(emu->memory.ram, 1, 0x10000, f)` on an array of
   49152 bytes — a *buffer over-read* (UB) that "by luck" dumped 64 KB
   by reading the adjacent `rom[]` in the struct. Consumers were
   built on this behaviour.
2. **b2af997** (2026-05-14, cppcheck pass): the overflow is truncated to 48 KB.
   A **correct** memory fix, but the documented contract (`--help`: "Dump
   64KB RAM") was not updated, nor were the consumers checked.
3. **A month of invisibility**: the deployed binary had not been rebuilt
   since — the regression lay dormant in HEAD without being shipped.
4. **Sprint 38**: 64 KB contract properly restored — $0000-$BFFF = raw
   RAM, $C000-$FFFF = **banked CPU view** (BASIC ROM / Microdisc overlay /
   upper RAM), read without side effects (the I/O page is not traversed).
   The interactive **F7** dump is aligned on the same contract.

## 5. Process lessons

1. **Deployed binary ≠ HEAD.** The dump regression lived for a month because
   `oric1-emu` was not rebuilt after each commit. Proposal:
   systematic rebuild at the end of each sprint (or CI producing the binary), and
   external suites (SCUMM-Oric) run against that binary.
2. **A memory-safety fix can break a contract.** When fixing
   an overflow detected by a tool, check what the code *meant* to
   do (here: dump 64 KB) and make `--help`/docs/consumers
   consistent — not just silence the tool.
3. **Require a trace for every FDC bug report.** Proposed policy:
   every external report on the disk subsystem must attach a
   `FDC_TRACE=1` output. It would have killed this one in ten minutes instead of three
   weeks of false leads on the reporter's side.

## 6. Open backlog

- [ ] `test_renderer_init_headless` fails in the `SDL2=1` profile (mode 0x3 vs
  0x1 expected) — pre-existing, independent of sprint 38, tracked in the ROADMAP.

## 7. References

| What | Where |
|------|-----|
| FDC trace | `src/storage/disk.c` (`fdc_trace_enabled`, SEEK, READ), `src/main.c` (writes + PC) |
| 64 KB dump fix | `src/main.c` (`--dump-ram-at` + F7) |
| Commits | `f7f988c` (sprint 38), `b2af997` (origin of the regression, 2026-05-14) |
| Retraction on the reporter's side | `SCUMM/docs/phosphoric_bug_report_wd1793_scroll.md` |
| Version | 1.16.84-alpha — CHANGELOG / ROADMAP / VERSION_TRACKING up to date |
