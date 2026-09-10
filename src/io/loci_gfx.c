/*
 * loci_gfx.c — émulation de l'assist graphique LOCI (opcode MIA $AE), lot 1
 *              « composition-pure » (spec extensions/graphics-assist-AE §8).
 *
 * Miroir de la logique du firmware (firmware/src/mia/api/gfx.c/.h) et des tests
 * natifs (extensions/graphics-assist-AE/tests/gfx_ops_test.c) : back-buffer en
 * banque XRAM 16 Ko (loci->xram[(bank<<14)+off]), carte des *dirty spans*
 * déterministe, primitives d'octets (clear/blit/compose) + collision bbox. Le
 * blit final vers l'écran reste au 6502 (aucun DMA) — non concerné ici.
 *
 * Sous-code dans API_A ; paramètres/retours sur le xstack, mêmes conventions
 * que les autres op_* (memcpy top-of-stack, xstack_push_n, api_return_*).
 *
 * Ceci est la 3e implémentation (firmware C, test natif C, émulateur C) — les
 * trois partagent EXACTEMENT la sémantique pure, seule change la couche d'accès.
 */

#include "io/loci.h"
#include "io/loci_internal.h"
#include <string.h>

/* ---- sous-codes (API_A) — spec §4 ---- */
#define GFX_INIT          0x00
#define GFX_RESET_DIRTY   0x01
#define GFX_GET_DIRTY     0x02
#define GFX_CLEAR         0x03
#define GFX_BLIT_RECT     0x11
#define GFX_COMPOSE_RECT  0x18
#define GFX_COLLIDE       0x28

#define GFX_MODE_COPY 0
#define GFX_MODE_OR   1
#define GFX_MODE_AND  2
#define GFX_MODE_XOR  3

#define GFX_HIRES_WB   40u
#define GFX_HIRES_ROWS 200u
#define GFX_TEXT_WB    40u
#define GFX_TEXT_ROWS  28u
#define GFX_MAX_BYTES  (GFX_HIRES_WB * GFX_HIRES_ROWS)   /* 8000 */
#define GFX_DIRTY_LEN  ((GFX_MAX_BYTES + 7u) / 8u)       /* 1000 */
#define GFX_SPAN_BUDGET 100

typedef struct { uint16_t off, len; } gfx_span;

/* ---- état (une seule LOCI émulée → statique, comme le firmware) ---- */
static uint8_t  g_dirty[GFX_DIRTY_LEN];
static uint8_t  g_bank  = 0;
static uint16_t g_wb    = GFX_HIRES_WB;
static uint16_t g_rows  = GFX_HIRES_ROWS;
static uint16_t g_bytes = GFX_HIRES_WB * GFX_HIRES_ROWS;
static int      g_ready = 0;

/* ================= logique PURE (miroir gfx.h / gfx_ops_test.c) ============ */

static inline void d_set(uint8_t *d, uint16_t off) { d[off >> 3] |= (uint8_t)(1u << (off & 7u)); }
static inline int  d_get(const uint8_t *d, uint16_t off) { return (d[off >> 3] >> (off & 7u)) & 1; }

static void d_mark(uint8_t *d, uint16_t off, uint16_t len, uint16_t cap) {
    for (uint16_t i = 0; i < len && (uint16_t)(off + i) < cap; ++i) d_set(d, (uint16_t)(off + i));
}

static int coalesce_spans(const uint8_t *d, uint16_t wb, uint16_t rows,
                          gfx_span *out, int max_out, int *overflow) {
    *overflow = 0;
    int n = 0;
    for (uint16_t row = 0; row < rows; ++row) {
        uint16_t base = (uint16_t)(row * wb), col = 0;
        while (col < wb) {
            if (!d_get(d, (uint16_t)(base + col))) { ++col; continue; }
            uint16_t start = col;
            while (col < wb && d_get(d, (uint16_t)(base + col))) ++col;
            if (n < max_out) { out[n].off = (uint16_t)(base + start); out[n].len = (uint16_t)(col - start); }
            ++n;
        }
    }
    if (n <= max_out) return n;
    *overflow = 1;
    n = 0;
    for (uint16_t row = 0; row < rows; ++row) {
        uint16_t base = (uint16_t)(row * wb);
        int first = -1, last = -1;
        for (uint16_t col = 0; col < wb; ++col)
            if (d_get(d, (uint16_t)(base + col))) { if (first < 0) first = col; last = col; }
        if (first < 0) continue;
        if (n < max_out) { out[n].off = (uint16_t)(base + first); out[n].len = (uint16_t)(last - first + 1); }
        ++n;
    }
    if (n <= max_out) return n;
    out[0].off = 0; out[0].len = (uint16_t)(wb * rows);
    return 1;
}

