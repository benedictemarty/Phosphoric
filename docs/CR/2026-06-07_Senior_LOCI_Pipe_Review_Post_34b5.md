# Senior review — LOCI/Sedoric pipe after E2E validation (sprints 34b0→34b5)

**Date**: 2026-06-07
**Scope**: `src/io/loci.c` (2276 LOC), `include/io/loci.h`, LOCI↔FDC callbacks in `src/io/microdisc.c` + `src/storage/disk.c`, wiring in `src/main.c`, `tests/integration/test_loci_sedoric_e2e.sh`.
**Upstream reference**: `2026-06-07_Senior_LOCI_Sedoric_Boot_34az_Closeout.md` (dynamic ROMDIS fix).

---

## 1. TL;DR

The pipe is **functionally solid**: 5 E2E scenarios byte-identical with the native Microdisc, clean encapsulation of the MIA/DSK/TAP trio, shared `fdc_t` bridge. No **P0** found.

**Moderate residual** risks (P1): POSIX `op_readdir` bug (wrong path for `stat`), `strstr(p, "..")` too lax, DSK writes not persisted in raw mode, and `static` `loci_overlay_buf` (messy lifecycle management). The `loci.c` file is starting to reach the natural limit of a single TU and would deserve a split (file / dir / dsk / tap / boot).

**Minor residual** risk (P2): unit-level coverage of error tests and FDC timing edge cases is almost absent; E2E only covers the happy paths.

---

## 2. Strengths

1. **Native ↔ LOCI separation well kept.** The cycle-timed `fdc_t` (`storage/disk.c`) is shared through `dsk_set_disk`/`fdc_set_disk`; neither `disk.c` nor `microdisc.c` references LOCI. The bridge is one-way (LOCI → FDC through DRQ/INTRQ callbacks symmetrical to the Microdisc callbacks — compare `loci.c:1776-1803` and `microdisc.c:23-48`). This is what made the 34az fix possible in 4 lines.

2. **Spin-window ABI contract documented and applied uniformly.** `api_install_blocked_stub` / `api_install_released_stub` / `api_return_*` (`loci.c:410-468`) encapsulate the self-modifying semantics of the sodiumlb firmware. The comment at `loci.c:368-405` explains the BUSY semantics overloaded onto the BVC operand — rare and valuable. Every path of `dispatch_op` exits through `api_return_*` or `api_install_released_stub`, which eliminates the whole class of "JSR $03B0 on zero" hangs.

3. **Correct POSIX/SDIMG symmetry through `fd_kind[]`.** The "fds[i] non-NULL + fd_kind[i] discriminator" pattern (`loci.c:184-197`, `868-869`, `944-945`) avoids the type-punning of the early sprints. Mixed cleanup works without leaks.

4. **34az dynamic ROMDIS fix applied cleanly.** `loci.c:1929-1948` mirrors exactly the logic of `microdisc.c:115-137` (active-low bits, IRQ applied before overlay). The comment references the original bug, which helps future readers.

5. **Differential E2E tests.** `tests/integration/test_loci_sedoric_e2e.sh` is the right approach: compare native vs LOCI RAM dumps over stable regions ($BB80 screen, $0500 BASIC). Graceful skipping of missing assets allows CI without the proprietary ROMs.

---

## 3. Residual risks

### P1 — Real bug: POSIX `op_readdir` stats with the wrong base path

**File**: `loci.c:1549-1551`

```c
const char* base = loci->flash_root[0] ? loci->flash_root : ".";
snprintf(fullpath, sizeof(fullpath), "%s/%s", base, de->d_name);
if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) { ... }
```

**Scenario**: `opendir("sub/")` opened a subdirectory (the `DIR*` points to `sub/`), but `op_readdir` rebuilds the stat path as `flash_root + de->d_name` — not `sub/de->d_name`. Consequences: (a) all files in a subdirectory are classified as non-DIR even when they are DIRs; (b) the reported `d_size` = 0 for entries that do not exist at root level. With Sedoric only at the root, **not observed**; it will become visible as soon as a UI browses `OPENDIR /sub`.

**Mitigation**: store the resolved path in a parallel array `loci.dirs_path[LOCI_DIR_MAX][256]` in `op_opendir`, then use it in `op_readdir`. ~15 LOC.

### P1 — Path traversal detected with `strstr(p, "..")`

**File**: `loci.c:713`

```c
if (strstr(p, "..")) return false;
```

**Scenario**: rejects legitimate file names containing `..` (e.g. `my..backup.dsk`, `a..b.tap`). At the same time, it does not detect URL-encoded variants or absolute paths rebuilt after stripping the volume prefix. Not a security hole (the attacker already controls the SD `.img`), but a **functional** risk: some Sedoric assets have multiple dots in their name.

