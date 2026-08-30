# Phosphoric IPC Control Protocol

Activate with `--control`. The emulator speaks a line-based text protocol
on stdin/stdout. Logs go to **stderr** so stdout stays a clean channel.

## Message kinds

| Direction | Prefix | Purpose |
|-----------|--------|---------|
| CMD       | (none) | Client → emu : one command per line |
| REP       | `OK ` or `ERR ` | Emu → client : reply to a CMD |
| EVT       | `EVT ` | Emu → client : spontaneous (e.g. break hit) |

All values are space-separated `key=value` tokens. **All numeric
arguments are parsed as hex** (`$XXXX`, `0xXXXX`, or bare `XXXX`).
Lengths follow the same rule — `read $F800 100` means 256 bytes, not
100. Format your client to use `${value:X}` explicitly to avoid
decimal-vs-hex surprises.

## Lifecycle

1. Emulator boots → emits `EVT ready pc=… cycles=0 version=…`.
2. Emulator stops immediately (control mode implies `--debug`-style
   initial break) → emits `EVT stopped pc=… cycles=0 reason=break`.
3. Client sends commands. Synchronous commands reply with `OK` /
   `ERR` and stay paused. Resume commands (`step`, `next`, `continue`)
   reply `OK` then hand control back to the CPU.
4. When the CPU stops again (breakpoint hit, step terminated, watchpoint),
   emulator emits `EVT stopped …` and waits for the next command.

## Commands implemented in 35a+35b

| CMD | Reply | Notes |
|-----|-------|-------|
| `hello [client=… proto=…]` | `OK server=phosphoric/X.Y proto=N caps=…` | recommended at session start |
| `regs` | `OK A=XX X=XX Y=XX SP=XX P=XX PC=XXXX cycles=N` | snapshot |
| `set <reg> <val>` | `OK` | reg = A/X/Y/SP/P/PC |
| `read <addr> <len> [bank]` | `OK XX XX XX ...` | bulk hex bytes, len ≤ 4096 (HEX!) ; optional bank selector |
| `bread <addr> <len>` | `OK bread len=N\n<N raw bytes>\n` | length-prefixed binary, len ≤ 65536 |
| `write <addr> <b0> <b1> ...` | `OK count=N` | at least one byte |
| `peek <subsystem>` | `OK k=v k=v ...` | sub = `via`/`psg`/`disk`/`fdc`/`acia`/`serial`/`tape`/`cassette`/`loci`/`video`/`ula`/`kbd`/`keyboard`/`joy`/`joystick`/`printer`/`mcp40` |
| `break <addr> [if <expr>]` | `OK id=N addr=XXXX` | PC breakpoint ; optional conditional expression |
| `unbreak <id>` | `OK` | removes by index |
| `break-list` | `OK id=N:addr=XXXX [id=… …]` | |
| `step` | `OK` then `EVT stopped reason=step` | single instruction |
| `next` | `OK` then `EVT stopped reason=…` | step-over JSR |
| `step-out` | `OK ret=XXXX` then `EVT stopped reason=break` | reads return addr from stack |
| `continue` | `OK` then `EVT stopped reason=break` on break hit | |
| `pause` | `OK pc=… cycles=…` then `EVT stopped reason=user` | works both when stopped AND while running |
| `reset` | `OK pc=…` | warm reset |
| `quit` | `OK` then process exits | |
| `watch <addr> [mode]` | `OK id=N addr=XXXX` | watchpoint ; mode = `w` write (défaut), `r` read, `a` access, `c` change |
| `unwatch <id>` | `OK` | |
| `watch-list` | `OK id=N:addr=XXXX [id=… …]` | |
| `raster <line>` | `OK id=N line=L` | break when PAL line reached (0..311) |
| `unraster <id>` | `OK` | |
| `load-tap <path>` | `OK size=N` | runtime tape mount |
| `load-rom <path>` | `OK size=N pc=XXXX` | replaces BASIC ROM, warm reset |
| `load-sym <path>` | `OK count=N total=M` | merges into the existing symbol table |
| `disasm <addr> <n>` | `OK addr=XXXX bytes=K disasm="…" [label=NAME]` (N lines) | server-side disassembly cross-check |

## Commandes additionnelles (post-35b)

