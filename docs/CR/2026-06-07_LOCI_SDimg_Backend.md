# Report — Sprint 34ao: LOCI raw SD image backend 2026-06-07

**Author**: bmarty
**Branch**: `feat/loci-sdimg`
**Version delivered**: v1.16.40-alpha
**Status**: Complete read-only implementation, 458 tests pass

---

## 1. Motivation

The previous sprint (34an) delivered LOCI end to end, but host-side
storage was limited to the POSIX sandbox (`--loci-flash DIR`). On the
real LOCI hardware, storage is a **microSD read by the Pi Pico**
with a FAT file system. This difference prevents testing
real SD images (extracted with `dd`) or images generated
by standard tools (`mkfs.fat`, `mtools`).

The goal of this sprint: add a `--loci-sdimg PATH.img` backend
that parses a raw FAT16/32 disk image and exposes its
files to the LOCI File I/O ops, without touching the code of the
6502-side LOCI ROM firmware.

---

## 2. Delivered architecture

### Isolated module

```
include/io/loci_sdimg.h    (~70 LOC, public API)
src/io/loci_sdimg.c        (~470 LOC, read-only FAT16/32 parser)
tests/unit/test_loci_sdimg.c (~250 LOC, 10 tests + FAT16 generator)
```

No external dependency: no FatFs, no libfat. Custom
implementation suited to our specific use (read-only, 8.3, superfloppy).

### Attack surface on loci.c

Minimal, to limit the risk of regression:
- 1 field added: `void* sdimg` in `loci_t`
- 11 ops dispatch to SDIMG when `loci->sdimg` is non-NULL
- 5 write ops cleanly reject with EACCES
- 2 public functions: `loci_attach_sdimg` / `loci_detach_sdimg`

### Handle mapping

The SDIMG backend uses internal slots (0..15 for files,
0..7 for dirs). To avoid collisions with the POSIX backend,
a **non-pointer sentinel tag** is stored in `fds[i]` / `dirs[i]`:
- `(void*)(0x1000000u | slot)` for files
- `(void*)(0x2000000u | slot)` for dirs

Cleanup (`loci_cleanup`) skips `fclose`/`closedir` when
`loci->sdimg` is active — detaching frees the real internal
handles of the backend.

---

## 3. FAT implementation

### FS auto-detection

Microsoft FAT spec rule (fixed size, no magic):
```c
if (count_of_clusters < 4085)        rejected (FAT12 unsupported)
else if (count_of_clusters < 65525)  → FAT16
else                                  → FAT32
```

### BPB parsing (sector 0)

| Offset | Field | Use |
|--------|-------|-------|
| 11 | bytes_per_sector | usually 512 |
| 13 | sectors_per_cluster | |
| 14 | reserved_sectors | FAT1 offset |
| 16 | num_fats | usually 2 |
| 17 | root_entries | FAT16 only |
| 19 | total_sectors_16 | 16-bit fallback |
| 22 | fat_size_16 | FAT16 |
| 32 | total_sectors_32 | if total16 = 0 |
| 36 | fat_size_32 | FAT32 |
| 44 | root_cluster | FAT32 only |

### FAT chain walk

`read_fat_entry(cluster)`:
- FAT16: `entry = u16[FAT_start + cluster*2]`, EOC ≥ 0xFFF8
- FAT32: `entry = u32[FAT_start + cluster*4] & 0x0FFFFFFF`, EOC ≥ 0x0FFFFFF8

### Directory iteration

Each entry is 32 bytes. The scan skips:
- byte 0x00 → end of directory
- byte 0xE5 → deleted entry
- attribute 0x0F (LFN) → long filename slot
- attribute 0x08 (VOLUME_ID) → volume label

The 8.3 name is rebuilt → `"NAME.EXT"` (extension omitted if empty).

### Case-insensitive lookup

`ci_strcmp` compares in upper case. Convention adopted:
- Input path: free-form (`hello.txt` ok)
- FAT representation: fixed-width upper-case 8.3
- `readdir` output: normalised "NAME.EXT" format

---

## 4. errno mapping

POSIX → LOCI:

| POSIX | LOCI | Case |
|-------|------|-----|
| ENOENT | LOCI_ENOENT (1) | file not found |
| EACCES | LOCI_EACCES (3) | write attempt |
| EISDIR | LOCI_EACCES (3) | open() on a dir |
| ENOTDIR | LOCI_EINVAL (7) | opendir() on a file |
| EBADF | LOCI_EBADF (16) | invalid fd |
| EMFILE | LOCI_EMFILE (5) | fd table full |
| EIO | LOCI_EIO (11) | low-level I/O failed |
| other | LOCI_EIO | conservative default |

---

## 5. Tests

10 tests in `tests/unit/test_loci_sdimg.c`:

