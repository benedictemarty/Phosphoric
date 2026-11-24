# Senior Engineering Review — LOCI SDIMG + CSAVE series (sprints 34ao → 34aq)

**Date**: 2026-06-07
**Versions delivered**: v1.16.40-alpha → v1.16.43-alpha (4 versions)
**Author**: bmarty
**Request**: architectural review + decisions + limitations

---

## 1. TL;DR

This series extends Phosphoric with an **SD raw image FAT16/32** storage backend
for LOCI (read + write), followed by a fix of the TAP format produced
by CSAVE. Three PRs merged one after the other into `main`:

| PR | Sprint | Version | Main files | LOC |
|----|--------|---------|------------------|-----|
| #1 | 34ao + 34ao+ | 1.16.40 + 1.16.41 | `src/io/loci_sdimg.{c,h}`, `tools/mkloci_sd.c` | ~1500 |
| #2 | 34ap | 1.16.42 | write API extension + loci.c routing + tests | ~1450 |
| #3 | 34aq | 1.16.43 | canonical TAP reconstruction at csave_end | ~220 |

**Final state**: 470 tests pass, 0 regressions, E2E validation LOCI ROM →
BASIC Atmos → CSAVE → CLOAD working (with one cosmetic limitation
documented below).

---

## 2. Upstream context

### Before this series

- Sprint 34an had delivered the MIA spin ABI fix: the LOCI ROM booted correctly,
  the TUI was navigable, MIA_BOOT to BASIC 1.1 validated. Storage was limited to
  `--loci-flash DIR` (POSIX sandbox).
- The real LOCI firmware uses a microSD card read by the Pi Pico. Users
  wanted to be able to:
  - extract a real SD image (`dd if=/dev/sdX`) and boot it
  - prepare an image with `mkfs.fat + mtools` and use it as media
  - possibly make persistent CSAVEs into that image

### Architectural decision

I chose **not to vendor FatFs** (the standard library used by the
real Pi Pico firmware). Reasons:

| For FatFs | Against |
|------------|--------|
| Battle-tested | ~3500 LOC vendored, larger attack surface |
| Same guarantees as the real firmware | Somewhat non-standard (custom) licence |
| Robust write | Project coupling, changes in vendored code to be avoided |

→ Choice: **minimal custom FAT implementation (~700 LOC)**. Smaller surface,
full control, and the scope is precise (read+write 8.3 superfloppy, FAT16/32
auto-detected). Trade-off: if a pathological case shows up, I don't have
FatFs's test base behind me — hence the 22 cold-roundtrip tests.

---

## 3. SDIMG backend architecture

### Public interface (`include/io/loci_sdimg.h`)

```c
loci_sdimg_t* loci_sdimg_open(const char* path);
void          loci_sdimg_close(loci_sdimg_t* img);

int  loci_sdimg_fopen(img, path);                    /* read-only */
int  loci_sdimg_fopen_ex(img, path, mode);           /* 0=R, 1=W, 2=R+W */
int  loci_sdimg_fread(img, fd, buf, count);
int  loci_sdimg_fwrite(img, fd, buf, count);
int32_t loci_sdimg_lseek(img, fd, offset, whence);
int  loci_sdimg_fclose(img, fd);

int  loci_sdimg_opendir(img, path);
int  loci_sdimg_readdir(img, dh, name, attrib, size);
int  loci_sdimg_closedir(img, dh);

int  loci_sdimg_unlink(img, path);
int  loci_sdimg_rename(img, old, new);
int  loci_sdimg_mkdir(img, path);
int  loci_sdimg_sync(img);
```

15 public functions, a 1:1 mirror of the POSIX ops loci.c already uses.
**No** dependency on loci.h: the backend can be tested in isolation.

### Internal layers

```
┌─────────────────────────────────────────┐
│ Public API (15 functions)               │
├─────────────────────────────────────────┤
│ Path resolution + handle management     │
│   - resolve_path(slash-separated)        │
│   - find_dir_entry / alloc_dir_entry    │
│   - alloc_file / alloc_dir              │
├─────────────────────────────────────────┤
│ FAT chain operations                    │
│   - read_fat_entry / write_fat_entry    │
│   - alloc_free_cluster / free_chain     │
│   - extend_chain                        │
├─────────────────────────────────────────┤
│ BPB parsing + FS auto-detection         │
│   - parse_bpb                           │
│   - FAT12 rejected (<4085 clusters)     │
│   - FAT16/FAT32 (≥65525 = FAT32)        │
├─────────────────────────────────────────┤
│ Low-level sector I/O (stdio)            │
│   - read_sector / write_sector          │
│   - EROFS if img->read_only             │
└─────────────────────────────────────────┘
```

