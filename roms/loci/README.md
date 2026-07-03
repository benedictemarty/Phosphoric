# LOCI ROM

LOCI ROM v0.3.0 by **Sodiumlightbaby** (sodiumlb), 2024.

- `locirom` — 16 KB raw ROM mapped at $C000-$FFFF (reset vector $FF29).
- `locirom.rp6502` — Same content with RP6502 header (`#!RP6502\n$C000 $400 $D3CA71B4\n`).

## Source

Downloaded from <https://github.com/sodiumlb/loci-rom/releases/tag/v0.3.0>.
Built from <https://github.com/sodiumlb/loci-rom> (cc65 toolchain).

## License

BSD-3-Clause, Copyright (c) 2024 Sodiumlightbaby — see source repository.

## Usage in Phosphoric

```bash
./oric1-emu -r roms/loci/locirom --loci
```

Requires Sprint 34y+ (`--loci` flag for MIA emulation at $03A0-$03BF).
Without `--loci`, the ROM boots but hangs polling the MIA registers.

# Diagnostic ROM (test108k)

Oric Diagnostic ROM v1.08k by **Mike Brown** (May 2023) — step-by-step
test of CPU/ULA/DRAM/VIA/PSG. Included in real LOCI firmware builds with
Mike Brown's permission; booted by a **long press (≥ 2 s)** of the LOCI
Action button (F8 in Phosphoric, or `loci-button long` on --control).

- `test108k.rom` — 50 Hz (PAL) version, 16 KB, mapped at $C000.
- `test108k-60HZ.rom` — 60 Hz screen variant.

## Source

Downloaded from <http://oric.signal11.org.uk/html/diagrom.htm>
(`test108k-bin.zip`). Copyright © Mike Brown 2009-2024.
