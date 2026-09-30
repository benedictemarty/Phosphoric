# Hardware conformance — datasheet audits

This document tracks how Phosphoric's emulation cores conform to the
manufacturers' reference datasheets. Every discrepancy is classified as **fixed**
or **accepted deviation** (with the reason). Accepted deviations are
deliberate modelling choices (functional model rather than bit-for-bit, or
behaviour locked in by existing tests/semantics); they are not
forgotten bugs.

Method: full reading of the datasheet, line-by-line cross-check against the
code, unit tests added for every fix.

---

## 1. WD1793 FDC (`src/storage/disk.c`) vs *Western Digital FD179X-02*

**Functional model over a flat image** (`side·tracks·spt + track·spt + (sec-1)) × 256`),
not bit-for-bit MFM. Real CRC, PLL data separator, gaps and write-precomp are therefore
absent by design.

### Conformant
Register decoding (A1A0), RESET (sector←1, track←0), INTRQ cleared on
status read / command write, step rates 6/12/20/30 ms, rotational
latency 200 ms/rev, BUSY bit, live INDEX/TRK0 patch in the Type I status.

### Fixed (v1.97.0-alpha)
| Discrepancy | Detail | Fix |
|-------|--------|-----------|
| **Read Address → sector register** | The datasheet (p.13) writes the **track** (ID byte 1) into the sector register; the code put the sector number there. | `disk.c`: `fdc->sector = addr_field[0]`. Test `test_fdc_read_address_loads_track`. |
| **STEP/STEP-IN/STEP-OUT: T flag ignored** | Table 2 (Type I bit 4 = Track Update): the track register is only updated if T=1; the code always wrote it. | `fdc_seek_track(...args, bool update_track)`; Restore/Seek pass `true`, Step\* pass `(value & 0x10) != 0`. The head (c_track) always moves. Test `test_fdc_step_track_update_flag`. |
| **Force Interrupt: unconditional INTRQ** | Datasheet p.15: HEX **D0** (i3-i0=0) terminates the command **without** an interrupt; only **D8** (i3) generates an immediate INTRQ. The code always raised INTRQ. | `if (value & 0x08) set_intrq(...)`. Tests `test_fdc_force_interrupt` (D0 = no INTRQ) + `test_fdc_force_interrupt_immediate` (D8). |

### Fixed (2.0.0-alpha.6, epic V2-E6)
| Discrepancy | Detail | Fix |
|-------|--------|-----------|
| **LOST DATA (S2) never set** | At 250 kbit/s in MFM, a byte goes by every **32 µs**: beyond that, an unserviced DRQ means the next byte is already there. The WD1793 raises S2 and carries on — this is how software knows it has missed the train. The bit was never raised. | Watchdog on the DRQ age (`FDC_BYTE_CYCLES` = 32, justified by the data rate): after more than one byte time, S2 is raised and counted (`lost_data_count`, reported at the end of the session). Tests `test_fdc_lost_data_when_cpu_too_slow` and its counter-check `test_fdc_no_lost_data_when_cpu_keeps_up`. Verified: **no false positive** on the 6 disks of the corpus. |
| **Write-protect (S6) not modelled** | Writes were always accepted, even on a protected medium. | `fdc_set_write_protect()`: Write Sector and Write Track are refused without modifying anything, status bit 6 + interrupt; the bit also appears in the Type I status. Wired to `--disk-write-protect` **and inferred from the file itself** (a `.dsk` that is read-only on the host behaves like a disk with its tab open). 3 tests. |
| **Multi-sector: no terminal RNF** | A multi-sector command silently stopped at the end of the track. The WD1793 keeps searching for the next sector and ends with **RECORD NOT FOUND**: this is what tells the software where the track ends. | RNF added to the final status, for reads as well as writes. |
| **Missing disk in Type II/III → RNF instead of NOT READY** | — | Already fixed before this sprint (`fdc_not_ready()`, called by the four Type II/III commands); the line in this document was **stale**. |

### Accepted deviations
| Discrepancy | Reason |
|-------|--------|
| **The lost byte is not really lost**: S2 is signalled at the right moment, but the data remain intact | Our flat-image model has no continuous MFM stream: bytes are served on CPU demand, not by the rotation. Software that tests S2 sees the right condition; software that ignores it gets correct data where the hardware would give it wrong data. Really scrolling the stream requires the MFM track model (backlog V2-E6/US6.3) — and would put disk loading at risk for a theoretical gain: no known Oric software is too slow. |
| READ TRACK without completion (no case in `fdc_read`) | Command hardly ever used by Oric software, and a correct implementation would require **synthesising an MFM track** (gaps, address marks) — that is the track model in the backlog. |
| S3 CRC ERROR never set | Structurally impossible on a flat image; damaged sectors are reported as RNF (S4). |
| C/S flags (side compare) and a0 (Deleted DAM) ignored | The side is driven by the Microdisc control register. |

