# Command REST API — Phosphoric

> Status: **initiative COMPLETE — Epics 1-4 delivered** (Sprints 92-95,
> v1.54.0-alpha). The API fully drives Phosphoric over HTTP/JSON
> (`make HTTPAPI=1`, option `--http-api`).
>
> Quick start:
> ```bash
> make HTTPAPI=1                       # (add SDL2=1 for the GUI)
> ./oric1-emu -r roms/basic11b.rom --http-api=8888 --http-api-root ./disks
> curl -s localhost:8888/regs
> curl -s -X POST --data 'path=game.tap' localhost:8888/tape
> curl -s -X POST localhost:8888/reset
> # Type (and run) a BASIC line remotely — \n = RETURN:
> curl -s -X POST --data-urlencode 'text=PRINT 2+2\n' localhost:8888/keys
> ```

## 1. Vision

Expose the control of Phosphoric (media, CPU/memory state, execution) over
HTTP/JSON on a dedicated port, for remote scripting, browser dashboards
and e2e tests — **without duplicating** the logic of the existing
`--control` protocol.

## 2. Context: what already exists

Phosphoric already has three control surfaces, but none of them is a
"classic" REST API:

| Surface       | Transport            | Use                                     |
|---------------|----------------------|-----------------------------------------|
| `--control`   | stdin/stdout (text)  | 30 commands (OricForge IDE)             |
| Cast server   | HTTP `:8080`         | MJPEG streaming + audio + snapshot      |
| GDB stub      | TCP `:1234`          | Debugging (RSP protocol)                |

The `--control` protocol (`src/control.c`) already contains reusable business
*handlers* (`cmd_load_tap`, `cmd_reset`, `cmd_load_disk`,
`cmd_peek`…). The REST API **does not rewrite** this logic: it adds an
HTTP transport plugged into the same dispatch.

## 3. Guiding architectural constraint

The emulator is **single-threaded**: the main loop executes
`CYCLES_PER_FRAME` (19968) cycles per frame in `src/main.c`. Any state
mutation (`savestate_load`, `microdisc_set_disk`, `cpu_reset`) must run
**at frame boundaries**, never from an HTTP thread in the middle
of an instruction. This is technical risk no. 1; it is isolated in Epic 2
(thread-safe command queue).

## 4. Backlog

### EPIC 1 — Decouple the dispatch from its transport *(Sprint 92, in progress)*

A reusable foundation even if the REST API were later abandoned.

- **US 1.1** — `control_sink_t`: output abstraction (`ok`/`err`/`raw`),
  two implementations: `stream` (stdout, current behaviour byte-identical)
  and `buffer` (in-memory accumulation for HTTP). The `cmd_*` handlers write
  to a sink instead of `stdout`.
- **US 1.2** — Extract `control_dispatch(emu, sink, line)` from the big `if/else`
  of `control_repl()`. Returns `control_result_t`
  (`CONTINUE`/`RESUME`/`QUIT`). `control_repl` becomes a thin loop.
- **DoD**: `make test-control` stays green down to the byte + a new unit
  test of the dispatch through a buffer sink.

### EPIC 2 — Thread-safe command queue *(Sprint 93)*

- **US 2.1** — Single-producer (HTTP thread) / single-consumer (emulator
  loop) queue, protected by mutex + condvar, drained once per frame in
  `main.c` (next to `control_poll_pause`).
- **DoD**: `test-httpapi` — 100 concurrent commands, zero corruption,
  clean valgrind.

### EPIC 3 — Minimal HTTP server *(Sprint 94)*

- **US 3.1** — HTTP server reusing the socket/`select` pattern of
  `src/network/cast_server.c` (no external dependency). Option
  `--http-api=PORT`, binding to **`127.0.0.1` by default**, network exposure
  is an explicit opt-in (`--http-api-bind`). Makefile flag `HTTPAPI ?= 0` modelled
  on `CAST=1`.