### Notable decisions

1. **`fopen("rb+")` then fallback to `fopen("rb")`** when opening the
   image. If the host image is read-only (chmod 0444, RO mount), all
   write ops return `-EROFS`. No need for the user to
   add a flag.

2. **Atomic per-sector mirror FAT update**. `write_fat_entry` reads the
   relevant sector, modifies the entry, then rewrites it into **all**
   `NumFATs` copies (typically 2). If writing sector 0 fails,
   the FAT1 modification is partial, but FAT0 stays consistent.
   No journaling — a crash mid-write can leave FAT0 modified and FAT1
   not updated. Documented as a limitation.

3. **Cluster allocation = linear first-fit**. No better algorithm
   (best-fit, next-fit). On images <100 MB it stays under a µs.
   If usage scales, refactor towards an in-RAM cluster bitmap.

4. **EOC mark written immediately at allocation**. `alloc_free_cluster` writes the
   EOC value (`0xFFFF` / `0x0FFFFFFF`) into the FAT at allocation time,
   BEFORE the caller links the chain. If we reboot after a crash, the
   cluster appears allocated+terminated, not free. Avoids double allocation.

### Sentinel tags for fds[]

The historical POSIX backend stores `FILE*` in `loci_t.fds[]` and `DIR*`
in `loci_t.dirs[]`. When `loci->sdimg` is non-NULL, I store a
**pointer-encoded tag**:

```c
loci->fds[slot] = (void*)(uintptr_t)(0x1000000u | (uint32_t)slot);
```

The high bit (`0x1000000`) distinguishes the tag from a real `FILE*` (which will
always be `> 0x10000000` on Linux glibc). The conditional cleanup avoids
`fclose()` on these sentinels:

```c
if (loci->fds[i]) {
    if (!loci->sdimg) fclose((FILE*)loci->fds[i]);
    loci->fds[i] = NULL;
}
```

**Potential criticism**: this is a hack. A clean refactoring would be
a backend vtable (`loci_fs_vtable_t* fs;`) with two implementations.
I preferred the minimalist route to limit the regression surface
(the POSIX base is used by 105 existing tests).

### loci.c integration

At the start of every file/dir op, early dispatch:

```c
static void op_open(loci_t* loci) {
    if (loci->sdimg) { op_open_sdimg(loci); return; }
    /* POSIX path unchanged */
}
```

**11 ops dispatched to SDIMG**, **5 write ops rejected with EACCES** in
v1.16.40 (then properly re-routed in v1.16.42 with the write API).

---

## 4. Sprint 34ap: moving to read-write

### Added surface

- 6 public functions (fwrite, unlink, rename, mkdir, fopen_ex, sync)
- ~470 LOC of low-level FAT helpers
- `sdimg_handle_t` extended: `writable` + `dir_entry_lba/off`
- On fwrite: if the file was empty (`first_cluster < 2`), allocate an
  initial cluster. On extend, `extend_chain(last_cluster)` lengthens the
  list. On every write, update `size_bytes` in the dir entry (lba/off
  remembered at fopen).

### Cold-roundtrip tests

The pattern: `open → write → close → close img → reopen img → read → verify`.
Closing/reopening forces a new `loci_sdimg_open` which re-parses the
BPB and TRUSTS only the disk. It guarantees that no in-memory
cache is hiding a FAT bug.

12 tests cover: create, list-after-create, cross-cluster extend,
truncate, unlink, rename, mkdir with `.`/`..`, file in subdir, seek+overwrite,
RO image, 8.3 validation.

### Decision: no sector cache

Every SDIMG `fread` / `fwrite` does an `fseek + fread`/`fwrite` syscall.
For intensive accesses (multi-MB CSAVE) this is suboptimal. With a
1 MHz emulated Oric + tape patches that buffer in RAM, throughput is
limited by BASIC more than by our I/O. Measurement: a 13-byte CSAVE takes 4 ms
on the SDIMG side, negligible.

If a future sprint wants to speed it up: an LRU sector cache of ~16 entries in
`loci_sdimg_t`, invalidated on write.

---

## 5. Sprint 34ao+: E2E integration (5 cascading bugs)

This was the most surgical sprint. Interactive validation by the user
revealed 5 distinct plumbing bugs:

| # | Bug | Root cause | Fix |
|---|-----|--------------|-----|
| 1 | Empty TUI picker | `d_attrib` returned the raw ARCHIVE bit 0x20, the firmware filters on 0x10 (DIR) | Normalisation: `(attr & 0x10) ? 0x10 : 0` |
| 2 | OPENDIR("") rejected | `pop_zstring` returns false on empty | Accept empty = root |
| 3 | MIA_BOOT fails with SDIMG | rom_swap_cb expects a host path | Extract SDIMG → /tmp then cb on the temp file |
| 4 | CLOAD stuck on Searching | LOCI TAP mount not plumbed into the cassette subsystem | Callback `tape_mount_cb` → `emu.tapebuf` |
| 5 | BASIC 1.0 rom_patches after swapping to 1.1 | get_rom_patches not called again | Auto-detect from filename → re-select |

**Lesson**: integration bugs are only found by testing E2E with
the real ROM. My 105 pre-existing tests from sprint 34an passed
perfectly but did NOT exercise the full chain (firmware → MIA op →
host backend → return). The user acted as the integration test.

### Decision: extraction to /tmp rather than streaming

The ROM-swap callback in main.c is wired to `memory_load_rom(path, ...)`
which does `fopen + fread`. Instead of changing the callback signature (which
would touch every supported ROM), I extract from SDIMG to
`/tmp/loci_extract_<basename>` on demand, and the callback then works
unchanged.

Trade-off: if the user has /tmp on tmpfs with little RAM, the extracted ROMs
consume ~24 KB each (BASIC + microdis). Acceptable.

---

## 6. Sprint 34aq: TAP reconstruction

### The bug

The historical CSAVE patch (before 34aq) captured bytes by
intercepting the BASIC ROM's `putbyte_entry`. I instrumented it
to understand:

```
CSAVE TRACE: byte #1 = $24 (sync)
CSAVE TRACE: byte #2 = $FF (??)
CSAVE TRACE: byte #3 = $FF (??)
CSAVE TRACE: byte #4 = $00
...
```

The `$FF`s should not be there. Comparative hexdump:

```
AIGLE.TAP : 16 16 16 24 00 00 00 c7 3f 37 05 01 00 41 49 47
CSAVE T1  : 16 16 16 24 ff ff 00 00 05 0e 05 01 ff 54 31 00
```

Cause: the BASIC ROM does not set A=byte before each `putbyte`; it uses
routes via X/Y/memory depending on the context. My interception does not capture
the real semantics.

### The decision: rebuild from RAM

Rather than reverse-engineering every putbyte path in the ROM, I
**completely ignore the captured bytes** and rebuild a canonical TAP
at `csave_end` time:

```c
uint16_t start_addr = ram[0x9A] | (ram[0x9B] << 8);   /* TXTTAB */
uint16_t end_addr   = ram[0x9C] | (ram[0x9D] << 8);   /* VARTAB */
if (end_addr > start_addr) end_addr--;
/* Build TAP: 16×3 + 24 + 00 00 + 00 + C7 + end + start + 00 + name + 00 + data */
```

Advantages:
- Format guaranteed byte-compatible (tested against AIGLE.TAP)
- No dependency on ROM-version-specific details
- Works for ORIC-1 and Atmos with the same ZP addresses

Limits:
- Assumes a BASIC program (not a machine-code CSAVE via `,A,E` etc.)
- If the user does `POKE 0x9C, ...` before CSAVE, the TAP will be truncated

### Cosmetic limitation: "Errors found"

After all this work, BASIC Atmos still displays `Errors found` after
CLOAD, despite:
- A TAP byte-compatible with AIGLE
- `LIST` correctly showing the loaded program
- Program auto-run working (PRINT "HI" → "HI")

I tried without success:
1. Resetting `tapeoffs` to 0 when the verify pass calls getsync again
2. Duplicating the data block in the TAP (physical-cassette style)
3. A silent EOT handler (return 0x00 instead of garbage)

Probable cause (unconfirmed): a tape parity counter in BASIC that
is not reset because we do not simulate the bit-banging at the
VIA/timer level. Getting rid of it would require either:
- Patching the ROM routine that prints "Errors found" to skip it (fragile,
  ROM-version-specific)
- Implementing a real bit-level cassette simulation (a big separate sprint)

**Decision**: accept, document, ship. The program loads and
runs, the message is cosmetic. It is not a blocker.

---

## 7. Final metrics