---

## 2. VIA 6522 (`src/io/via6522.c`) vs *Rockwell R6522 Rev.9*

### Conformant
Map of the 16 registers (Table 1); IFR/IER bits (Fig 29); IER bit7
set/clear semantics + read bit7=1; flags cleared by ORA/ORB access; independent
CA2/CB2 interrupt modes; input latch (ACR 0-1);
CA2/CB2 handshake/pulse; the 8 shift-register modes (rotation
MSB→bit0, T2 rate=N+2, φ2=÷2, mode 4 free-run without flag).

### Corrigé (v1.98.0-alpha) — deux points annulés en 2.6.0 par la mesure (voir « Corrigé (2.6.0) »)
| Écart | Détail | Correctif |
|-------|--------|-----------|
| **One-shot T1/T2 froze after timeout** | Datasheet p.8/p.9: the counter keeps decrementing (only the flag stops re-arming) so that the host can read the time elapsed since the interrupt. | New field `t1_active/t2_active` (counter is counting) distinct from `t1_running/t2_running` (firing still possible). Counting is gated on `*_active` (stays true after timeout); firing/reload on `*_running`. The `t1_running=false` after timeout semantics is **preserved** (savestate/control/debugger + existing tests untouched). Tests `test_timer1/2_one_shot_counter_continues`. |
| **PB7 timer output: missing DDRB.7 condition** | Datasheet p.9: PB7 is the Timer 1 output only if **DDRB.7 AND ACR.7 = 1**; the code only tested ACR.7. | Gate `(acr & 0x80) && (ddrb & 0x80)` at the 4 sites (ORB read, T1CH pull-low, underflow toggle, `via_get_pb7`). Evidence: **unit test `test_pb7_timer_requires_ddrb7`** (DDRB.7=0 → normal pin; =1 → timer output) **+ strong integration evidence** (v1.99.1): the CSAVE→`--tape-out-capture`→CLOAD roundtrip passes end to end (`test-tape-roundtrip`), which really exercises the PB7 Timer 1 output under the DDRB.7 gate during a real ROM CSAVE. |

### Fixed (2.0.0-alpha.2, epic V2-E3)
| Discrepancy | Detail | Fix |
|-------|--------|-----------|
| **Timer 1 period 2 cycles too short** (former accepted deviation no. 2) | Fig 16: period **N+2**. The 6522 underflow is not reaching zero but the transition from `$0000` to `$FFFF` (one cycle later), and reloading from the latch takes one more cycle. The code fired as soon as it hit zero and reloaded in the same cycle: period **N**. An error of 0.02 % at 100 Hz, but **20 % for N=10** — audible on short sounds and digidrums. | **Cycle-by-cycle** counting with underflow on `$0000 → $FFFF` and a new field `t1_reload` for the reload cycle (`via6522.c`). Same for Timer 2 (without reload). **The reason given for not fixing it turned out to be unfounded**: the byte-exact baselines are intact — identical Atmos boot, **13 real programs** (6 disks, 7 tapes) identical on screen, `make tests` fully green including the tape roundtrip that exercises PB7 at 416/624 cycles. What made the fix safe: the machine now advances cycle by cycle (epics V2-E1/E2), so the timer is no longer approximated to ±6 cycles by the batches. Vectors: `test_via_t1_freerun_period_is_n_plus_2` (N = 1, 2, 5, 10, 100, 999, 9998), `test_via_t1_oneshot_timeout_cycle`, `test_via_pb7_square_wave_period`, `test_via_t1_counter_readback`, `test_via_ca2_pulse_lasts_one_cycle`, and the integration test `test_via_t1_frame_rate_over_50_frames` (exactly 50 interrupts in 50 frames — a single cycle of drift would make it fail). |

### Corrigé (2.6.0) — comportement mesuré sur un vrai 6522

