/**
 * @file loci_internal.h
 * @brief Shared private declarations for the 4 LOCI translation units.
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-06-07
 *
 * NOT a public header — not installed. Used to wire together
 * loci_core.c / loci_fs.c / loci_bus.c / loci_boot.c after the
 * mechanical split (Sprint 34c R4). Function names are preserved
 * across the split — only their linkage changes from static to extern.
 */

#ifndef LOCI_INTERNAL_H
#define LOCI_INTERNAL_H

#include "io/loci.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* ─── errno / BUSY / xstack helpers (defined in loci_core.c) ──── */

/* Sync the top of xstack into the $03AC register (6502-visible). */
void xstack_sync(loci_t* loci);
/* Reset the xstack to empty (ptr = LOCI_XSTACK_SIZE). */
void xstack_zero(loci_t* loci);
/* Push N bytes; returns false on overflow. */
bool xstack_push_n(loci_t* loci, const void* data, size_t n);
bool xstack_push_u32(loci_t* loci, uint32_t v);
bool xstack_push_i32(loci_t* loci, int32_t v);
/* Update the LOCI errno register pair ($03AD / $03AE). */
void set_errno(loci_t* loci, uint16_t e);

/* ─── API return helpers (defined in loci_core.c) ─────────────── */

/* Install the BLOCKED stub at $03B0-$03B3 (6502 spins on BVC -2). */
void api_install_blocked_stub(loci_t* loci);
/* Install the RELEASED stub at $03B0-$03B3 (BVC +0 falls through). */
void api_install_released_stub(loci_t* loci);
/* Patch BUSY bit at $03B2.7. */
void set_busy(loci_t* loci, bool busy);
/* Resolve the current API call with A/X. */
void api_return_ax(loci_t* loci, uint16_t val);
/* Resolve with A/X + SREG (32-bit). */
void api_return_axsreg(loci_t* loci, uint32_t val);
/* Resolve as an error : zero xstack, set errno, return 0xFFFFFFFF. */
void api_return_errno(loci_t* loci, uint16_t e);

/* ─── xram DMA window helpers (defined in loci_core.c) ────────── */

/* Refresh RW0/RW1 register from xram at current addr0/addr1. */
void refresh_rw0(loci_t* loci);
void refresh_rw1(loci_t* loci);

/* ─── FS-side helpers shared with boot TU (defined in loci_fs.c) */

/* Sandbox path resolver — rejects '..' and volume escape attempts. */
bool resolve_path(loci_t* loci, const char* in,
                  char* out, size_t outsize);
/* Map host errno → LOCI errno. */
uint16_t map_errno(int e);
/* Map an SDIMG-layer negative errno → LOCI errno. */
int sdimg_errno_to_loci(int neg_errno);
/* Extract a file from the SD image to a /tmp temp file (mkstemp-safe). */
bool sdimg_extract_to_temp(loci_t* loci, const char* sd_path,
                           char* out_host_path, size_t out_sz);

/* ─── Bus TU exports (defined in loci_bus.c) ──────────────────── */

/* Prefixed with `loci_` to avoid colliding with same-named globals in
 * src/storage/tap.c (tap_close has the same name but a different signature). */
bool loci_tap_open(loci_t* loci, const char* host_path);
void loci_tap_close(loci_t* loci);
bool loci_dsk_open(loci_t* loci, uint8_t drive, const char* host_path);
void loci_dsk_close(loci_t* loci, uint8_t drive);
void loci_dsk_flush(loci_t* loci, uint8_t drive);
/* FDC DRQ/INTRQ callbacks bridged to fdc_t (registered by loci_init). */
void loci_fdc_set_drq(void* userdata);
void loci_fdc_clr_drq(void* userdata);
void loci_fdc_set_intrq(void* userdata);
void loci_fdc_clr_intrq(void* userdata);

/* ─── API op handlers (extern so dispatch in core can call them) */

/* In loci_core.c. */
void op_pix_xreg(loci_t* loci);
void op_rng_lrand(loci_t* loci);
void op_clock(loci_t* loci);
void op_clk_getres(loci_t* loci);
void op_clk_gettime(loci_t* loci);
void op_clk_settime(loci_t* loci);

/* In loci_fs.c. */
void op_open(loci_t* loci);
void op_close(loci_t* loci);
void op_read_xstack(loci_t* loci);
void op_write_xstack(loci_t* loci);
void op_lseek(loci_t* loci);
void op_unlink(loci_t* loci);
void op_rename(loci_t* loci);
void op_read_xram(loci_t* loci);
void op_write_xram(loci_t* loci);
void op_mount(loci_t* loci);
void op_umount(loci_t* loci);
void op_getcwd(loci_t* loci);
void op_opendir(loci_t* loci);
void op_closedir(loci_t* loci);
void op_readdir(loci_t* loci);
void op_mkdir(loci_t* loci);
void op_uname(loci_t* loci);

/* In loci_bus.c. */
void op_tap_seek(loci_t* loci);
void op_tap_tell(loci_t* loci);
void op_tap_read_header(loci_t* loci);

/* In loci_boot.c. */
void op_mia_boot(loci_t* loci);
void op_cpu_phi2(loci_t* loci);
void op_oem_codepage(loci_t* loci);
void op_stdin_opt(loci_t* loci);
void op_map_tune_tmap(loci_t* loci);
void op_map_tune_tior(loci_t* loci);
void op_map_tune_tiow(loci_t* loci);
void op_map_tune_tiod(loci_t* loci);
void op_map_tune_tadr(loci_t* loci);
void op_adj_scan(loci_t* loci);

#endif /* LOCI_INTERNAL_H */
