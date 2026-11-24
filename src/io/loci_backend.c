/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file loci_backend.c
 * @brief Dispatch of the loci_emu.h interface to the LOCI backend chosen at
 *        launch: stub (none), emul (co-simulated firmware, --loci-emu) or hw
 *        (LOCI-USB: the Feather runs the LOCI firmware, --loci-hw)
 * @author bmarty <bmarty@mailo.com>
 *
 * Each backend is compiled with its functions renamed (loci_be_rename.h);
 * those present in this binary are announced by the Makefile (LOCI_BE_HAS_EMUL,
 * LOCI_BE_HAS_HW). With no choice, it is the stub: external LOCI inactive. Two
 * functions are called very often without an external LOCI (active: every
 * ROM read; idle_poll: every instruction): they avoid the indirect
 * call as long as the stub is in place.
 */
#include "io/loci_emu.h"
#include "loci_be_api.h"
#include <stddef.h>
#include <string.h>

typedef struct {
    const char* name;   /* name passed to loci_emu_select */
#define LBE_FIELD_F(r, n, p, a) r (*n) p;
#define LBE_FIELD_V(n, p, a) void (*n) p;
    LOCI_BE_API(LBE_FIELD_F, LBE_FIELD_V, LBE_FIELD_F)
} loci_be_t;

#define LBE_CAT2(a, b) a##b
#define LBE_CAT(a, b) LBE_CAT2(a, b)
#define LBE_DECL_F(r, n, p, a) r LBE_CAT(LBE_PFX, LBE_CAT2(_, n)) p;
#define LBE_DECL_V(n, p, a) void LBE_CAT(LBE_PFX, LBE_CAT2(_, n)) p;
#define LBE_INIT_F(r, n, p, a) .n = LBE_CAT(LBE_PFX, LBE_CAT2(_, n)),
#define LBE_INIT_V(n, p, a) .n = LBE_CAT(LBE_PFX, LBE_CAT2(_, n)),

#define LBE_PFX lbe_stub
LOCI_BE_API(LBE_DECL_F, LBE_DECL_V, LBE_DECL_F)
static const loci_be_t k_stub = { "stub", LOCI_BE_API(LBE_INIT_F, LBE_INIT_V, LBE_INIT_F) };
#undef LBE_PFX

#ifdef LOCI_BE_HAS_EMUL
#define LBE_PFX lbe_emul
LOCI_BE_API(LBE_DECL_F, LBE_DECL_V, LBE_DECL_F)
static const loci_be_t k_emul = { "emul", LOCI_BE_API(LBE_INIT_F, LBE_INIT_V, LBE_INIT_F) };
#undef LBE_PFX
#endif

#ifdef LOCI_BE_HAS_HW
#define LBE_PFX lbe_hw
LOCI_BE_API(LBE_DECL_F, LBE_DECL_V, LBE_DECL_F)
static const loci_be_t k_hw = { "hw", LOCI_BE_API(LBE_INIT_F, LBE_INIT_V, LBE_INIT_F) };
#undef LBE_PFX
#endif

static const loci_be_t* const k_backends[] = {
#ifdef LOCI_BE_HAS_EMUL
    &k_emul,
#endif
#ifdef LOCI_BE_HAS_HW
    &k_hw,
#endif
    NULL
};

static const loci_be_t* g_be = &k_stub;

static const loci_be_t* find(const char* name) {
    for (int i = 0; k_backends[i]; i++)
        if (strcmp(k_backends[i]->name, name) == 0) return k_backends[i];
    return NULL;
}

bool loci_emu_backend_available(const char* name) { return name && find(name) != NULL; }

/* Missing: nothing changes, the caller says why (option, dependency). */
bool loci_emu_select(const char* name) {
    const loci_be_t* b = name ? find(name) : NULL;
    if (b) g_be = b;
    return b != NULL;
}

/* ── Dispatch ──────────────────────────────────────────────────────────── */

#define LBE_FWD_F(r, n, p, a) r LBE_CAT(loci_emu_, n) p { return g_be->n a; }
#define LBE_FWD_V(n, p, a) void LBE_CAT(loci_emu_, n) p { g_be->n a; }
#define LBE_FWD_NONE(r, n, p, a)
LOCI_BE_API(LBE_FWD_F, LBE_FWD_V, LBE_FWD_NONE)

bool loci_emu_active(void) { return g_be != &k_stub && g_be->active(); }

int loci_emu_idle_poll(int cycles) {
#ifdef LOCI_BE_HAS_HW
    if (g_be == &k_hw) return lbe_hw_idle_poll(cycles);
#endif
    (void)cycles;
    return 0;
}