Ces commandes sont implémentées dans `src/control.c` (dispatch `control_dispatch`)
et exposées aussi via l'API HTTP. Les réponses suivent la convention `OK …`/`ERR …`.

| CMD | Usage | Rôle |
|-----|-------|------|
| `keys <texte>` | reste brut de la ligne (espaces préservés) | injecte des frappes clavier |
| `hunt` puis `hunt <op> [val]` | op ∈ `eq,same,changed,up,down,list,clear` | recherche de valeur en mémoire (snapshots successifs) |
| `watch-region <start> <end> [rwx]` | bornes hex, flags sous-ensemble de `rwx` | surveille une plage mémoire |
| `watch-region-clear` | — | efface toutes les régions surveillées |
| `watch-region-list` | — | liste les régions surveillées |
| `save-mem <file> <addr> <len>` | | écrit une zone mémoire dans un fichier |
| `load-mem <file> <addr>` | | charge un fichier en mémoire à `addr` |
| `state-save <file>` | | sauvegarde l'état (`.ost`) |
| `state-load <file>` | | restaure un état (`.ost`) |
| `load-disk <drive A-D> <path>` | nécessite `--disk-rom` | insère une disquette à chaud |
| `eject-disk <drive A-D>` | | éjecte la disquette d'un lecteur |
| `eject-tape` | — | éjecte la cassette |
| `loci-button [long]` | défaut = appui court | déclenche le bouton Action LOCI (`--loci`) |
| `sym-group <N> <on\|off>` | | active/désactive un groupe de symboles |
| `stuck-bits <s0> [s1]` | masques hex | injecte des bits RAM bloqués |
| `trace <sub> …` | sub = `start`/`stop`/`save` ; spec `start` : `now\|pc:HEX stop:cycle:N\|brk\|write:HEX\|read:HEX ring:N sym` | pilote la trace CPU |

## Async commands while running

Once a `continue` or `step` has been acknowledged, the CPU is running.
The stdin parser keeps polling once per frame (~20 ms latency). During
this window only two commands are accepted :

- `pause` → emu replies `OK pc=… cycles=…` then emits
  `EVT stopped pc=… cycles=… reason=user`. The REPL is now interactive
  again.
- `quit` → emu replies `OK` and exits.

Anything else returns `ERR busy: emulator running, only pause/quit
allowed (received <cmd>)`. The IDE should not queue : send `pause`,
wait for `EVT stopped`, then send the synchronous command.

## Events

| EVT | Fields | Trigger |
|-----|--------|---------|
| `ready` | `pc cycles version` | once at start-up |
| `stopped` | `pc cycles reason` | every transition CPU → REPL |
| `halt` | `pc cycles reason` | terminal (sprint 35b) — process is about to exit |

`stopped` reasons : `break` (PC breakpoint hit), `temp` (step-out or
next landed on the temporary breakpoint), `step` (single step
terminated), `user` (async `pause` while running), `watch` (watchpoint
write hit), `raster` (raster-line breakpoint hit).

`halt` reasons : `jam` (CPU halted, KIL opcode or similar), `cycle_limit`
(`--cycles N` reached). When the IDE sends `quit`, no `halt` event is
emitted — the `OK` ack to `quit` is the only signal.

## Example session

```
EVT ready pc=F88F cycles=0 version=1.16.72-alpha
EVT stopped pc=F88F cycles=0 reason=break
> regs
OK A=00 X=00 Y=00 SP=FD P=24 PC=F88F cycles=0
> read $F88F 4
OK A2 FF 9A 58
> break $F893
OK id=0 addr=F893
> continue
OK
EVT stopped pc=F893 cycles=6 reason=break
> quit
OK
```

## Future sprints

- `bread <addr> <len>` (framing binaire pour les memory inspectors ≥ 32 KB) est
  **livré** (cf. tableau des commandes). Restent en backlog : strict framing avec
  numéros de ligne et séquences d'ACK, timeouts serveur, référence client
  exhaustive en Python.

## Error handling

Unknown commands or malformed arguments return `ERR <message>`. The
client should treat any non-`OK`/`EVT` line on stdout as a protocol
violation (logs are on stderr — never on stdout).

## Stability

Protocol shape may evolve until the OricForge integration is shipped to
production. Tag the consumed version in your IDE setup so a future change
in command shape doesn't surprise you.
