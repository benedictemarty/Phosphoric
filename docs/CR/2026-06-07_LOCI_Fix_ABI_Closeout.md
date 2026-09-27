# Compte-rendu — Fermeture LOCI / Fix ABI 2026-06-07

**Auteur** : bmarty
**Branche** : `fix/mia-spin-abi` → mergée sur `main` (commit `26a3977`)
**Version livrée** : v1.16.39-alpha

---

## 1. Contexte

Suite au CR du 2026-06-06 (Sprint 34am, pre-seed PSG R7=$7F) qui
laissait le clavier inopérant malgré le fix R7, l'ingé principal a
mené une review approfondie du code source LOCI ROM + firmware Pi Pico,
puis identifié la **vraie cause racine** : l'ABI de la spin window
`$03B0-$03B9` n'était pas matérialisée.

---

## 2. Cause racine (analyse de l'ingé principal)

### Le contract MIA n'est pas un register file

La fenêtre `$03B0-$03B9` du firmware LOCI est **du code 6502
auto-modifiant** que le coprocesseur Pi Pico réécrit à chaque
transition d'opération. Le 6502 ABI est :

```asm
STA  $03AF       ; trigger op
JSR  $03B0       ; CALL la spin window
; on return: A = result_lo, X = result_hi
;            $03B8/B9 contiennent SREG (high 16 bits pour AXSREG)
```

### Blocked stub (op queued, BUSY=1)

```
$03B0  B8           CLV
$03B1  50 FE        BVC -2     (rebouclage permanent sur $03B0)
$03B3  A9 --        LDA #--    (operand A à patcher par release)
```

L'octet `$03B2 = $FE` encode **simultanément** :
- l'opérande de BVC (= -2, branche en arrière)
- le flag BUSY (bit 7 = 1)

### Released stub (op done, BUSY=0)

```
$03B0  B8           CLV
$03B1  50 00        BVC +0     (fall-through)
$03B3  A9 <A_lo>    LDA #A
$03B5  A2 <X_hi>    LDX #X
$03B7  60           RTS
$03B8  <SREG_lo>
$03B9  <SREG_hi>
```

L'octet `$03B2 = $00` encode :
- l'opérande de BVC (= +0, donc fall-through)
- le flag BUSY (bit 7 = 0)

### Le bug

Phosphoric prior à ce fix :
- Écrivait `$03B4` (A), `$03B6` (X), `$03B8`/`$03B9` (SREG)
- **Ne écrivait jamais** `$03B0`/`$03B1`/`$03B2`/`$03B3`/`$03B5`/`$03B7`

Conséquence : à `loci_init`, `regs[0x10..0x1F]` valait `$00..$00` après
le `memset`. Un `JSR $03B0` du 6502 fetchait `0x00 0x00 0x00 0x00 ...`
= `BRK BRK BRK ...`. Le 6502 vectorise via `$FFFE/F` (vecteur IRQ) et
diverge.

Le premier fastcall du boot LOCI est `tap_tell()` à `main.c:1159` via
`update_tap_counter()` — AVANT `InitKeyboard()` (l.1186) et le `while(1)`
(l.1188). Donc :
1. `tap_tell` jamais retourné
2. `main()` bloqué
3. `InitKeyboard` jamais appelée → PSG R7 jamais programmée
4. `while(1)` jamais atteint → pas de spinner
5. Le clavier semble "ne pas marcher" alors qu'il scanne correctement

Le pre-seed R7 du Sprint 34am masquait le symptôme R7 mais laissait
l'ABI cassée. Le released stub étant inexistant, `main()` restait bloqué.

### Pourquoi les 105 tests existants passaient

Mes tests appelaient `loci_write(..., 0x03AF, op)` puis vérifiaient les
registres `regs[0x14]` (A), `regs[0x16]` (X). Ils **court-circuitaient
le `JSR $03B0`** en lisant directement les registres post-dispatch. Le
vrai 6502, lui, devait passer par la spin window — qui n'existait pas.

---

## 3. Patch livré (Option B)

### Nouvelles helpers

```c
static void api_install_blocked_stub(loci_t* loci) {
    loci->regs[0x10] = 0xB8;  // CLV
    loci->regs[0x11] = 0x50;  // BVC
    loci->regs[0x12] = 0xFE;  // -2 + BUSY=1
    loci->regs[0x13] = 0xA9;  // LDA #
}

static void api_install_released_stub(loci_t* loci) {
    loci->regs[0x10] = 0xB8;
    loci->regs[0x11] = 0x50;
    loci->regs[0x12] = 0x00;  // +0 + BUSY=0
    loci->regs[0x13] = 0xA9;
}
```

### api_set_ax étendu

Mirror byte-pour-byte du firmware `api.h:183-186` :

```c
static void api_set_ax(loci_t* loci, uint16_t val) {
    loci->regs[0x14] = val & 0xFF;          // A immediate
    loci->regs[0x15] = 0xA2;                 // LDX #
    loci->regs[0x16] = (val >> 8) & 0xFF;    // X immediate
    loci->regs[0x17] = 0x60;                 // RTS
}
```

### Dispatch $03AF refactorisé