| # | Test | Checks |
|---|------|---------|
| 1 | open_image_detects_fat16 | FS auto-detection + total_size |
| 2 | open_nonexistent_fails | NULL if file missing |
| 3 | opendir_root_lists_entries | Root enumeration, correct attributes |
| 4 | fopen_read_hello | Complete read of a small file |
| 5 | fopen_case_insensitive | "hello.txt" ↔ "HELLO.TXT" |
| 6 | fopen_nested_path | "SUB/INSIDE.BIN" cross-directory |
| 7 | fopen_missing_returns_enoent | Correct errno |
| 8 | lseek_set_cur_end | 3 seek modes |
| 9 | opendir_subdir_lists_inside | Subdir listing + end-of-dir |
| 10 | fopen_bad_handle_close | EBADF on invalid fd |

### Inline FAT16 image generator

To avoid committing a binary to git, the test generates a minimal FAT16
image on the fly in `/tmp/loci_sdimg_test_<PID>.img`:
- 4 BPB sectors + 2 FATs of 32 sectors
- Root dir with "HELLO.TXT" (13 bytes), "SUB" (dir), volume label
- "SUB" → "INSIDE.BIN" (4 bytes: `DE AD BE EF`)
- Padded to 8000 sectors (~4 MB) to reach the FAT16 threshold

Cleanup at the end of main(), even on failure.

---

## 6. Validation

```bash
$ make tests
# 458 tests, 0 fail
$ make test-loci-sdimg
Test image: /tmp/loci_sdimg_test_364040.img
  [1] open_image_detects_fat16                           PASS
  [2] open_nonexistent_fails                             PASS
  [3] opendir_root_lists_entries                         PASS
  [4] fopen_read_hello                                   PASS
  [5] fopen_case_insensitive                             PASS
  [6] fopen_nested_path                                  PASS
  [7] fopen_missing_returns_enoent                       PASS
  [8] lseek_set_cur_end                                  PASS
  [9] opendir_subdir_lists_inside                        PASS
  [10] fopen_bad_handle_close                            PASS
  Results: 10 passed, 0 failed (total: 10)
$ make test-loci
  Results: 108 passed, 0 failed (total: 108)  # no regression
```

---

## 7. End-to-end usage

### Creating a usable SD image

```bash
# 16 MB of empty FAT16
dd if=/dev/zero of=sdcard.img bs=1M count=16
mkfs.fat -F 16 -n LOCI sdcard.img

# Push files into it (mtools without /etc/mtools.conf)
MTOOLSRC=/dev/null mcopy -i sdcard.img roms/basic11b.rom ::/BASIC11.ROM
MTOOLSRC=/dev/null mcopy -i sdcard.img tapes/asteroids.tap ::/AST.TAP

# Run LOCI on it
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg sdcard.img
```

### Compatibility with a real Pi Pico SD card

```bash
# Copy from a real LOCI microSD
sudo dd if=/dev/sdX of=loci_real.img bs=1M status=progress
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_real.img
```
→ useful to debug a behaviour observed on real hardware
without risking the physical card.

---

## 8. Known limitations

| Limitation | Workaround | Roadmap |
|--------|------------|---------|
| Read-only | Prepare the image on the host with `mtools` | Sprint 34ap considered: write + FAT alloc |
| No LFN | Rename to 8.3 (FILE1.TXT) | Future, on request |
| No MBR | Image must be superfloppy | Future, on request |
| FAT12 rejected | Use FAT16 (image ≥ ~4 MB) | Not planned |
| No cache | OK for the Oric (1 MHz) | Benchmark before optimising |

---

## 9. Metrics

| Indicator | Value |
|------------|--------|
| LOC added | ~790 (sdimg.c + .h + test + integration) |
| Tests added | 10 SDIMG |
| Total Phosphoric tests | 458 (vs 448 before) |
| Regressions | 0 |
| SDIMG file/dir ops | 11 (read) + 5 rejected (write) |
| Sprint in the LOCI series | 15th (34y → 34ao) |
| Bumped version | 1.16.39-alpha → 1.16.40-alpha |

---

## 10. Reproducibility

```bash
git clone <repo> && cd Oric1
git checkout feat/loci-sdimg
make clean && make SDL2=1
make test-loci-sdimg   # 10 PASS
make tests             # 458 PASS

# Quick demo image
dd if=/dev/zero of=demo.img bs=1M count=16
mkfs.fat -F 16 demo.img
MTOOLSRC=/dev/null mcopy -i demo.img roms/basic11b.rom ::/BASIC11.ROM
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg demo.img
```

---

**Status**: Sprint 34ao delivered, automated test validation ✅.
Interactive validation on the real LOCI ROM: to be done during the next
E2E boot (the LOCI ROM will request files through FOPEN/OPENDIR, which
will be served from the FAT image instead of the POSIX sandbox).

— End of report
