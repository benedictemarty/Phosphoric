# WebAssembly build (browser)

Phosphoric compiles to **WebAssembly** via Emscripten: the complete emulator
(6502 CPU, memory, VIA, PSG, ULA, keyboard, cassette, disk) runs in a
browser tab, rendered on a `<canvas>`, audio via Web Audio, keyboard via the DOM — by
reusing the existing SDL2 path (Emscripten's SDL port).

## Prerequisites

An active [Emscripten SDK](https://emscripten.org) (`emcc` in the `PATH`):

```bash
git clone https://github.com/emscripten-core/emsdk
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ./emsdk_env.sh
```

## Build and run

```bash
make wasm
(cd web && python3 -m http.server 8000)
# open http://localhost:8000/phosphoric.html
```

`make wasm` produces in `web/`: `phosphoric.html` (page + canvas),
`phosphoric.js`, `phosphoric.wasm`, and `phosphoric.data` (the ROMs from `roms/`
preloaded into the virtual file system). The page starts the Atmos
(`-r /roms/basic11b.rom`); click the screen to give it keyboard focus and enable audio.

## Deployment: required assets + CSP

The page loads its logic (definition of `Module`, UI, keyboard, drag-and-drop)
from **`web/shell.js`** — an external file that must be **deployed alongside**
`phosphoric.html`/`.js`/`.wasm`/`.data`. `shell.js` is version-controlled (source) and
referenced by `phosphoric.html` via `<script src="shell.js">`. The same goes for
**`web/picowifi_js.js`** (relay-less modem transport), loaded just before it.

This externalisation makes the bundle **compatible with a strict Content-Security-Policy**.
A host serving the page under `script-src 'self' 'wasm-unsafe-eval'` would block an
inline `<script>` (`script-src-elem`) → `Module` never defined → `Module.canvas`
`undefined` → fatal error "can't access property … canvas is undefined" at the WebGL
`createContext`. By externalising all the JS (and replacing the canvas's inline
`oncontextmenu` attribute with a DOM listener), `phosphoric.html` no longer has
**any inline script/handler**: it runs under a strict CSP as well as under a
permissive policy (GitHub Pages). `phosphoric.js` is already loaded externally
by Emscripten (`<script async src=phosphoric.js>`), and WASM compilation requires
`'wasm-unsafe-eval'` in `script-src`.

### Minimal required CSP

```
Content-Security-Policy: script-src 'self' 'wasm-unsafe-eval'
```

- **`'self'`** — allows `shell.js`, `picowifi_js.js` and `phosphoric.js` (external).
- picowifi modem: if the policy restricts `connect-src`, add the relay
  (`ws://127.0.0.1:8766`) or, without a relay, the sites targeted by `fetch()`.
- **`'wasm-unsafe-eval'`** — **mandatory**: `phosphoric.js` compiles the module via
  `WebAssembly.instantiateStreaming`/`instantiate`, which are blocked under `script-src 'self'`
  alone. It is the WASM sub-token (≠ `'unsafe-eval'`, which is far broader) — safe.

⚠️ **Do not remove `'wasm-unsafe-eval'`**: under a bare `script-src 'self'`,
WebAssembly compilation is refused and **the emulator does not start** (no
`Initializing Phosphoric …` log). Since the externalisation fix, neither `'unsafe-inline'`
nor a hash/nonce is needed — only the WASM token is.

> A `script-src-elem` block whose source is `sandbox eval code` (and not
> `phosphoric.html`) comes from a **browser extension** (content script), not
> from Phosphoric: the page injects no inline script nor `eval`. No effect on
> the emulator.

## Interface (web page)

The page (`web/shell.html`) presents a **vertical icon rail on the left**
(JOric-style) and the ORIC keyboard as an overlay:

- **MODEL** — switches between ORIC-1 / Atmos machines (badge `1`/`A`, cold restart with
  the chosen ROM).
- **LOAD** + **drag-and-drop** of a `.tap`/`.dsk` onto the screen: the file is
  inserted and the machine restarts on it (cassette `-t …-f`, or disk
  `--disk-rom microdis.rom -d …`). **EJECT** button to remove it.
- **Deep links (URL parameters)** — `?rom=oric1|atmos` chooses the machine and
  `?media=<file>` loads a media file on the first page load. The **type** is
  inferred from the extension: `.tap` → cassette (`-t … -f`), `.dsk` → disk. For
  a `.dsk`, the **Microdisc controller is enabled at boot** (`--disk-rom
  microdis.rom`) as soon as the URL targets a disk, then the image is hot-inserted
  — without that, the boot would happen without an FDC and the insertion would fail. The
  target file must be served as **binary** (a server returning a fallback HTML
  page with status 200 makes the insertion fail: "not a valid TAP/DSK").
- **LOCI** (or `?loci=1` in the URL) — plugs in the **LOCI cartridge** (HLE
  emulation `--loci`, cold restart): the machine boots into the **LOCI menu**
  (`roms/loci/locirom`). Its **"internal flash" storage** is `/loci`, an
  **IDBFS file system persisted in the browser's IndexedDB** (it
  survives reloads), seeded on first launch with `basic11b.rom`,
  `basic10.rom`, `microdis.rom` and `locirom`. In LOCI mode, **LOAD / drag-and-drop
  copies the file into this flash** (`.dsk`, `.tap`, `.rom`, …) instead of
  inserting it: you then pick it in the menu (**Space** opens the picker
  for the current field, **ESC** = boot), as on the real cartridge. **F8** =
  Action button (back to the menu). `?loci=0` or clicking LOCI again
  returns to normal mode. **LOCI + cassette**: `?loci=1&media=prog.tap` boots
  BASIC directly on the cassette with the cartridge present (picowifi ACIA at
  `$0380`, persistent flash) — equivalent to `-t prog.tap -f --loci --loci-flash …`
  natively (the ProphetOric/OricTel case, which only probe the ACIA at `$0380`).
  Not available on the web: co-simulation of the real
  firmware (`--loci-emu`), host USB keys and SD image (`--loci-sdimg`).
- **MODEM** (or `?modem=1`) — **picowifi** modem (emulated PicoWiFiModemUSB firmware,
  `--serial picowifi:Web:web`, simulated WiFi "Web") on the 6551 ACIA:
  `$0380` with LOCI, `$031C` otherwise. With LOCI, `--serial-buffer 32` is added:
  it is the 32-byte RX ring of the LOCI firmware (`acia.c`,
  `ACIA_RX_BUFFER_SIZE`), without which the `ATZ` echo overflows (ProphetOric: « pas
  de modem » — "no modem"). Without LOCI, bare 6551: no FIFO. Since the browser cannot open TCP, each
  connection (ATDT, ATGET, ATRD/ATRT, ATDISKRD…) goes through a **WebSocket
  relay** to be started on the machine: `python3 tools/picowifi_ws_relay.py`
  (default `ws://127.0.0.1:8766/`, other relay: `?relay=ws://host:port/`,
  remembered). The relay also terminates **TLS** (secure `ATDT`, `ATGET
  https://`) with system certificate verification. With LOCI, the modem's NVRAM
  (`AT&W`) is persisted in the flash (`/loci/picowifi.cfg`).
  **Without a relay** (`?relay=none`, in the manner of Phosphoneo's `neomodem.js`):
  `web/picowifi_js.js` provides virtual sockets — the modem's HTTP requests
  (`ATGET http(s)://`, `ATDISKRD/WR`) are replayed with `fetch()`
  (sites allowing CORS, or through a same-origin proxy `?httpproxy=/proxy?url=`)
  and the response is rendered as a reconstructed HTTP/1.1; `ATRD`/`ATRT` read
  the browser's clock. The request headers are forwarded (e.g.
  `ResponseFormat`), except those that `fetch()` refuses or sets itself (Host, Connection,
  Content-Length, User-Agent…). **`?httpsame=h1,h2`**: a request to `h1`/`h2`
  (whatever the port and scheme, e.g. `ATD-prophet.3617.fr:8998` then raw
  HTTP) goes to **the page's origin** + path — for a page served by the
  same server under a `connect-src 'self'` CSP, without mixed content. Telnet/BBS and
  raw TCP require the relay: without it, the connection is dropped at the first
  non-HTTP byte (NO CARRIER).
  Example: `phosphoric.html?loci=1&modem=1&relay=none&httpsame=prophet.3617.fr&media=prophetoric.tap`.
- **RESET** — cold reboot keeping the ROM and media.
- **KEYS** — shows/hides the virtual keyboard.
- **FULL** — full screen (the canvas is centred and scaled to the screen height,
  240/224 ratio preserved).
- **CRT** — scanlines + vignette filter over the screen (state remembered).
- **SAVE / REST** — saves the state to an `.ost` (downloaded) / restores an
  `.ost` (applied **live**, without reboot).
- **TAPE / DISK LEDs** (below the screen) — light up during a CLOAD (cassette)
  or a disk access (WD1793 BUSY).
- **Faithful ORIC virtual keyboard**, as a **semi-transparent overlay over
  the screen**: real layout (ESC, CTRL, FUNCT, 2× SHIFT, RETURN, DEL, SPACE,
  arrows ↑←↓→) with **sticky CTRL / FUNCT / SHIFT** modifiers (the physical Shift/
  Ctrl/Alt keys also drive these modifiers). The shifted symbols
  shown as superscripts are **derived from the real matrix** (`char_map`):
  `2`→`@`, `6`→`^`, `-`→`_`, `;`→`:`, `[`→`{`, `\`→`|`, etc. On the **ORIC-1**, the
  **FUNCT key (Atmos-only) is absent**.

> **CTRL+T and other chords:** the browser reserves certain shortcuts
> (CTRL+T = new tab) at the OS level — they never reach the
> canvas. Use the **CTRL key of the virtual keyboard**: it writes the
> ORIC matrix through a direct C call (`web_key`), so the browser does not
> intercept it. Same for FUNCT.

> Serve the files over **HTTP** (not `file://`): the browser refuses to
> load a `.wasm` from the local file system.

## Technical details

- **Main loop**: in the browser, the C `while` loop must yield
  to the event loop every frame. This is done via
  **Asyncify** (`-sASYNCIFY`) + `emscripten_sleep()` in the frame limiter
  (`src/main.c`, guarded by `__EMSCRIPTEN__`) — which also paces at ~50 Hz.
- **Stack**: `emulator_t` is large (framebuffer + memory) and lives on the
  stack of `main()`; the build forces `-sSTACK_SIZE=8MB` (the default 64 KB
  would overflow).
- **Network**: features that require native sockets/threads
  (TCP/PTY/COM serial backends, GDB stub, Cast server) link as no-ops in
  the browser — except the picowifi modem, whose `serial_picowifi.c` routes
  connections (`pw_read`/`pw_write`/`pw_close`/`pw_wait`) to JS WebSockets
  (`EM_JS`, fd ≥ `0x100000`) opened to the relay
  `?host=H&port=P&tls=0|1`; waits yield to the browser via
  `emscripten_sleep` (Asyncify). Relay: `tools/picowifi_ws_relay.py`
  (Python stdlib, 127.0.0.1 by default, `--allow HOTE[:PORT]`, `--origin URL`) — the machine core, video, audio,
  keyboard, cassette and disk all work. The LOCI cartridge (HLE)
  works; only its RP2040 co-simulation (`libemul`, native) is replaced
  by a stub (`loci_emu_stub.c`).
- **Persistent LOCI flash**: linked with `-lidbfs.js`; `web/shell.js` mounts IDBFS on
  `/loci` in `preRun` (a run dependency for the duration of `syncfs(true)`),
  seeds the ROMs in `onRuntimeInitialized` (the preloaded `/roms` files
  only exist at that point) then syncs back to IndexedDB every 5 s and
  on `pagehide`.
- **`web_peek(addr)`**: side-effect-free memory read (`memory_peek`)
  exported for the UI and the e2e tests (reading the `$BB80` text screen).
- **Browser e2e test**: `make test-web-loci` builds the web version, serves it
  locally and drives it in headless Chrome via Playwright (LOCI menu at boot,
  seeded flash, import + persistence across reload, file listed by the
  picker, and mount + boot of a real `.dsk` if `disks/3dfongus.dsk` exists).
  SKIP if emsdk, node, Playwright or Chrome are missing; not in `make tests`.
- **Modem e2e test**: `make test-web-picowifi` (9/9), BASIC programs
  tokenised into auto-run `.tap` files (`bas2tap`) and loaded with `?media=`:
  1. relay: BASIC → ACIA `$031C` → picowifi WASM → WebSocket → relay →
     local TCP server, and back;
  2. without relay: `ATGET` replayed by `fetch()` (CORS server), local `ATRD`;
  3. LOCI + cassette + `?httpsame=`: ACIA `$0380`, `ATD-` then raw HTTP
     rewritten to the origin, `ResponseFormat` received by the server;
  4. LOCI + default modem: 32-byte RX ring; without LOCI, none.
- **Test arguments**: a JSON array in `sessionStorage`
  `phos_extra_args` is appended to the command line (e.g. `--type-keys`).

## Fidelity — verified

The WASM output is **byte-identical to the native build** for identical
inputs: a headless Atmos boot (`-n -c N --screenshot`) compiled to WASM and
run under Node.js produces the **exact same PPM capture** as the native binary
(tested at 2M and 5M cycles). The core's bus-cycle-level determinism is preserved
through WebAssembly compilation.

**Browser** rendering has also been validated: the page loaded in headless Chromium
(`--virtual-time-budget`) displays the canvas with the correct Atmos boot screen
("ORIC EXTENDED BASIC V1.1 / © 1983 TANGERINE / 37631 BYTES FREE /
Ready" + CAPS indicator) — the SDL2 → WebGL/canvas path works end
to end. **Keyboard input** has also been validated: typing `PRINT 6*7` + RETURN
through the `web_key` bridge (virtual keyboard) displays `42` — ROM boot → keyboard
injection → BASIC execution → rendering, entirely in the browser.
