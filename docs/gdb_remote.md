# Debugging the 6502 via GDB remote (`--gdb`)

Phosphoric embeds a **GDB Remote Serial Protocol (RSP)** server: you can attach
`gdb`, `lldb` or an IDE (VS Code, CLion) to the emulated Oric to set
breakpoints, single-step, and inspect/modify the 6502's registers and memory.
No other Oric emulator offers this.

## Starting

```bash
./oric1-emu -r roms/basic11b.rom --gdb           # port 1234 (default)
./oric1-emu -r roms/basic11b.rom --gdb=3333      # port of your choice
```

The emulator opens the port and **waits** for the client to connect. The machine
starts halted at the reset vector; GDB drives the execution.

## Attaching GDB

```bash
gdb -ex 'target remote :1234'
```

Then, in GDB:

```
(gdb) info registers          # A X Y SP PC P
(gdb) x/8xb 0xfffc            # read memory (vectors)
(gdb) break *0xc000           # breakpoint on an address
(gdb) continue
(gdb) stepi                   # one instruction step
(gdb) set $pc = 0x0400        # force the PC
(gdb) set {char}0x0400 = 0xa9 # write a byte
(gdb) detach                  # detach (the Oric keeps running)
```

> Mainline `gdb` does not know the `mos6502` architecture: it may print a
> warning, but memory access / breakpoints / step all work. The
> register description is provided by the stub via `target.xml`.

## Execution model

- GDB breakpoints and the native REPL share the **same** `debugger_t`:
  `Z0`/`z0` add/remove entries in the same table as the `b` command.
- **Ctrl-C** in GDB interrupts execution (SIGINT signal, `S02`); the latency
  is at most one frame (~20 ms).
- A client disconnection lets the Oric resume freely.

## Supported RSP commands

`?` · `g`/`G` · `p`/`P` · `m`/`M` · `c`/`s` · `Z0`/`z0`, `Z1`/`z1` (breakpoints) ·
`Z2`/`z2` (write watch), `Z3`/`z3` (read watch), `Z4`/`z4` (access watch) · `H` · `D` · `k` ·
`qSupported`, `qAttached`, `qC`, `qfThreadInfo`/`qsThreadInfo`, `qOffsets`,
`qSymbol`, `qXfer:features:read:target.xml` · `QStartNoAckMode` · `vCont?`/`vCont`.

Register block (`g`/`G`): `A X Y SP PClo PChi P` (7 bytes, PC little-endian).

## Notes

- Memory reads (`m`) have **no side effects**: the $0000-$BFFF range is
  read from RAM (the VIA/ACIA I/O registers are never touched), and
  $C000-$FFFF via the CPU view (ROM/overlay).
- The transport is raw TCP (POSIX sockets), with no external dependency.