- **US 3.2** — Endpoints (mapped onto the existing handlers):

  | Method   | Route                              | Handler                       |
  |----------|------------------------------------|-------------------------------|
  | `GET`    | `/hello`                           | `cmd_hello`                   |
  | `GET`    | `/regs`                            | `cmd_regs`                    |
  | `GET`    | `/mem?addr=&len=[&bank=cpu\|ram\|rom\|overlay]` | `cmd_read`       |
  | `POST`   | `/mem`                             | `cmd_write`                   |
  | `POST`   | `/reset`                           | `cmd_reset`                   |
  | `POST`   | `/nmi`                             | `nmi` (button under the Oric) |
  | `POST`   | `/tape` `{path}` / `DELETE /tape`  | `cmd_load_tap` / `cmd_eject_tape` |
  | `POST`   | `/disk/{A-D}` / `DELETE`           | `cmd_load_disk` / `cmd_eject_disk` |
  | `GET`    | `/peek/{via\|psg\|disk\|acia\|tape\|loci\|video\|kbd\|joy\|printer}` | `cmd_peek` |
  | `POST`   | `/exec/{step\|next\|step-out\|continue\|pause}` | execution dispatch |
  | `POST`   | `/keys` `{text}`                   | `cmd_keys` (Epic 4)           |

  Responses in **JSON** (`{"ok":true,"reply":…}` / `{"ok":false,"error":…}`),
  CORS header. *(Delivered: the mapping is done to a `--control` line passed
  to `control_queue_submit`, not a direct call to the handler.)*
- **US 3.3** — Security: file paths (`/tape`, `/disk`) restricted to
  an allowed directory (`--http-api-root`), absolute paths and
  `..` are refused.

### EPIC 4 — Keyboard injection & documentation *(Sprint 95)* — DELIVERED

- **US 4.1** ✅ — `POST /keys {text}` → `keys` command → dynamic injection
  buffer (`kbd_inject_*`), consumed key by key by the loop
  (`feed_kbd_inject`, press/hold/release ~5 frames/key) on the emulator
  thread. Escapes: `\n`/`\r` → RETURN, `\t`, `\e`, `\\`.
- **US 4.2** ✅ — README + `docs/http-api.md` + `curl` examples; CHANGELOG /
  VERSION_TRACKING / CIRRUS_OS / ROADMAP up to date.

### EPIC 5 — Debug parity (`--control` bridge + 7 gaps) *(Sprint 97)* — DELIVERED

Interactive debugging (including the 7 gaps of Sprint 96) can now be driven
remotely. Same principle: **each route maps to a `--control` line.**

| Method   | Route                                   | `--control` command         |
|----------|-----------------------------------------|-----------------------------|
| `GET`    | `/break`                                | `break-list`                |
| `POST`   | `/break` `{addr[,if]}`                  | `break <addr> [if <expr>]`  |
| `DELETE` | `/break/{id}`                           | `unbreak <id>`              |
| `GET`    | `/watch`                                | `watch-list`                |
| `POST`   | `/watch` `{addr[,mode=w\|r\|a\|c]}`     | `watch <addr> [mode]`       |
| `DELETE` | `/watch/{id}`                           | `unwatch <id>`              |
| `POST`   | `/raster` `{line}` / `DELETE /raster/{id}` | `raster` / `unraster`    |
| `GET`    | `/disasm?addr=&n=`                      | `disasm <addr> <n>`         |
| `POST`   | `/set` `{reg,val}` or `{via,val}`       | `set <reg> <val>` / `set via` |
| `POST`   | `/hunt` `{[op[,val]]}`                  | `hunt [op] [val]`           |
| `POST`   | `/save` `{path,addr,len}`               | `save-mem` (sandbox)        |
| `POST`   | `/load` `{path,addr}`                   | `load-mem` (sandbox)        |
| `POST`   | `/state/save` `{path}` / `/state/load`  | `state-save` / `state-load` |
| `POST`   | `/sym` `{path}`                         | `load-sym` (sandbox)        |

- **Watch modes**: `w` write, `r` read, `a` access, `c` change.
- **Conditional breakpoints**: the expression (`A==5 && M[$C000]>10`) is
  URL-encoded by the client and decoded by `get_param` (`&&` → `%26%26`).