**Mitigation**: tokenise on `/` and reject the exact component `..` (and the empty string). 6 LOC.

### P1 — DSK writes not persisted in raw mode / partially in MFM mode

**File**: `loci.c:1825-1873` + `dsk_close` at `1875-1889`.

`dsk_open` loads the image into RAM (`loci->dsk_image[drive]`), and `fdc_set_disk` routes writes there. `dsk_fp` is opened `r+b` but **never written back**: neither in `dsk_close` nor in `loci_cleanup`. Consequence: Sedoric SAVEs on a disk attached via `--loci-sdimg` go through the SDIMG path (explicit sync at `main.c:1269`), so the 34b2 E2E scenario is OK; but a mount via `--loci-flash` loses the writes on close.

**Triggering scenario**: `./oric1-emu --loci --loci-flash /tmp/myflash …`, mount a `.dsk`, SAVE from Sedoric, quit. The host `.dsk` is unchanged.

**Mitigation**: in `dsk_close` (and `loci_cleanup`), if `dsk_image[i]` is non-NULL and `dsk_fp[i]` is open for writing, `fseek 0` + `fwrite(image, size)`. ~10 LOC. Alternative: a dirty flag updated by FDC writes.

### P2 — `static` `loci_overlay_buf` in `loci_rom_swap_cb`

**File**: `src/main.c:364-367`

```c
static uint8_t* loci_overlay_buf = NULL;
if (loci_overlay_buf) { free(loci_overlay_buf); loci_overlay_buf = NULL; }
loci_overlay_buf = (uint8_t*)malloc((size_t)sz);
```

The comment acknowledges the debt ("acceptable leak at shutdown"). The `static` storage also prevents testing in isolation (residual state between tests if main were ever called in a loop). No immediate production impact.

**Mitigation**: move the buffer into `emulator_t` (field `loci_overlay_buf` + free in `emulator_cleanup`).

### P2 — Asymmetric clamp in `op_tap_seek`

**File**: `loci.c:1698-1699`

```c
if (loci->tap_size > 0 && pos >= loci->tap_size) {
    pos = loci->tap_size - 1;
}
```

Returns `pos = size-1` instead of `size` (the firmware allows SEEK to EOF). Consequence: a `TAP_SEEK(0xFFFFFFFF)` followed by `TAP_TELL` reports `size-1`, off by one vs the firmware. Probably no observed ROM impact.

### P2 — No protection against a corrupted `xstack_ptr`

**File**: `loci.c:2222-2227` (`API_STACK` write)

A push decrements `xstack_ptr` down to 0, after which it is silently ignored. No errno is raised. An `op_*` that assumes a certain number of bytes can then read zeros (the `op_open` case with a partial path). The firmware behaves the same, so it is not a bug, but it is a debugging trap: symptom = ENOENT while the 6502 "believed" it had pushed a path. Recommendation: `log_debug` when `xstack_ptr == 0` on push.

---

## 4. Technical debt

- **`loci.c` = 2276 LOC in a single TU.** Clearly identifiable sections (`/* ─── ... ─── */`) — an invitation to split. Suggestion: `loci_core.c` (init/lifecycle/MIA register file + dispatch + xstack), `loci_fs.c` (open/close/read/write/lseek + opendir, POSIX + SDIMG dispatch), `loci_bus.c` (DSK + TAP register windows + FDC callbacks), `loci_boot.c` (MIA_BOOT + ROM resolution). ~1 sprint.

- **POSIX/SDIMG duplication in `op_write_xstack` (`loci.c:1084-1137`).** The pop+validate logic (35 LOC) is copied. An indirection through a mini-vtable `loci_fs_ops_t {open,close,read_n,write_n,lseek,unlink,rename}` is mentioned in `loci.h:232` (TODO vtable). Also true in: `op_unlink`, `op_rename`, `op_mkdir`, `op_opendir`, `op_closedir`, `op_readdir` (all have an sdimg preamble + the POSIX path). ~150 LOC could be compressed.

- **`dispatch_op` (`loci.c:2094-2144`): 34 switch entries.** Fine for now, but a table `static const struct { uint8_t op; void (*fn)(loci_t*); } ops[]` would make adding a new op (and tracing by name) more uniform. Limited benefit.

- **`op_mount` function (`loci.c:1312-1366`): 55 LOC with 3 paths (SDIMG/POSIX, TAP/DSK), path resolution + auto-open + callback + persistence.** Deserves to be split into helpers (`mount_resolve_path`, `mount_attach_backend`).

- **`derive_basic_rom_path` (`loci.c:1968-1973`) hardcodes `basic11b.rom` / `basic10.rom`.** If the user wants to boot a custom ROM through MIA_BOOT without mounting `LOCI_MNT_ROM`, it fails. To be documented or exposed through a setter.