```c
if (off == LOCI_REG_API_OP) {
    if (value == 0x00) {
        xstack_zero(loci);
        api_return_ax(loci, 0);    // → released stub
        return;
    }
    if (value == 0xFF) {
        api_install_blocked_stub(loci);  // 6502 spin permanent
        return;
    }
    api_install_blocked_stub(loci);
    dispatch_op(loci, value);       // handler → api_return_*
}
```

### Init/Reset

`loci_init` et `loci_reset` appellent maintenant `seed_initial_stub()`
qui pose un released no-op stub (A=X=SREG=0) — protège contre les
probes pré-op.

### Default ENOSYS

L'ancien fallback dans `dispatch_op` faisait `set_errno + set_busy(false)`
— mais ne posait pas le released stub. Remplacé par `api_return_errno`
qui passe par la chaîne complète.

---

## 4. Tests différentiels 6502

3 nouveaux tests qui exécutent un **vrai `cpu_step()`** sur un programme
6502 minimal et exercent l'ABI complète :

### `test_6502_initial_jsr_returns_zero`
Pré-op, vérifie que `JSR $03B0` revient cleanly avec A=X=0 grâce au
seed initial.

### `test_6502_jsr_spin_zxstack_op_00`
```asm
LDA #0
STA $03AF       ; trigger zxstack
JSR $03B0       ; spin → released stub
STA $0200       ; record A
STX $0201       ; record X
BRK             ; halt
```
Vérifie : xstack_ptr revient à 256, A=0, X=0.

### `test_6502_jsr_spin_returns_via_released_stub`
Op `RNG_LRAND` (0x04). Vérifie : retour 31-bit positif via AXSREG,
BUSY clear, PC à la sentinelle BRK ($041A).

### Plomberie Makefile

`TEST_LOCI_SRCS` link maintenant cpu6502 + memory + addressing + banking
pour permettre l'exécution réelle de 6502.

---

## 5. Validation utilisateur

Sessions interactives du 2026-06-07 ont confirmé :

| Test | Avant | Après |
|------|-------|-------|
| Boot ROM LOCI v0.3.0 | atteint $C354 (IRQ) | atteint while(1) |
| Spinner TUI visible | ❌ | ✅ |
| Lettres a/b/c/d/t/k/m/o | ❌ | ✅ |
| Flèches ↑↓←→ | ❌ | ✅ |
| Toggle widgets | ❌ | ✅ |
| ESC → fresh boot | ❌ | ✅ |
| MIA_BOOT op 0xA0 | ❌ | ✅ |
| ROM swap vers BASIC 1.0/1.1 | ❌ | ✅ |
| Scénario E2E complet | ❌ | ✅ |

**Phosphoric est officiellement le premier émulateur grand public à
supporter LOCI en bout-en-bout** : la ROM LOCI v0.3.0 boote, affiche
son TUI, accepte la navigation au clavier, exécute MIA_BOOT, et swappe
vers la ROM BASIC sélectionnée (1.0 / 1.1) en chargeant depuis le
sandbox `--loci-flash DIR`.

---

## 6. Sémantique UI LOCI (quirk découvert lors des tests)

Convention non-intuitive du LOCI ROM (loci-rom/src/main.c:901-938) :

| Touche | Action |
|--------|--------|
| **ESC** | Boot fresh selon config courante (`boot(false)`) |
| **Return** sur button `BOOT` | Boot fresh idem (`boot(false)`) |
| **Return** sur menu général | Resume save state (`boot(true)`) — no-op sans save state |

Ce n'est pas un bug Phosphoric, c'est le design choisi par sodiumlb.

---

## 7. Métriques finales

| Indicateur | Valeur |
|------------|--------|
| Tests test-loci | 108 (105 unitaires + 3 différentiels 6502) |
| Tests global Phosphoric | 448 |
| Régression sur autres modules | 0 |
| Ops API LOCI implémentées | 28/36 (78%) |
| Bug critique résolu | ABI MIA spin window |
| Validation E2E utilisateur | ✅ Confirmée |

---

## 8. Crédits

- **bmarty** (Phosphoric) : implémentation sprints 34y-34am + intégration patch
- **Ingé principal** : analyse cause racine ABI, suggestion Option B,
  contract firmware déchiffré
- **sodiumlb** : ROM LOCI v0.3.0 + firmware open-source (BSD-3-Clause)
- **rumbledethumps** : architecture RP6502 ancestrale dont LOCI hérite

---

## 9. Reproductibilité

```bash
git clone <repo> && cd Oric1
git checkout main
make clean && make SDL2=1
make tests   # 448 pass

# Preparer le sandbox
mkdir -p ~/loci-vfs
cp roms/basic1*.rom roms/microdis.rom ~/loci-vfs/

# Lancer LOCI ROM complet
./oric1-emu -r roms/loci/locirom --loci --loci-flash ~/loci-vfs

# Dans le TUI :
#   ↑↓←→ : navigation
#   b    : toggle BASIC 1.0/1.1
#   t    : toggle TAP
#   f    : toggle FDC
#   ESC  : boot fresh selon config
# → bascule dans BASIC 1.0/1.1 Atmos !
```

---

**Statut** : LOCI scénario E2E **validé**. La série de 14 sprints
(34y → 34an) est officiellement terminée. Reste comme polish optionnel :
WD1793 cycle-accurate, TAP bit-streamer $0317, diag ROM via `--loci-diag`.

— Fin du CR