Report des correctifs de Neo6502Vic20 (US-30), qui a repris cette VIA et l'a confrontée
aux programmes de test VIC-20 de VICE (`testprogs/VIC20` : viavarious, via_sr, via_pb7,
via_wrap, via_t1crash, via_t1irqack, via_mapping), dont les références sont **mesurées
sur matériel réel** : 61/61 là-bas (≈ 12 avant). Là où la mesure contredit la datasheet,
la mesure l'emporte ; deux correctifs de la v1.98.0 (ci-dessus) sont ainsi **annulés**.
Ces programmes tournent sur la machine VIC-20 de Neo6502Vic20, pas dans la CI de
Phosphoric ; ici, les règles sont verrouillées par des tests unitaires (`test-io`).

| Écart | Mesure | Correctif | Test |
|-------|--------|-----------|------|
| One-shot : le compteur s'enroulait après le time-out | viavarious via1, via4 : T1 se **recharge depuis le latch** à chaque sous-dépassement, en one-shot aussi (seule l'IRQ est unique) — contredit la datasheet et la v1.98.0 | rechargement dans tous les modes | `test_via_t1_oneshot_reloads_from_latch` |
| Compteurs en avance d'un cycle | viavarious via1-5 : T1/T2 ne décomptent qu'au cycle **suivant** l'écriture de T1C-H / T2C-H (IRQ à N+2 cycles de l'écriture) | `t1_reload` à l'écriture, champ `t2_hold` | `test_via_t1_underflow_one_cycle_after_zero`, `…_t2_…`, `…_oneshot_timeout_cycle`, `…_counter_readback` |
| Bascule du mode de T2 immédiate | via1 G, via2, via9 : le mode (φ2 / PB6, ACR bit 5) change au cycle suivant, dans les deux sens | champ `t2_phi2` | `test_via_t2_mode_switch_next_cycle` |
| T2 sur 16 bits quand il cadence le SR | via20, via21 : T2 compte sur **8 bits** (octet bas rechargé, période latch + 2), IRQ unique au passage des 16 bits à `$FFFF` | champ `t2_reload` | `test_via_t2_8bit_when_sr_uses_t2` |
| PB7 conditionnée à DDRB.7 | via10-13, via_pb7 : PB7 est la sortie de T1 dès que ACR bit 7 = 1, **même avec DDRB.7 = 0** ; bascule à 1 au RESET, 0 à l'écriture de T1C-H, 1 quand ACR bit 7 passe à 1, change d'état à chaque IRQ T1 — contredit la datasheet et la v1.98.0 | conditions DDRB retirées | `test_pb7_timer_output_ignores_ddrb7`, `test_via_pb7_toggle_rules` ; CSAVE réel : `test-tape-roundtrip` vert |
| Registre à décalage | via_sr (8 modes) : 16 demi-périodes de CB1 démarrées par toute lecture **ou** écriture du SR ; sortie aux états pairs, entrée aux impairs ; T2 : événement 2 cycles après chaque sous-dépassement de l'octet bas ; φ2 : un événement par cycle, le premier 3 cycles après l'ACR (1 après l'accès) ; ACR = 000 tient le drapeau à 0 | SR réécrit (événements) | `test_sr_shift_out_phi2`, `test_sr_shift_in_phi2`, `test_via_sr_any_access_starts_and_acr0_clears_flag` |

Sur l'Oric : corpus de 36 programmes réels identique à l'écran, boots, CSAVE/CLOAD et
chargement au signal inchangés ; la section `.ost` VIA gagne 5 octets (état T2 / SR ;
un `.ost` antérieur se relit). Coût mesuré : ≈ +4 % par trame.

### Déviations assumées (restantes)
| # | Écart | Datasheet | Raison |
|---|-------|-----------|--------|
| 2b | Le demi-cycle du time-out one-shot (N+1,5) n'est pas représenté ; depuis 2.6.0 le drapeau tombe au cycle mesuré sur vrai 6522 (N+2 cycles après le cycle d'écriture, référence viavarious) | « N+1,5 cycles après l'écriture de T1C-H » | Un demi-cycle n'est pas représentable à la granularité du cycle entier (il faudrait le niveau N4, cf. `docs/ACCURACY.md`). La **période** du mode continu, elle, est exacte — c'est elle qui fixe les fréquences. |
| 4 | Écriture T1L-H (reg 7) efface le flag T1 | Fig 12/13 : seule l'écriture T1C-H (reg 5) l'efface explicitement | Comportement exact de reg 7 sur le flag **incertain** (divergence entre datasheets MOS et Rockwell) → pas de correction sans confirmation (principe : ne pas inventer). |
| 5 | RESET efface compteurs/latches/SR | La datasheet dit qu'ils sont **préservés** | Sans conséquence (état power-on indéfini) ; `test_via_reset` verrouille l'état actuel. |