- **Unresolved TODO**: `loci.h:232` `TODO(vtable)` — currently 1 (POSIX/SDIMG/inused), not an immediate problem.

- **UTF-8 conventions / French comments mixed with English.** Consistent in the recent sprints, but the older sections (up to ~1200) are 100 % English. Not critical.

- **`op_uname` (`loci.c:1606-1611`) hardcodes `release 1.16.27`.** Should be an `EMU_VERSION` macro to stay consistent with `include/emulator.h`. Risk: the 6502 sees an outdated version if a LOCI ROM displays it.

- **Magic numbers `0x80` / `0x7F` for DRQ/INTRQ** everywhere. The `#define LOCI_DSK_STAT_*` exist; use them in `loci_dsk_read` / `loci_fdc_*_intrq`.

---

## 5. Test coverage

### Well covered
- Happy path of the Sedoric V4 boot (E2E 34b0).
- DIR catalogue (E2E 34b1).
- BASIC SAVE/LOAD/RUN round trip (E2E 34b2, 34b2b).
- TAP fast-load via LOCI + MIA_BOOT (E2E 34b5).
- xstack push/pop, MIA register file (probably `tests/unit/test_loci.c` — not read, but referenced in the Makefile).

### Insufficiently covered
- **API op error paths**: no test observes `errno=ENOSYS/EACCES/EIO/EBADF` on the 6502 side. A silent ENOSYS false positive would go unnoticed.
- **FDC timing edge cases**: reverse interleave, multi-track Read on an 18-sector DSK (vs the default 17). The 34ay comment mentions `interleave par address mark ID` (*interleave by address mark ID*), but no test confirms it.
- **CTRL race conditions**: `op_mia_boot` loads `microdis.rom`, then Sedoric writes `$0314` before the first `RESTORE`. A differential test on the exact order of the `sync_overlay` calls (expected sequence) would prevent a silent regression.
- **Mixed FDC + SDIMG + flash_root cleanup**: no test for a clean exit with SDIMG files open + directories open + DSK mounted.
- **Saturated `xstack` limit**: no test sends a 256-byte path.
- **Path traversal security**: no test for `..` / `0:..\..\etc\passwd`.
- **WRITE_XSTACK with count = `LOCI_XSTACK_SIZE`**: boundary edge case.
- **`op_tap_read_header` on a corrupted TAP** (no sync mark, truncated header).

---

## 6. Actionable recommendations (sprint 34c)

Sorted by decreasing value/effort ratio:

| # | Action | Effort | Value |
|---|--------|--------|--------|
| **R1** | Fix POSIX `op_readdir`: track the resolved path per dir slot + use the right base for `stat()` (`loci.c:1549-1551`). Add a unit test with a subdirectory. | XS (~15 LOC + 1 test) | Avoids a user-visible bug as soon as the first UI descends into a subdirectory. |
| **R2** | Harden `resolve_path`: tokenise on `/`, reject exact `..` (not `strstr`). Add 3 `EACCES` tests + 1 "`my..file.tap` accepted" test (`loci.c:702-718`). | XS (~10 LOC + 4 tests) | Real security + unblocks legitimate names. |
| **R3** | Persist DSK writes in raw mode: flush `image → fp` in `dsk_close` + `loci_cleanup`. Add a SAVE→quit→re-mount→LOAD test via `--loci-flash` (`loci.c:1875-1889`). | S (~20 LOC + 1 E2E test) | Fixes a real silent data loss in flash mode. |
| **R4** | Split `loci.c` into 4 TUs (`core / fs / bus / boot`). No vtable extraction yet — just a mechanical split with private headers. | M (~1 day, mechanical refactor) | Maintenance cost drops sharply, and the subsequent vtable extraction becomes trivial. |
| **R5** | Add an **error path** unit test: `test_loci_errno.c` driving the dispatcher with invalid inputs and checking the reported LOCI `errno` (`$03AD/AE`). Covers EBADF, EINVAL, ENOENT, EACCES over 8-10 ops. | S (~150 LOC) | Safety net against silent regressions of the error paths. |

**Out of scope for sprint 34c, to be recorded**:
- Revisit Q1 of the 34az closeout (full switch to `microdisc_t` instead of the `fdc_t` bridge) if another CTRL bug shows up in write mode.
- A clean-room mini Sedoric disk for versioned E2E without proprietary dependencies.
- Publicly document the spin-window semantics for future contributors (a README in `src/io/loci/`).

---

*Reviewer: staff engineer, full read of `loci.c` + diff 34ax→34az + main.c wiring.*
*No blocking bug for merging sprints 34b0-b5. R1/R2/R3 recommended before the next release tag.*
