/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file loci_backend.c
 * @brief Aiguillage de l'interface loci_emu.h vers le backend LOCI choisi au
 *        lancement : stub (aucun), emul (firmware co-simulé, --loci-emu) ou hw
 *        (LOCI-USB : la Feather fait tourner le firmware LOCI, --loci-hw)
 * @author bmarty <bmarty@mailo.com>
 *
 * Chaque backend est compilé avec ses fonctions renommées (loci_be_rename.h) ;
 * ceux présents dans ce binaire sont annoncés par le Makefile (LOCI_BE_HAS_EMUL,
 * LOCI_BE_HAS_HW). Sans choix, c'est le stub : LOCI externe inactif. Deux
 * fonctions sont appelées très souvent sans LOCI externe (active : chaque
 * lecture de ROM ; idle_poll : chaque instruction) : elles évitent l'appel
 * indirect tant que le stub est en place.
 */
#include "io/loci_emu.h"
#include "loci_be_api.h"
#include <stddef.h>
#include <string.h>

typedef struct {
    const char* name;   /* nom passé à loci_emu_select */
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

/* Absent : rien ne change, l'appelant dit pourquoi (option, dépendance). */
bool loci_emu_select(const char* name) {
    const loci_be_t* b = name ? find(name) : NULL;
    if (b) g_be = b;
    return b != NULL;
}

/* ── Aiguillage ────────────────────────────────────────────────────────── */

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
