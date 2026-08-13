# Phosphoric 1.93.0-alpha — I/O device bus redesign + headless capture tools

*Draft for forum.defence-force.org (Emulators / Oric section). English, as per the
forum's convention. A French version can be produced on request.*

---

**Subject:** Phosphoric 1.93.0-alpha — full I/O bus redesign + new headless capture tools

Hi everyone,

Phosphoric — my cycle-accurate ORIC-1 / Atmos emulator written in C11 — has a new
development release (**v1.93.0-alpha**). It does not add flashy end-user features;
instead it closes a long internal cleanup effort and ships a few tools that people
building demos, automated tests or captures should find handy. Here is what changed,
and — more importantly — *why the architecture rework matters*.

## The headline: a real I/O device bus

Like most 8-bit emulators, Phosphoric grew organically. Every device that lives in the
ORIC's page-3 I/O window ($0300–$03FF) — the Microdisc WD1793 FDC, the ACIA 6551 serial
chip, the LOCI SD interface, the Digitelec DTL 2000 modem, the Mageco/ORICON MIDI
board — was wired **by hand** into a giant `main.c`.

The read/write dispatch was a cascade of hard-coded conditions:

```c
if (has_loci   && loci_in_range(addr))     return loci_read(...);
if (has_acia   && acia_in_range(addr))     return acia_read(...);
if (has_microdisc && mdc_in_range(addr))   return microdisc_read(...);
/* ... and so on, with cross-dependencies between them ... */
```

The nasty part is the *cross-dependencies*: the ACIA and the Microdisc overlap; the
ORICON window sits on top of the Microdisc; an ACIA relocated to $0380 has to consult
the LOCI's MIA reliability. All of that priority logic lived inline, duplicated across
the read path, the write path, the tick loop, the savestate code and the CLI glue.
`main.c` had swollen to ~4690 lines and adding or removing a device meant touching it
in five or more places. (Removing the old OCULA experiment earlier had shown just how
painful that was.)

**The redesign replaces the cascade with a device bus.** There is now a single small
contract:

```c
typedef struct io_device_s {
    const char* name;
    bool    (*claims)(emulator_t* emu, uint16_t addr);           /* does this device own this address? */
    uint8_t (*read)(emulator_t* emu, uint16_t addr);
    bool    (*write)(emulator_t* emu, uint16_t addr, uint8_t v); /* true = consumed, false = fall back to VIA */
    bool    (*claims_write)(emulator_t* emu, uint16_t addr);     /* optional, for asymmetric read/write claims */
    /* + save_tag / save / load hooks for savestate */
} io_device_t;
```

Devices are registered in an ordered table `io_bus[]`; the order **is** the priority.
The read/write callbacks collapse to *a single loop over the bus plus the VIA
fall-back*. Every page-3 device — Microdisc, ACIA, LOCI and its three disjoint
sub-windows, DTL2000, Mageco — now lives on the bus. Adding a device is one table entry;
no more surgery on the core.

A couple of contract details that turned out to matter for correctness:

- **`write` returns "consumed".** A device can decline a write (return `false`) and let
  it fall back to the VIA — needed when a device must *observe* writes to its window
  without necessarily overriding the default VIA behaviour. Impossible to express with a
  plain `void` write.
- **`claims_write` is separate from `claims`.** A device's write claim can differ from
  its read claim. Exclusive-range devices simply leave it `NULL` and always return
  `true`.

Beyond the dispatch, two more pieces of the monolith were extracted the same way:

- **Savestate is now per-device.** Each device can persist its own `.ost` section
  through the contract (DTL2000 and Mageco done). A device that has nothing to save emits
  no section, so a normal-boot save file stays byte-for-byte identical — zero regression
  for existing save states. LOCI is deliberately left out for now (its OS file handles
  aren't serialisable as-is).
- **Peripheral tick moved out of `main.c`** into `io_bus_tick()`, preserving the exact
  historical order so timing stays identical.

The result: `main.c` went from **4690 down to 4334 lines**, and the core (6502, memory,
VIA, ULA, PSG) was not touched at all.

## Proven iso-behaviour — the part I care about most

Every single step of this rework was verified **byte-for-byte identical** against the
previous binary: golden PPM screenshots of a BASIC boot, a Sedoric/Microdisc boot and a
LOCI SD boot were compared before and after each migration, and the full test suite
(900+ checks) stayed green throughout. The whole point of a
"strangler" refactor is that nothing observable changes — so if you were happy with
Phosphoric before, this release behaves exactly the same, just on far cleaner
foundations.

If you're curious about the gory details, the design notes and the critical
architecture audit that kicked this off are in the repo under
`docs/architecture/io-bus.md` and `docs/architecture/AUDIT-ARCHITECTURE.md`.

## New tools for headless / automation users

Alongside the cleanup, a few headless capture features landed (handy for CI, for
demo-making, and for anyone validating output without a screen):

- **`--screenshot-text FILE` / `--screenshot-ansi FILE`** — dump the actual text
  content of the screen ($BB80, 40×28) as plain ASCII, or a true-colour ANSI rendering
  of the framebuffer.
- **`--screenshot-text-at C:FILE` / `--screenshot-ansi-at C:FILE`** — same, but captured
  at a given CPU cycle (same model as the existing `--screenshot-at`).
- **`--audio-wav FILE` / `--psg-trace FILE`** — render the AY-3-8910 PSG to a 16-bit
  stereo WAV, or log every sound-register write with its CPU cycle. Same audio engine as
  the SDL2 path, so what you hear is what you capture.
- **`--video`** now embeds a PCM audio track in the headless AVI recording (it used to be
  a silent MJPEG).

## Try it

```
make                        # native + SDL2 build
./oric1-emu -r roms/basic11b.rom
./oric1-emu -r roms/basic11b.rom --headless --screenshot-text-at 3000000:boot.txt
```

Feedback, bug reports and — especially — testing on real ORIC software are very
welcome. This is still an alpha, so I'd like as many eyes on it as possible before
moving towards a stable release.

Thanks, and happy hacking,
bmarty
```

*(Formatting note: this is Markdown; for the phpBB forum I can convert the code blocks
to `[code]…[/code]` BBCode on request.)*