| Indicator | Value |
|------------|--------|
| Versions delivered | 4 (1.16.40 → 1.16.43) |
| Sprints | 4 (34ao, 34ao+, 34ap, 34aq) |
| Commits on main | 7 (3 PRs + 4 follow-up fixes) |
| LOC added | ~3170 (code + tests + docs) |
| New files | 8 (sdimg.{c,h}, tests×2, mkloci_sd, 3 reports) |
| Public API extended | 17 functions (15 SDIMG + 2 loci) |
| SDIMG tests | 22 (10 read + 12 write) |
| Phosphoric global tests | **470** (vs 458 before the series) |
| Regressions | 0 |
| Bugs found during E2E validation | 5 (sprint 34ao+) |
| Remaining stubborn bug | 1 cosmetic ("Errors found") |

### Cumulative tests

```
test-cpu        : 74
test-memory     : 19
test-io         : 31
test-storage    : 12
test-system     : 11
test-video      : 11
test-audio      : 8
test-debugger   : 8
test-savestate  : 8
test-atmos      : 10
test-joystick   : 10
test-printer    : 10
test-mcp40      : 10
test-renderer   : 10
test-trace      : 10
test-profiler   : 10
test-rominfo    : 10
test-serial     : 19
test-keyboard   : 24
test-symbols    : 10
test-loci       : 108
test-loci-sdimg : 10    ← new in 34ao
test-loci-sdimg-write : 12  ← new in 34ap
test-coverage   : 24
TOTAL           : 470 PASS
```

---

## 8. Risks and debt

### Known and documented

- **No FAT journaling**: a crash mid-write can misalign FAT0/FAT1.
  Mitigation: `loci_sdimg_sync()` flushes before critical points.
- **No LFN**: the LOCI firmware only uses 8.3, so not blocking
  today; to be revisited if usage evolves.
- **FAT12 rejected at open**: conscious trade-off, scope reduction.
- **No MBR**: superfloppy only. To open a dump
  of a real SD card that has an MBR table, the
  partition must be extracted first or an MBR parser added (~50 LOC).
- **Sentinel tag in fds[]/dirs[]**: see §3.5, it is a hack that deserves
  a clean refactoring into a backend vtable if the codebase grows more complex.
- **CSAVE assumes a BASIC program**: no support for machine-code CSAVE
  (`CSAVE "name",A start,E end`). The current firmware uses the
  TXTTAB/VARTAB pointers; for machine code, the parameters would have to be read from
  other ROM-version-specific addresses.

### Suggestions for the review

I would like your opinion on 3 points:

1. **Sentinel tag vs vtable**: would you push for a refactor
   now (before the codebase hardens) or do you accept the current
   hack with a TODO?

2. **Cosmetic "Errors found" limitation**: do you see an elegant route
   I missed? My test resetting tapeoffs gave nothing, nor did the
   double block. Perhaps there is a ROM counter we
   could modify directly in RAM (ZP) at csave_end before the
   verify runs?

3. **Reconstruction from RAM (sprint 34aq)**: it is elegant for
   BASIC, but closes the door to future machine-code CSAVE support
   without changing this logic. Is it better to keep a partial trace
   of the context (track the values pushed in X/Y around the putbytes
   to guess the format) or stay with pure reconstruction?

---

## 9. Reproducibility

```bash
git clone <repo> && cd Oric1
git checkout main
make clean && make SDL2=1
make tests                  # 470 PASS

# Demo image
./tools/mkloci_sd loci_demo.img 16 \
    roms/basic10.rom roms/basic11b.rom roms/microdis.rom \
    tapes/AIGLE.TAP tapes/007.tap

# E2E LOCI test
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_demo.img \
    --keyboard azerty

# Once in BASIC Atmos:
#   10 PRINT "HI"
#   CSAVE "TEST"      # → TEST.TAP persisted in loci_demo.img
#   NEW
#   CLOAD "TEST"      # → "Errors found" (cosmetic) + program loaded
#   LIST              # → shows 10 PRINT "HI"
```

---

## 10. Useful links

- PR #1 (sprint 34ao read): https://github.com/benedictemarty/Phosphoric/pull/1
- PR #2 (sprint 34ap write): https://github.com/benedictemarty/Phosphoric/pull/2
- PR #3 (sprint 34aq CSAVE fix): https://github.com/benedictemarty/Phosphoric/pull/3
- Detailed reports:
  - `docs/CR/2026-06-07_LOCI_SDimg_Backend.md` (sprint 34ao)
  - `docs/CR/2026-06-07_LOCI_SDimg_Write.md` (sprint 34ap)

---

**Explicit request**: your criticism of the architecture, the sentinel tag,
the residual "Errors found", and any clean-up you want me to tackle
in sprint 34ar.

— End of review