static void compose_row(uint8_t *dst, const uint8_t *src, const uint8_t *mask,
                        uint16_t n, int mode) {
    for (uint16_t i = 0; i < n; ++i) {
        uint8_t s = src[i], dd = dst[i], op;
        switch (mode) {
            case GFX_MODE_OR:  op = (uint8_t)(dd | s); break;
            case GFX_MODE_AND: op = (uint8_t)(dd & s); break;
            case GFX_MODE_XOR: op = (uint8_t)(dd ^ s); break;
            default:           op = s;                 break;
        }
        if (mask) { uint8_t m = mask[i]; dst[i] = (uint8_t)((dd & (uint8_t)~m) | (op & m)); }
        else      { dst[i] = op; }
    }
}

static int collide_bbox(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    if (ax >= bx + bw || bx >= ax + aw) return 0;
    if (ay >= by + bh || by >= ay + ah) return 0;
    return 1;
}

/* ================= couche ABI émulateur ============ */

static uint8_t *gfx_base(loci_t *loci) { return &loci->xram[(uint32_t)g_bank << 14]; }

static int rect_ok(uint16_t off, uint16_t w, uint16_t h) {
    if (w == 0 || h == 0 || w > g_wb) return 0;
    uint32_t last = (uint32_t)off + (uint32_t)(h - 1) * g_wb + w;
    return last <= g_bytes;
}

/* pop n octets du sommet du xstack (idiome op_read_xram). */
static int gfx_pop(loci_t *loci, void *dst, size_t n) {
    if (loci->xstack_ptr + n > LOCI_XSTACK_SIZE) return 0;
    memcpy(dst, &loci->xstack[loci->xstack_ptr], n);
    loci->xstack_ptr += (uint16_t)n;
    return 1;
}