- **`hunt`** (cheat finder): `op ∈ {eq <val>, same, changed, up, down, list,
  clear}`; `POST /hunt` without `op` seeds over the whole address space.
- **Files**: `/save` `/load` `/state/*` `/sym` are **sandboxed** in
  `--http-api-root` (`..` and absolute paths rejected → 403).
- **Binary `%` literals** accepted in all numeric parameters.
- Extended `hello` caps: `watch-mode,break-cond,hunt,save-mem,load-mem,
  state-save,state-load,set-via,bin-literal` (additive extension,
  `CONTROL_PROTO_VERSION` unchanged).

Examples:

```bash
curl -s -X POST --data 'addr=C000&mode=r'                localhost:8888/watch
curl -s -X POST --data-urlencode 'addr=0500' \
                --data-urlencode 'if=A==5 && X==3'       localhost:8888/break
curl -s -X POST                                          localhost:8888/hunt        # seed
curl -s -X POST --data 'op=eq&val=7B'                    localhost:8888/hunt        # narrow
curl -s -X POST --data 'path=snap.ost'                   localhost:8888/state/save
curl -s -X POST --data 'via=2&val=FF'                    localhost:8888/set
```

### EPIC 6 / US 2 — Bank-aware memory inspection *(Sprint 98)* — DELIVERED