---

---

## 3. AY-3-8912 PSG (`src/audio/ay3891x.c`) vs *General Instrument AY-3-8910/8912* datasheet

Since 2.0.0-alpha.4: machine clocked at **clock/8** (125 kHz), output
**integrated** over the steps covered by each sample (previously: fractional
accumulators at the 44.1 kHz sample rate).

### Conformant (verified by MEASURING the produced signal)
- **Tone** = `clock/(16×TP)` ✓ — measured to ±0.1 % for TP = 4, 8, 16, 50, 100, 284, 500.
- **Envelope**: step = `clock/(8×EP)`, cycle of **32** states = `clock/(256×EP)` ✓ —
  measured to ±2 % for EP = 100, 200, 500, 1000.
- **Noise LFSR** 17 bits, taps bit0 ⊕ bit3 ✓ (sequence compared step by step with a
  reference; never stuck at zero; output balanced to ±5 %).
- **Mixer** R7: bit=1 disables the channel's tone/noise ✓.
- Widths: tone 12 bits, noise 5 bits, envelope 16 bits ✓; `TP=0→1` ✓;
  envelope shapes 0-15 ✓; 16-level log volume table ✓.

### Fixed (2.0.0-alpha.4, epic V2-E5)
| Discrepancy | Detail | Fix |
|-------|--------|-----------|
| **Envelope 2× too slow** | One step every `clock/(16×EP)` instead of `clock/(8×EP)`. Yet this document declared it **conformant**: its recalculation assumed a cycle of **16 states**, whereas the envelope counter has **32**. With 32 states, `clock/(16×EP)` per step gives a cycle of `clock/(512×EP)` — twice too slow compared with the datasheet's `clock/(256×EP)`. **A wrong assumption invisible to recalculation, revealed by measuring the signal.** | Envelope counter clocked by the internal `clock/8` clock, one step every `EP`. Measured: decay duration conformant to ±2 % over EP = 100…1000 (test `test_ay_envelope_period_matches_datasheet`). |
| **Aliasing above Nyquist** | The counters were clocked by accumulators at the sample rate: any transition faster than 44.1 kHz folded back into audible noise instead of being attenuated. Measured: a tone at TP=1 (62.5 kHz) came out at 18.4 kHz with full amplitude. | Machine clocked at `clock/8` and output **integrated** over the steps covered by each sample (box filter). TP=1 is now attenuated (RMS divided by ~3) instead of aliasing — which is also what the speaker of a real ORIC does. Test `test_ay_no_aliasing_above_nyquist`. |

