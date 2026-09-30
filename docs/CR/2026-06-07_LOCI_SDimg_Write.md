# Report — Sprint 34ap: SDIMG read-write 2026-06-07

**Author**: bmarty
**Branch**: `feat/loci-sdimg-write`
**Version delivered**: v1.16.42-alpha
**Status**: Automated tests ✅ — LOCI E2E validation still to do

---

## 1. Motivation

Sprint 34ao delivered a read-only SDIMG backend. This allowed the
LOCI ROM to read files from the SD image but blocked every
write operation (CSAVE, MKDIR, UNLINK, RENAME). This sprint completes
the backend with FAT16/32 write support.

---

## 2. API added

```c
/* Open existing (mode 0=R) or create/truncate (mode 1=W, 2=R+W). */
int  loci_sdimg_fopen_ex(loci_sdimg_t* img, const char* path, int mode);

/* Write at current cursor; extends file + allocates clusters as needed. */
int  loci_sdimg_fwrite(loci_sdimg_t* img, int fd,
                       const void* buf, uint16_t count);

/* Delete file: free chain, mark entry deleted (0xE5). */
int  loci_sdimg_unlink(loci_sdimg_t* img, const char* path);

/* Rename (same dir → in-place; cross-dir → alloc+delete). */
int  loci_sdimg_rename(loci_sdimg_t* img,
                       const char* old_path, const char* new_path);

/* Create directory with "." and ".." entries. */
int  loci_sdimg_mkdir(loci_sdimg_t* img, const char* path);

/* Flush host I/O buffer. */
int  loci_sdimg_sync(loci_sdimg_t* img);
```

---

## 3. Architecture

### Low-level helpers

| Helper | Role |
|--------|------|
| `write_sector` | atomic per-sector write, early EROFS if the image is RO |
| `write_fat_entry` | updates the NumFATs copies of the FAT mirror |
| `alloc_free_cluster` | first-fit search + immediate EOC mark |
| `free_cluster_chain` | frees the whole chain starting from the first cluster |
| `extend_chain` | alloc + link last → new |
| `find_dir_entry` | returns (lba, off) for in-place update |
| `alloc_dir_entry` | finds a free slot, extends the dir's cluster if full |
| `to_fat_83` | normalises host basename → packed 8.3 (uppercase, forbidden chars) |
| `update_dir_entry` | rewrites the size + first_cluster fields |
| `write_new_entry` | creates a new entry (attrib, cluster, size) |
| `mark_entry_deleted` | first byte → 0xE5 |

### Extension of `sdimg_handle_t`

```c
typedef struct {
    /* existing fields ... */
    bool     writable;            /* opened in W or R+W mode */
    uint32_t dir_entry_lba;       /* location of dir entry, for size updates */
    uint32_t dir_entry_off;
} sdimg_handle_t;
```

`dir_entry_lba/off` is filled in at open time (via parent dir lookup
→ find_dir_entry) and used on every fwrite to persist the
new file size.

### Open semantics

```c
FILE* fp = fopen(path, "rb+");
if (!fp) {
    fp = fopen(path, "rb");
    if (fp) img->read_only = true;   // fall back to read-only
}
```

If the host image is read-only (mode 0444 or RO mount), all write
ops return `-EROFS` instead of crashing or corrupting the image.

---

## 4. Routing in loci.c

| Op | Before 34ap | After 34ap |
|----|------------|------------|
| `op_open` with O_CREAT/O_TRUNC/RDWR | EACCES | `fopen_ex(mode≥1)` |
| `op_write_xstack` | EACCES | `fwrite` from xstack |
| `op_write_xram` | EACCES | `fwrite` from the xram window |
| `op_unlink` | EACCES | `loci_sdimg_unlink` |
| `op_rename` | EACCES | `loci_sdimg_rename` |
| `op_mkdir` | EACCES | `loci_sdimg_mkdir` |

Mapping of LOCI modes to SDIMG modes:
```c
int rw = flags & LOCI_O_RDWR;     /* bits 0-1 */
if ((flags & write_flags) || rw == 1 || rw == 3) {
    mode = (rw == 3) ? 2 : 1;     /* RDWR → 2, otherwise W=1 */
}
```