`GET /mem` accepts a **`bank`** parameter to read a specific memory layer
**under** the ORIC $C000-$FFFF overlay, independently of the current paging (parity
with b2's paging overrides):

| `bank`    | Layer read |
|-----------|-----------|
| `cpu` *(default)* | current CPU view (whatever is paged in) |
| `ram`     | underlying RAM (`upper_ram` = RAM behind the ROM ≥ $C000) |
| `rom`     | BASIC/monitor ROM ($C000-$FFFF) |
| `overlay` | Microdisc overlay ROM ($E000-$FFFF) |

Same parameter on the `--control` side (`read <addr> <len> [bank]`) and in the REPL
(`m addr [len] [bank]`). Cap `mem-bank`.

> **`POST /mem`** expects the body parameters **`addr=`** and **`bytes=`**
> (space-separated hex bytes) — mapped onto the line `write <addr> <bytes>`.
> A root `GET /` returns a help page that auto-lists all routes.

```bash
curl -s "localhost:8888/mem?addr=C000&len=1&bank=rom"   # 1st byte of the BASIC ROM
curl -s "localhost:8888/mem?addr=C000&len=1&bank=ram"   # RAM hidden behind it
```

### EPIC 6 / US 1 — Conditional tracing *(Sprint 99)* — DELIVERED

CPU tracing (`--trace`) becomes **conditional** and **bounded** (parity with
b2's triggers and circular buffer).

| Method   | Route             | Effect |
|----------|-------------------|-------|
| `POST`   | `/trace` `{spec}` | arms the trace (`trace start <spec>`) |
| `GET`    | `/trace`          | status (`active/armed/count/ring`) |
| `POST`   | `/trace/stop`     | stops recording (keeps the ring) |
| `POST`   | `/trace/save` `{path}` | writes the ring to a file (sandbox) |
| `DELETE` | `/trace`          | disarms + frees the ring |

**Spec** (`spec=`, space-separated tokens):
`now` | `pc:HEX` — start; `stop:cycle:N` | `stop:brk` | `stop:write:HEX` |
`stop:read:HEX` — stop; `ring:N` — circular buffer; `sym` — inline symbols.

Same syntax on the `--control` side (`trace start <spec>`, `trace stop|save|status|
off`) and in the REPL. Cap `trace-cond`. The write/read triggers rely on a second
memory hook (`trace_callback2`), independent of the watchpoints.

```bash
# trace 500 instructions around a routine, with symbols, then retrieve the ring
curl -s -X POST --data-urlencode 'spec=pc:E000 stop:brk ring:500 sym' localhost:8888/trace
curl -s -X POST --data 'path=run.log'  localhost:8888/trace/save
```

### EPIC 6 / US 3 — r/w/x memory access map *(Sprint 100)* — DELIVERED

Marks **arbitrary regions** with read/write/execute flags, without the
limit of the 8 fixed watchpoints (parity with b2's per-byte flags).

| Method   | Route             | Effect |
|----------|-------------------|-------|
| `POST`   | `/watch-region` `{start,end[,flags]}` | flags the region (default `rw`) |
| `GET`    | `/watch-region`   | lists the flagged runs |
| `DELETE` | `/watch-region`   | clears the whole map |

`flags` = subset of `rwx`. Read/write trigger through the memory trace
callback (shared with the watchpoints); execute is tested before each
instruction. Same on the `--control` side (`watch-region <start> <end> [flags]`,
`watch-region-list`, `watch-region-clear`) and in the REPL (`wr START END [rwx]`). Cap
`access-map`.

```bash
curl -s -X POST --data 'start=2000&end=2010&flags=rw' localhost:8888/watch-region
curl -s "localhost:8888/watch-region"     # list
```

### EPIC 6 / US 5 — Broader inspection coverage *(Sprint 101)* — DELIVERED

`GET /peek/{sub}` now covers 4 more subsystems (parity with b2's
inspection windows), exposing only real fields:
`video`/`ula`, `kbd`, `joy`, `printer` (MCP-40 state included). Same commands in the
REPL (`video`/`kbd`/`joy`/`printer`) and `--control` (`peek …`).

```bash
curl -s "localhost:8888/peek/video"    # ULA mode, OCULA, framebuffer
curl -s "localhost:8888/peek/kbd"      # 8-column keyboard matrix
```

### EPIC 6 / US 4 — Symbol groups *(Sprint 102)* — DELIVERED

Symbols can be **tagged by group** (0-255) and a group
enabled/disabled — handy for telling apart symbols from different banks
(BASIC ROM vs Microdisc overlay vs LOCI), in the manner of b2's groups.

| Method   | Route                    | Effect |
|----------|--------------------------|-------|
| `POST`   | `/sym` `{path[,group]}`  | loads a symbol file into a group |
| `POST`   | `/sym/group` `{group,enabled}` | enables/disables a group |

`symbol_lookup`/`symbol_resolve` ignore symbols of disabled groups.
Same commands in `--control` (`load-sym FILE [group]`, `sym-group N on|off`) and in the
REPL (`sym load FILE [g]`, `sym group N on|off`, `sym groups`).

```bash
curl -s -X POST --data 'path=basic.sym&group=1'   localhost:8888/sym
curl -s -X POST --data 'path=microdisc.sym&group=2' localhost:8888/sym
curl -s -X POST --data 'group=2&enabled=off'      localhost:8888/sym/group
```

### EPIC 6 / US 6a — RAM stuck-bit injection *(Sprint 103)* — DELIVERED

RAM fault injection through **stuck bits** (parity with b2's "RAM errors"):
each data bit can be forced to 0 or to 1 on RAM reads
($0000-$BFFF).

| Method   | Route                      | Effect |
|----------|----------------------------|-------|
| `POST`   | `/stuck-bits` `{zero,one}` | hex masks: `zero`=bits→0, `one`=bits→1 |
| `GET`    | `/stuck-bits`              | current masks |

Formula: `val = (v & ~zero) | one`. `zero=00&one=00` disables it. Same
commands in `--control` (`stuck-bits <s0> [s1]`) and the REPL (`stuck S0 [S1]`).

```bash
curl -s -X POST --data 'zero=00&one=01' localhost:8888/stuck-bits   # bit0 stuck at 1
curl -s -X POST --data 'zero=00&one=00' localhost:8888/stuck-bits   # off
```

> The **visual timing overlays** part of US 6 (TV beam, raster lines)
> remains in the backlog: it is GUI/SDL rendering, unverifiable in headless mode.

## 5. Estimate

~600-900 LOC in total (sink+dispatch ~200, queue ~120, HTTP server+routing
~300, path security ~80, tests ~200). Order of magnitude comparable to the
cast module.