void op_gfx(loci_t *loci) {
    uint8_t a = loci->regs[LOCI_REG_API_A];

    if (a != GFX_INIT && !g_ready) { api_return_errno(loci, LOCI_EINVAL); return; }

    switch (a) {
    case GFX_INIT: {
        uint8_t bank, mode;
        if (!gfx_pop(loci, &bank, 1) || !gfx_pop(loci, &mode, 1)) { api_return_errno(loci, LOCI_EINVAL); return; }
        xstack_zero(loci);
        if (bank > 3 || mode > 1) { api_return_errno(loci, LOCI_EINVAL); return; }
        g_bank = bank;
        if (mode == 1) { g_wb = GFX_TEXT_WB;  g_rows = GFX_TEXT_ROWS;  }
        else           { g_wb = GFX_HIRES_WB; g_rows = GFX_HIRES_ROWS; }
        g_bytes = (uint16_t)(g_wb * g_rows);
        memset(g_dirty, 0, GFX_DIRTY_LEN);
        g_ready = 1;
        api_return_ax(loci, 0);
        return;
    }
    case GFX_RESET_DIRTY:
        memset(g_dirty, 0, GFX_DIRTY_LEN);
        xstack_zero(loci);
        api_return_ax(loci, 0);
        return;

    case GFX_CLEAR: {
        uint8_t value;
        if (!gfx_pop(loci, &value, 1)) { api_return_errno(loci, LOCI_EINVAL); return; }
        xstack_zero(loci);
        memset(gfx_base(loci), value, g_bytes);
        memset(g_dirty, 0xFF, (g_bytes + 7u) / 8u);
        api_return_ax(loci, 0);
        return;
    }
    case GFX_BLIT_RECT: {
        uint8_t w, h; uint16_t src_off, dst_off;
        if (!gfx_pop(loci, &h, 1) || !gfx_pop(loci, &w, 1) ||
            !gfx_pop(loci, &dst_off, 2) || !gfx_pop(loci, &src_off, 2)) { api_return_errno(loci, LOCI_EINVAL); return; }
        xstack_zero(loci);
        if (!rect_ok(src_off, w, h) || !rect_ok(dst_off, w, h)) { api_return_errno(loci, LOCI_EINVAL); return; }
        uint8_t *base = gfx_base(loci);
        for (uint16_t r = 0; r < h; ++r) {
            uint16_t so = (uint16_t)(src_off + r * g_wb), d0 = (uint16_t)(dst_off + r * g_wb);
            compose_row(&base[d0], &base[so], NULL, w, GFX_MODE_COPY);
            d_mark(g_dirty, d0, w, g_bytes);
        }
        api_return_ax(loci, 0);
        return;
    }
    case GFX_COMPOSE_RECT: {
        uint8_t w, h, mode; uint16_t src_off, dst_off, mask_off;
        if (!gfx_pop(loci, &mask_off, 2) || !gfx_pop(loci, &mode, 1) ||
            !gfx_pop(loci, &h, 1) || !gfx_pop(loci, &w, 1) ||
            !gfx_pop(loci, &dst_off, 2) || !gfx_pop(loci, &src_off, 2)) { api_return_errno(loci, LOCI_EINVAL); return; }
        xstack_zero(loci);
        int use_mask = (mask_off != 0xFFFF);
        if (mode > GFX_MODE_XOR || !rect_ok(src_off, w, h) || !rect_ok(dst_off, w, h) ||
            (use_mask && !rect_ok(mask_off, w, h))) { api_return_errno(loci, LOCI_EINVAL); return; }
        uint8_t *base = gfx_base(loci);
        for (uint16_t r = 0; r < h; ++r) {
            uint16_t so = (uint16_t)(src_off + r * g_wb), d0 = (uint16_t)(dst_off + r * g_wb);
            const uint8_t *mask = use_mask ? &base[(uint16_t)(mask_off + r * g_wb)] : NULL;
            compose_row(&base[d0], &base[so], mask, w, mode);
            d_mark(g_dirty, d0, w, g_bytes);
        }
        api_return_ax(loci, 0);
        return;
    }
    case GFX_COLLIDE: {
        uint16_t ax, ay, aw, ah, bx, by, bw, bh;
        if (!gfx_pop(loci, &bh, 2) || !gfx_pop(loci, &bw, 2) || !gfx_pop(loci, &by, 2) || !gfx_pop(loci, &bx, 2) ||
            !gfx_pop(loci, &ah, 2) || !gfx_pop(loci, &aw, 2) || !gfx_pop(loci, &ay, 2) || !gfx_pop(loci, &ax, 2)) {
            api_return_errno(loci, LOCI_EINVAL); return;
        }
        xstack_zero(loci);
        api_return_ax(loci, (uint16_t)collide_bbox(ax, ay, aw, ah, bx, by, bw, bh));
        return;
    }
    case GFX_GET_DIRTY: {
        static gfx_span spans[GFX_SPAN_BUDGET];
        int ovf;
        int n = coalesce_spans(g_dirty, g_wb, g_rows, spans, GFX_SPAN_BUDGET, &ovf);
        uint16_t count = (uint16_t)((n & 0x7FFF) | (ovf ? 0x8000 : 0));
        xstack_zero(loci);
        for (int i = n - 1; i >= 0; --i) {
            xstack_push_n(loci, &spans[i].len, 2);
            xstack_push_n(loci, &spans[i].off, 2);
        }
        xstack_push_n(loci, &count, 2);
        memset(g_dirty, 0, GFX_DIRTY_LEN);   /* reset auto */
        api_return_ax(loci, 0);
        return;
    }
    default:
        api_return_errno(loci, LOCI_EINVAL);
        return;
    }
}