---

## 5. Tests

`tests/unit/test_loci_sdimg_write.c` — 12 cold-roundtrip tests:

| # | Test | Checks |
|---|------|---------|
| 1 | open_blank_image_is_writable | No accidental RO on a fresh image |
| 2 | create_small_file_round_trip | Create + write + close + reopen + read |
| 3 | file_listed_in_root_after_create | Dir entry visible after close |
| 4 | write_across_cluster_boundary | Extend chain over 5000 bytes (>1 cluster) |
| 5 | truncate_existing_via_fopen_ex_w | fopen(W) frees the old chain |
| 6 | unlink_removes_file_and_frees_clusters | Re-create possible after unlink |
| 7 | rename_in_same_dir | Update name in place |
| 8 | mkdir_creates_subdir_with_dot_entries | Cluster init with . and .. |
| 9 | create_file_in_subdir | Parent lookup + extend dir cluster |
| 10 | write_then_seek_overwrite | Read-modify-write in the middle of the file |
| 11 | read_only_image_rejects_writes | chmod 0444 → EROFS on every op |
| 12 | invalid_83_name_rejected | "TOOLONG.TXT" + char '*' rejected |

### Cold-roundtrip pattern

Each test does: `open → write → close+reopen` (new
`loci_sdimg_t*` instance) → `read` to check that the data really
landed on disk (not just in a memory cache).

---

## 6. Validation

```bash
$ make test-loci-sdimg-write
  [12] invalid_83_name_rejected                                PASS
  Results: 12 passed, 0 failed (total: 12)

$ make tests
# 470 tests overall, 0 fail
```

Total count:
- Before 34ap: 458 tests
- After 34ap: **470** (+12 SDIMG write)

---

## 7. Remaining limitations

| Limitation | Notes |
|--------|-------|
| No LFN support | The LOCI firmware uses 8.3 names — not blocking |
| FAT12 rejected | Image < 4 MB → use FAT16 |
| No MBR | Image must be superfloppy (BPB at sector 0) |
| No ftruncate | Truncation goes through `fopen(W)` |
| No cache | 1 fread/fwrite per sector — OK at 1 MHz CPU |
| No journaling | A crash during a write can leave the FAT inconsistent |
| Cross-dir rename | Implemented as alloc+delete (not atomic) |

---

## 8. Metrics

| Indicator | Value |
|------------|--------|
| LOC added in loci_sdimg.c | ~470 |
| LOC added in loci.c | ~110 |
| LOC of write tests | ~370 |
| Public API extended | 6 functions |
| Total SDIMG tests | 22 (10 read + 12 write) |
| Overall Phosphoric tests | 470 |
| Regressions | 0 |
| Sprint in the LOCI series | 16th (34y → 34ap) |
| Version bumped | 1.16.41 → 1.16.42 |

---

## 9. Reproducibility

```bash
git clone <repo> && cd Oric1
git checkout feat/loci-sdimg-write
make clean && make SDL2=1
make test-loci-sdimg-write  # 12 PASS
make tests                  # 470 PASS

# E2E write demo (existing image from sprint 34ao)
./oric1-emu -r roms/loci/locirom --loci --loci-sdimg loci_demo.img
# (in BASIC after MIA_BOOT)
> 10 PRINT "HELLO"
> CSAVE "TEST"
> NEW
> CLOAD "TEST"
> LIST   # should display 10 PRINT "HELLO"
```

---

## 10. To do for the next sprint

- Interactive E2E validation: CSAVE from BASIC → check that the
  file shows up in the LOCI picker in the next session
- Optional: add an LRU sector cache (~16 sectors) to reduce
  fread/fwrite calls during heavy sequential writing
- Optional: MBR partition support (parse the table at sector 0 + offset
  to the first active partition)

---

**Status**: Sprint 34ap delivered on the automated-tests side. Interactive E2E
validation (CSAVE + RELOAD) to be confirmed in the next session.

— End of report