### Fixed (v1.99.0-alpha)
| Discrepancy | Detail | Fix |
|-------|--------|-----------|
| **Noise 2× too fast** | The noise generator reused `tone_rate` (`clock/8` = the tone's **toggle** rate) to clock the LFSR. But noise has no ÷2 toggle: the datasheet gives `clock/(16×NP)` (same /16 prescaler as the tone). MAME models this missing ÷2 through its `prescale_noise`. | New `noise_rate = clock/16` passed to `ay_step_sample()` (distinct from `tone_rate`). Test `test_ay_noise_rate_clock_div16` (reference LFSR over the exact number of steps). **Empirical evidence**: `--audio-wav` capture — `PING` (tone) byte-for-byte identical to the HEAD binary, `EXPLODE` (noise) differs → only the noise changes, no tone/envelope regression. |

### The ORIC output stage (read from the official schematic)

These values come from the Oric-1/Atmos schematic (sheet *PSG 1 Keyboard*, `audio.sch`,
revision Issue 6.1) — see `docs/architecture/oric-audio-output.md` for the details
and calculations. They made it possible to **lift two deviations** previously
"accepted", and to clarify a third one.

| Element | Value | Role | Modelled? |
|---------|--------|------|------------|
| `R4` | 1 kΩ | common load on which CH_A, CH_B and CH_C are **tied together** | **yes** — this parallel mixing is what *averages* the three channels |
| `R2` / `R3` | 4.7 kΩ / 470 Ω | voltage divider (−20.8 dB) | no — this is **gain**, made up by the LM386 amplifier (×20) |
| `C5` | 10 nF | low-pass on `R2 ∥ R3` = 427 Ω → **f_c = 37.2 kHz** | no — **out of band**: above Nyquist at 44.1 kHz, and per-sample integration already covers that range |
| `C4` | *ambiguous* | coupling to the amplifier → **blocks DC** | **yes**, for DC blocking only (see below) |
| `LM386` + `SP1` | — | amplifier and internal speaker | no — response not measured |

### Fixed (2.0.0-alpha.5)
| Discrepancy | Detail | Fix |
|-------|--------|-----------|
| **DC component in the output** | The PSG signal is unipolar (0 → +max): measured, a DC offset of **+8188** for three channels at full volume, i.e. half the amplitude. On the machine, `C4` couples the PSG to the amplifier and does **not** let this DC through. We were therefore sending a permanent offset to the DAC — wasted dynamic range and a "click" at the start and end of every sound. | Fixed-point DC blocking (Q16 estimator, time constant of 4096 samples ≈ **1.7 Hz**, hence inaudible), which can be disabled with `dc_block_off` to observe the bare generator. Measured after the fix: DC **+8188 → +15**, and a **symmetrical** signal (−8232 / +8252 instead of 0 / +16383). Tests `test_ay_output_has_no_dc_offset`, `test_ay_dc_block_preserves_audio`. |

### Lifted deviations (2.0.0-alpha.5)
| Former deviation | Verdict |
|--------------------|---------|
| "Mixing of the three channels by a sum divided by 3 — the real AY sums **currents**, the sum is not perfectly linear" | **Unfounded.** The schematic shows CH_A/CH_B/CH_C **tied together** on `R4`: in parallel, the outputs do not add up, they **average**. The measurement reported on the Defence Force forum confirms it: a single channel at 1 V gives ≈ 0.33 V, not 1 V. Our `sum / 3` **is** that behaviour. Locked in by `test_ay_parallel_mixing_averages_channels` (the dynamic range of 3 channels is 3× that of a single one, to within 5 %). |
| "No analogue low-pass filter" | **Irrelevant at 44.1 kHz.** The only low-pass in the circuit (`C5` on `R2 ∥ R3`) cuts off at **37.2 kHz**, above the representable band. |

### Accepted deviations
| Discrepancy | Reason |
|-------|--------|
| The **`C4` coupling is not modelled as an audible high-pass**: only the DC blocking is, at 1.7 Hz | The value of `C4` **cannot be determined** from this document: it is marked "2k2" there, with no unit, whereas the other capacitors on the sheet carry theirs (`10n`, `47n`, `220uF`). The two plausible readings give very different circuits — **2.2 nF → high-pass at ≈ 3.3 kHz** (the sound would lose all its bass) or **2.2 µF → ≈ 3.3 Hz** (plain DC blocking). We do not decide by guesswork: only the **certain** effect is modelled. A measurement on a real machine (or a photo of the PCB) would settle it. |
| Response of the LM386 and of the internal speaker | Not measured. What the emulator reproduces is the **electrical signal**, not the response of the small speaker — which is also what anyone listening on headphones expects. |
| The AY-3-8912 has **no Port B** (reg 15) but the code models it (`audio.h`, init 0xFF) | No effect on the Oric (Port B unused); cosmetic. |
| R7 bit 6 semantics (Port A direction): the code returns the keyboard input when bit6=1, whereas the datasheet describes R7 bits 6/7 as the port direction | **Empirically validated** on the Oric (keyboard tested extensively) → deliberate Oric-specific model; not changed without confirmation (principle: do not invent). |


---

## History
- **v1.99.0-alpha** (2026-08-12) — AY-3-8912 PSG fix: noise rate
  brought back to `clock/(16×NP)` (was 2× too fast). +1 `test-audio` test (13).
  Empirical evidence with `--audio-wav` (tone unchanged, noise changed). Accepted
  deviations: Port B absent on the 8912, R7 bit6 semantics.
- **v1.98.0-alpha** (2026-08-12) — VIA 6522 fixes: one-shot T1/T2 that
  keeps decrementing after timeout (`t1_active/t2_active`), and DDRB.7 gating
  on the PB7 timer output. +3 `test-io` tests (46). Remaining as accepted deviations:
  T1 free-run N+2 (baseline risk), T1L-H flag (uncertain), counter reset.
- **v1.97.0-alpha** (2026-08-12) — WD1793 audit (FD179X-02 datasheet) + VIA 6522
  (R6522 Rev.9 datasheet). 3 WD1793 fixes (Read Address, T flag, Force
  Interrupt D0/D8) + 3 tests. VIA deviations documented.
