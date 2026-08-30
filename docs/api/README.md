# Reference API — Phosphoric v1.107.3-alpha

Derniere mise a jour : 2026-08-30

---

## Table des matieres

1. [Emulateur (emulator.h)](#emulateur)
2. [CPU 6502 (cpu6502.h)](#cpu-6502)
3. [Memoire (memory.h)](#memoire)
4. [VIA 6522 (via6522.h)](#via-6522)
5. [Clavier (keyboard.h)](#clavier)
6. [Joystick IJK (joystick.h)](#joystick-ijk)
7. [Video (video.h, export.h)](#video)
8. [Audio PSG (audio.h)](#audio-psg)
9. [Stockage TAP (tap.h)](#stockage-tap)
10. [Disque WD1793 (disk.h)](#disque-wd1793)
11. [Sedoric (sedoric.h)](#sedoric)
12. [Microdisc (microdisc.h)](#microdisc)
13. [Imprimante (printer.h)](#imprimante)
14. [Traceur MCP-40 (mcp40.h)](#traceur-mcp-40)
15. [Systeme de fichiers hote (hostfs.h)](#systeme-de-fichiers-hote)
16. [Debogueur (debugger.h)](#debogueur)
17. [Sauvegarde d'etat (savestate.h)](#sauvegarde-detat)
18. [Trace CPU (trace.h)](#trace-cpu)
19. [Profileur CPU (profiler.h)](#profileur-cpu)
20. [Analyse ROM (rominfo.h)](#analyse-rom)
21. [Journalisation (logging.h)](#journalisation)
22. [Cast / Streaming (cast_server.h)](#cast--streaming)

---

## Emulateur

**Fichier :** `include/emulator.h`

### Constantes

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `EMU_VERSION` | `"1.107.3-alpha"` | Version de l'emulateur |
| `ORIC_CLOCK_HZ` | 1000000 | Frequence CPU 1 MHz |
| `ORIC_FRAME_RATE` | 50 | Taux de rafraichissement PAL |
| `CYCLES_PER_FRAME` | 19968 | Cycles CPU par frame |
| `VSYNC_CYCLE` | 16384 | Cycle de debut VSync |

### Types

```c
typedef enum { ORIC_MODEL_ORIC1, ORIC_MODEL_ATMOS } oric_model_t;

typedef struct {
    uint16_t getsync_entry, getsync_end;
    uint16_t readbyte_entry, readbyte_end;
    uint16_t fast_load_addr;
} rom_patches_t;

typedef struct {
    cpu6502_t       cpu;
    memory_t        memory;
    via6522_t       via;
    ay3891x_t       psg;
    video_t         video;
    oric_keyboard_t keyboard;
    oric_joystick_t joystick;
    oric_printer_t  printer;
    microdisc_t     microdisc;
    debugger_t      debugger;
    cpu_trace_t     trace;
    cpu_profiler_t  profiler;
    /* ... champs internes (tape, disques, options) ... */
} emulator_t;
```

---

## CPU 6502

**Fichier :** `include/cpu/cpu6502.h`

### Constantes (drapeaux processeur)

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `FLAG_CARRY` | 0x01 | Retenue |
| `FLAG_ZERO` | 0x02 | Zero |
| `FLAG_INTERRUPT` | 0x04 | Masque IRQ |
| `FLAG_DECIMAL` | 0x08 | Mode BCD |
| `FLAG_BREAK` | 0x10 | Break |
| `FLAG_OVERFLOW` | 0x40 | Debordement |
| `FLAG_NEGATIVE` | 0x80 | Negatif |

### Types

```c
typedef enum { IRQF_VIA = 0x01, IRQF_DISK = 0x02 } cpu_irq_source_t;

typedef enum {
    ADDR_IMPLICIT, ADDR_ACCUMULATOR, ADDR_IMMEDIATE,
    ADDR_ZERO_PAGE, ADDR_ZERO_PAGE_X, ADDR_ZERO_PAGE_Y,
    ADDR_RELATIVE, ADDR_ABSOLUTE, ADDR_ABSOLUTE_X, ADDR_ABSOLUTE_Y,
    ADDR_INDIRECT, ADDR_INDEXED_INDIRECT, ADDR_INDIRECT_INDEXED
} addressing_mode_t;

typedef struct {
    uint8_t  A, X, Y, SP, P;
    uint16_t PC;
    uint64_t cycles;
    int      cycles_left;
    bool     halted, nmi_pending;
    uint8_t  irq;           /* bitfield (IRQF_VIA | IRQF_DISK) */
    memory_t* memory;
} cpu6502_t;
```

### Fonctions

| Signature | Description |
|-----------|-------------|
| `void cpu_init(cpu6502_t* cpu, memory_t* memory)` | Initialise le CPU et connecte la memoire |
| `void cpu_reset(cpu6502_t* cpu)` | Reset du CPU (lit vecteur RESET $FFFC) |
| `int cpu_step(cpu6502_t* cpu)` | Execute une instruction, retourne les cycles consommes |
| `int cpu_execute_cycles(cpu6502_t* cpu, int cycles)` | Execute N cycles, retourne les cycles reels |
| `void cpu_nmi(cpu6502_t* cpu)` | Declenche une NMI (front descendant) |
| `void cpu_irq_set(cpu6502_t* cpu, cpu_irq_source_t source)` | Active une source IRQ (level-triggered) |
| `void cpu_irq_clear(cpu6502_t* cpu, cpu_irq_source_t source)` | Desactive une source IRQ |
| `void cpu_set_flag(cpu6502_t* cpu, cpu_flags_t flag, bool value)` | Modifie un drapeau |
| `bool cpu_get_flag(const cpu6502_t* cpu, cpu_flags_t flag)` | Lit un drapeau |
| `int cpu_disassemble(const cpu6502_t* cpu, uint16_t addr, char* buf, size_t size)` | Desassemble a l'adresse, retourne taille instruction |
| `void cpu_get_state_string(const cpu6502_t* cpu, char* buf, size_t size)` | Etat CPU en chaine lisible |

### Table d'opcodes (cpu_internal.h)

```c
typedef struct {
    const char*      name;
    uint8_t          cycles;
    uint8_t          size;
    addressing_mode_t addressing_mode;
} opcode_info_t;

extern const opcode_info_t opcode_table[256];
int cpu_execute_opcode(cpu6502_t* cpu, uint8_t opcode);
```

---

## Memoire

**Fichier :** `include/memory/memory.h`

### Constantes

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `MEMORY_SIZE` | 65536 | Espace adressable 64 KB |
| `RAM_SIZE` | 49152 | RAM 48 KB ($0000-$BFFF) |
| `ROM_SIZE` | 16384 | ROM 16 KB ($C000-$FFFF) |

### Types

```c
typedef enum { MEM_READ, MEM_WRITE, MEM_EXEC } mem_access_type_t;
typedef enum { BANK_ROM, BANK_RAM } memory_bank_t;

typedef struct {
    uint8_t ram[RAM_SIZE];
    uint8_t rom[ROM_SIZE];
    uint8_t charset[2048];
    uint8_t upper_ram[ROM_SIZE];
    bool    rom_enabled, overlay_active, basic_rom_disabled;
    uint8_t charset_bank;
    /* callbacks I/O, trace */
} memory_t;
```

### Fonctions

| Signature | Description |
|-----------|-------------|
| `bool memory_init(memory_t* mem)` | Initialise la memoire (RAM pattern Oricutron) |
| `void memory_cleanup(memory_t* mem)` | Libere les ressources |
| `bool memory_load_rom(memory_t* mem, const char* filename, uint16_t offset)` | Charge un fichier ROM a l'offset |
| `bool memory_load_charset(memory_t* mem, const char* filename)` | Charge un charset ROM |
| `uint8_t memory_read(memory_t* mem, uint16_t address)` | Lecture avec banking + I/O routing |
| `void memory_write(memory_t* mem, uint16_t address, uint8_t value)` | Ecriture avec banking + I/O routing |
| `uint16_t memory_read_word(memory_t* mem, uint16_t address)` | Lecture mot 16-bit (little-endian) |
| `void memory_write_word(memory_t* mem, uint16_t address, uint16_t value)` | Ecriture mot 16-bit |
| `void memory_set_io_callbacks(memory_t* mem, ...)` | Configure les callbacks I/O ($0300-$031F) |
| `void memory_set_trace(memory_t* mem, bool enabled, ...)` | Active le tracage memoire |
| `void memory_clear_ram(memory_t* mem, uint8_t pattern)` | Remplit la RAM avec un motif |
| `uint8_t* memory_get_ptr(memory_t* mem, uint16_t address)` | Pointeur direct (bypass banking) |

---

## VIA 6522

**Fichier :** `include/io/via6522.h`

### Registres (offsets depuis $0300)

| Constante | Offset | Description |
|-----------|--------|-------------|
| `VIA_ORB` | 0x00 | Port B sortie |
| `VIA_ORA` | 0x01 | Port A sortie |
| `VIA_DDRB/DDRA` | 0x02-0x03 | Direction des ports |
| `VIA_T1CL/T1CH` | 0x04-0x05 | Timer 1 compteur |
| `VIA_T1LL/T1LH` | 0x06-0x07 | Timer 1 latch |
| `VIA_T2CL/T2CH` | 0x08-0x09 | Timer 2 compteur |
| `VIA_SR` | 0x0A | Shift Register |
| `VIA_ACR` | 0x0B | Auxiliary Control |
| `VIA_PCR` | 0x0C | Peripheral Control |
| `VIA_IFR` | 0x0D | Interrupt Flag |
| `VIA_IER` | 0x0E | Interrupt Enable |

### Drapeaux d'interruption

`VIA_INT_CA2` (0x01), `VIA_INT_CA1` (0x02), `VIA_INT_SR` (0x04), `VIA_INT_CB2` (0x08), `VIA_INT_CB1` (0x10), `VIA_INT_T2` (0x20), `VIA_INT_T1` (0x40)

### Fonctions

| Signature | Description |
|-----------|-------------|
| `void via_init(via6522_t* via)` | Initialise le VIA |
| `void via_reset(via6522_t* via)` | Reset du VIA |
| `uint8_t via_read(via6522_t* via, uint8_t reg)` | Lecture registre (0x00-0x0F) |
| `void via_write(via6522_t* via, uint8_t reg, uint8_t value)` | Ecriture registre |
| `void via_update(via6522_t* via, int cycles)` | Mise a jour timers (appeler chaque cycle CPU) |
| `void via_set_port_callbacks(via6522_t* via, ...)` | Configure les callbacks ports A/B |
| `void via_set_irq_callback(via6522_t* via, ...)` | Configure le callback IRQ |
| `void via_trigger_ca1(via6522_t* via)` | Declenche interruption CA1 |
| `void via_trigger_ca2(via6522_t* via)` | Declenche interruption CA2 |
| `void via_trigger_cb1(via6522_t* via)` | Declenche interruption CB1 |
| `void via_trigger_cb2(via6522_t* via)` | Declenche interruption CB2 |
| `void via_set_cb1(via6522_t* via, bool state)` | Change le niveau CB1 (detection de front) |

---

## Clavier

**Fichier :** `include/io/keyboard.h`

### Fonctions

| Signature | Description |
|-----------|-------------|
| `void oric_keyboard_init(oric_keyboard_t* kb)` | Initialise le clavier |
| `void oric_keyboard_reset(oric_keyboard_t* kb)` | Reset du clavier |
| `void oric_keyboard_set_layout(oric_keyboard_t* kb, oric_kb_layout_t layout)` | QWERTY ou AZERTY |
| `bool oric_keyboard_press_char(oric_keyboard_t* kb, char c)` | Simule une touche ASCII |
| `void oric_keyboard_release_all(oric_keyboard_t* kb)` | Relache toutes les touches |
| `bool oric_keyboard_handle_sdl_event(oric_keyboard_t* kb, const SDL_Event* event)` | Traite un evenement SDL2 |

---

## Joystick IJK

**Fichier :** `include/io/joystick.h`

### Constantes (bits actifs bas)

`IJK_LEFT` (bit 0), `IJK_RIGHT` (bit 1), `IJK_DOWN` (bit 3), `IJK_UP` (bit 4), `IJK_FIRE` (bit 5)

### Fonctions

| Signature | Description |
|-----------|-------------|
| `void oric_joystick_init(oric_joystick_t* joy)` | Initialise le joystick |
| `void oric_joystick_reset(oric_joystick_t* joy)` | Reset |
| `void oric_joystick_set_mode(oric_joystick_t* joy, oric_joy_mode_t mode)` | Mode DISABLED/SDL_GAMEPAD/KEYBOARD |
| `void oric_joystick_press(oric_joystick_t* joy, uint8_t mask)` | Appui direction/fire |
| `void oric_joystick_release(oric_joystick_t* joy, uint8_t mask)` | Relachement |
| `void oric_joystick_release_all(oric_joystick_t* joy)` | Relache tout |
| `uint8_t oric_joystick_read(const oric_joystick_t* joy)` | Lit l'etat Port A (actif bas) |
| `bool oric_joystick_open_sdl(oric_joystick_t* joy, int index)` | Ouvre une manette SDL2 |
| `void oric_joystick_close_sdl(oric_joystick_t* joy)` | Ferme la manette SDL2 |
| `bool oric_joystick_handle_sdl_event(oric_joystick_t* joy, const SDL_Event* ev)` | Traite un evenement SDL2 |

---

## Video

**Fichiers :** `include/video/video.h`, `include/video/export.h`

### Constantes

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `ORIC_SCREEN_W` | 240 | Largeur en pixels |
| `ORIC_SCREEN_H` | 224 | Hauteur en pixels |
| `ORIC_TEXT_COLS` | 40 | Colonnes mode texte |
| `ORIC_TEXT_ROWS` | 28 | Lignes mode texte |
| `ORIC_HIRES_W` | 240 | Largeur HIRES |
| `ORIC_HIRES_H` | 200 | Hauteur HIRES |

### Couleurs ORIC

`ORIC_BLACK` (0), `ORIC_RED` (1), `ORIC_GREEN` (2), `ORIC_YELLOW` (3), `ORIC_BLUE` (4), `ORIC_MAGENTA` (5), `ORIC_CYAN` (6), `ORIC_WHITE` (7)

### Fonctions Video

| Signature | Description |
|-----------|-------------|
| `bool video_init(video_t* vid)` | Initialise le sous-systeme video |
| `void video_cleanup(video_t* vid)` | Libere les ressources |
| `void video_reset(video_t* vid)` | Reset video |
| `void video_set_mode(video_t* vid, bool hires)` | Bascule texte/HIRES |
| `void video_render_frame(video_t* vid, const uint8_t* memory)` | Rend une frame dans le framebuffer |
| `void video_get_rgb(uint8_t oric_color, uint8_t* r, uint8_t* g, uint8_t* b)` | Convertit couleur ORIC vers RGB |

### Fonctions Export

| Signature | Description |
|-----------|-------------|
| `bool video_export_ppm(const video_t* vid, const char* filename)` | Export PPM (P6 binaire) |
| `bool video_export_bmp(const video_t* vid, const char* filename)` | Export BMP 24-bit |
| `bool video_export_ascii(const video_t* vid, FILE* fp, unsigned int sx, unsigned int sy)` | Export texte ANSI |
| `bool video_export_auto(const video_t* vid, const char* filename)` | Detection auto par extension |

---

## Audio PSG

**Fichier :** `include/audio/audio.h`

### Constantes

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `AY_NUM_CHANNELS` | 3 | Canaux tonaux |
| `AY_NUM_REGISTERS` | 16 | Registres PSG |
| `AUDIO_SAMPLE_RATE` | 44100 | Frequence d'echantillonnage |
| `AUDIO_BUFFER_SIZE` | 2048 | Taille buffer audio |

### Fonctions

| Signature | Description |
|-----------|-------------|
| `void ay_init(ay3891x_t* ay, uint32_t clock_rate)` | Initialise le PSG avec frequence d'horloge |
| `void ay_reset(ay3891x_t* ay)` | Reset du PSG |
| `void ay_write_address(ay3891x_t* ay, uint8_t addr)` | Selectionne un registre (0-15) |
| `void ay_write_data(ay3891x_t* ay, uint8_t data)` | Ecrit dans le registre selectionne |
| `uint8_t ay_read_data(ay3891x_t* ay)` | Lit le registre selectionne |
| `void ay_generate(ay3891x_t* ay, int16_t* buffer, int num_samples)` | Genere des echantillons audio |
| `bool audio_init(ay3891x_t* psg)` | Initialise la sortie audio SDL2 |
| `void audio_cleanup(void)` | Ferme la sortie audio |
| `void audio_pause(bool pause)` | Pause/reprise audio |
| `void audio_set_cast_server(cast_server_t* server)` | Connecte le Cast pour streaming |

---

## Stockage TAP

**Fichier :** `include/storage/tap.h`

### Constantes

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `TAP_SYNC_BYTE` | 0x16 | Octet de synchronisation |
| `TAP_MARKER` | 0x24 | Marqueur debut header |
| `TAP_NAME_LEN` | 16 | Longueur max nom programme |

### Types

```c
typedef enum { TAP_BASIC = 0x00, TAP_MACHINE = 0x80, TAP_SCREEN = 0xC0 } tap_program_type_t;
```

### Fonctions

| Signature | Description |
|-----------|-------------|
| `tap_file_t* tap_open_read(const char* filename, bool fast_load)` | Ouvre un .TAP en lecture |
| `tap_file_t* tap_open_write(const char* filename)` | Ouvre un .TAP en ecriture |
| `void tap_close(tap_file_t* tap)` | Ferme le fichier |
| `bool tap_read_header(tap_file_t* tap, tap_header_t* header)` | Lit l'en-tete TAP |
| `bool tap_write_header(tap_file_t* tap, const tap_header_t* header)` | Ecrit l'en-tete TAP |
| `int tap_read_data(tap_file_t* tap, uint8_t* buffer, size_t size)` | Lit un bloc de donnees |
| `bool tap_write_data(tap_file_t* tap, const uint8_t* data, size_t size)` | Ecrit un bloc de donnees |
| `void tap_rewind(tap_file_t* tap)` | Rembobine au debut |
| `uint32_t tap_tell(const tap_file_t* tap)` | Position courante |
| `uint32_t tap_size(const tap_file_t* tap)` | Taille totale |
| `bool tap_eof(const tap_file_t* tap)` | Test fin de fichier |
| `uint8_t tap_checksum(const uint8_t* data, size_t size)` | Calcul checksum |
| `bool tap_from_basic(const char* basic, const char* tap, bool auto_run)` | Convertit BASIC → TAP |
| `bool tap_from_binary(const char* bin, const char* tap, uint16_t start, uint16_t exec, const char* name)` | Convertit binaire → TAP |

---

## Disque WD1793

**Fichier :** `include/storage/disk.h`

### Registres FDC

| Constante | Offset | Description |
|-----------|--------|-------------|
| `FDC_STATUS/COMMAND` | 0 | Status (lecture) / Commande (ecriture) |
| `FDC_TRACK` | 1 | Registre piste |
| `FDC_SECTOR` | 2 | Registre secteur |
| `FDC_DATA` | 3 | Registre donnees |

### Fonctions

| Signature | Description |
|-----------|-------------|
| `void fdc_init(fdc_t* fdc)` | Initialise le FDC |
| `void fdc_reset(fdc_t* fdc)` | Reset du FDC |
| `void fdc_set_disk(fdc_t* fdc, uint8_t* data, uint32_t size)` | Connecte une image disque |
| `uint8_t fdc_read(fdc_t* fdc, uint8_t reg)` | Lit un registre FDC |
| `void fdc_write(fdc_t* fdc, uint8_t reg, uint8_t value)` | Ecrit un registre FDC |
| `void fdc_ticktock(fdc_t* fdc, unsigned int cycles)` | Avance le timing (appeler chaque cycle) |

---

## Sedoric

**Fichier :** `include/storage/sedoric.h`

### Constantes

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `SEDORIC_SECTOR_SIZE` | 256 | Taille secteur |
| `SEDORIC_TRACKS` | 42 | Pistes par disque |
| `SEDORIC_SECTORS` | 17 | Secteurs par piste |

### Fonctions

| Signature | Description |
|-----------|-------------|
| `sedoric_disk_t* sedoric_create(void)` | Cree un disque vierge |
| `sedoric_disk_t* sedoric_load(const char* filename)` | Charge un .DSK |
| `bool sedoric_save(sedoric_disk_t* disk, const char* filename)` | Sauvegarde un .DSK |
| `void sedoric_destroy(sedoric_disk_t* disk)` | Libere un disque |
| `uint8_t* sedoric_get_sector(sedoric_disk_t* disk, uint8_t track, uint8_t sector)` | Pointeur vers secteur |
| `bool sedoric_read_sector(const sedoric_disk_t* disk, uint8_t t, uint8_t s, uint8_t* buf)` | Lit un secteur |
| `bool sedoric_write_sector(sedoric_disk_t* disk, uint8_t t, uint8_t s, const uint8_t* buf)` | Ecrit un secteur |

---

## Microdisc

**Fichier :** `include/io/microdisc.h`

### Adresses I/O

| Constante | Adresse | Description |
|-----------|---------|-------------|
| `MICRODISC_FDC_BASE` | $0310 | WD1793 registres ($0310-$0313) |
| `MICRODISC_CTRL` | $0314 | Controle / statut IRQ |
| `MICRODISC_DRQ` | $0318 | Statut DRQ |

### Bits de controle

`MICRODISC_CTRL_INTENA` (0x01), `MICRODISC_CTRL_ROMDIS` (0x02), `MICRODISC_CTRL_DENSITY` (0x08), `MICRODISC_CTRL_SIDE` (0x10), `MICRODISC_CTRL_DRIVE` (0x60), `MICRODISC_CTRL_EPROM` (0x80)

### Fonctions

| Signature | Description |
|-----------|-------------|
| `void microdisc_init(microdisc_t* md)` | Initialise le Microdisc |
| `void microdisc_reset(microdisc_t* md)` | Reset du Microdisc |
| `uint8_t microdisc_read(microdisc_t* md, uint16_t addr)` | Lecture I/O |
| `void microdisc_write(microdisc_t* md, uint16_t addr, uint8_t value)` | Ecriture I/O |
| `void microdisc_set_disk(microdisc_t* md, uint8_t drive, uint8_t* data, uint32_t size, uint8_t tracks, uint8_t spt)` | Connecte un disque au lecteur |
| `bool microdisc_load_rom(microdisc_t* md, const char* filename)` | Charge la ROM overlay |
| `void microdisc_cleanup(microdisc_t* md)` | Libere les ressources |

---

## Imprimante

**Fichier :** `include/io/printer.h`

### Types

```c
typedef enum { PRINTER_NONE, PRINTER_TEXT, PRINTER_MCP40 } oric_printer_type_t;
```

### Fonctions

| Signature | Description |
|-----------|-------------|
| `void oric_printer_init(oric_printer_t* printer)` | Initialise l'imprimante |
| `bool oric_printer_open(oric_printer_t* printer, const char* filename)` | Ouvre le fichier de sortie |
| `void oric_printer_close(oric_printer_t* printer)` | Ferme la sortie |
| `void oric_printer_check_strobe(oric_printer_t* p, uint8_t old_pcr, uint8_t new_pcr, uint8_t data)` | Detecte le signal STROBE (CA2) |
| `void oric_printer_flush(oric_printer_t* printer)` | Vide le buffer de sortie |
| `bool oric_printer_is_active(const oric_printer_t* printer)` | Verifie si l'imprimante est active |

---

## Traceur MCP-40

**Fichier :** `include/io/mcp40.h`

### Constantes

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `MCP40_WIDTH` | 480 | Largeur zone de tracage |
| `MCP40_HEIGHT` | 400 | Hauteur zone de tracage |

### Couleurs

`MCP40_BLACK` (0), `MCP40_BLUE` (1), `MCP40_GREEN` (2), `MCP40_RED` (3)

### Fonctions

| Signature | Description |
|-----------|-------------|
| `void mcp40_init(mcp40_t* mcp)` | Initialise le traceur |
| `void mcp40_reset(mcp40_t* mcp)` | Efface le papier |
| `void mcp40_receive_byte(mcp40_t* mcp, uint8_t byte)` | Recoit un octet Centronics |
| `bool mcp40_export_bmp(const mcp40_t* mcp, const char* filename)` | Exporte en BMP |
| `void mcp40_set_output(mcp40_t* mcp, const char* filename)` | Configure l'export automatique |
| `void mcp40_get_pen_rgb(mcp40_color_t color, uint8_t* r, uint8_t* g, uint8_t* b)` | Couleur RGB d'un stylo |

---

## Systeme de fichiers hote

**Fichier :** `include/hostfs/hostfs.h`

### Constantes

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `HOSTFS_MAX_PATH` | 256 | Longueur max chemin |
| `HOSTFS_MAX_HANDLES` | 8 | Fichiers ouverts simultanes |

### Fonctions

| Signature | Description |
|-----------|-------------|
| `bool hostfs_init(hostfs_t* hfs)` | Initialise le HostFS |
| `void hostfs_cleanup(hostfs_t* hfs)` | Libere les ressources |
| `bool hostfs_mount(hostfs_t* hfs, const char* path, bool read_only)` | Monte un repertoire |
| `void hostfs_unmount(hostfs_t* hfs)` | Demonte |
| `bool hostfs_is_mounted(const hostfs_t* hfs)` | Verifie si monte |
| `int hostfs_open(hostfs_t* hfs, const char* name, bool writing)` | Ouvre un fichier (retourne handle 0-7) |
| `bool hostfs_close(hostfs_t* hfs, int handle)` | Ferme un fichier |
| `int hostfs_read(hostfs_t* hfs, int handle, uint8_t* buf, size_t size)` | Lit des donnees |
| `int hostfs_write(hostfs_t* hfs, int handle, const uint8_t* buf, size_t size)` | Ecrit des donnees |
| `bool hostfs_seek(hostfs_t* hfs, int handle, uint32_t position)` | Positionne le curseur |
| `uint32_t hostfs_size(hostfs_t* hfs, int handle)` | Taille du fichier |
| `int hostfs_list(hostfs_t* hfs, char* buf, size_t buf_size)` | Liste les fichiers |
| `bool hostfs_delete(hostfs_t* hfs, const char* name)` | Supprime un fichier |
| `bool hostfs_rename(hostfs_t* hfs, const char* old, const char* new)` | Renomme un fichier |
| `bool hostfs_oric_to_host_path(const hostfs_t* hfs, const char* oric, char* host, size_t size)` | Chemin ORIC → hote |
| `bool hostfs_host_to_oric_name(const char* host, char* oric)` | Nom hote → ORIC |

---

## Debogueur

**Fichier :** `include/debugger.h`

### Constantes

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `DEBUGGER_MAX_BREAKPOINTS` | 16 | Points d'arret maximum |
| `DEBUGGER_MAX_WATCHPOINTS` | 8 | Points de surveillance maximum |

### Fonctions

| Signature | Description |
|-----------|-------------|
| `void debugger_init(debugger_t* dbg)` | Initialise le debogueur |
| `bool debugger_should_break(debugger_t* dbg, emulator_t* emu)` | Verifie si on doit s'arreter |
| `void debugger_repl(debugger_t* dbg, emulator_t* emu)` | Lance la boucle REPL interactive |
| `int debugger_add_breakpoint(debugger_t* dbg, uint16_t addr)` | Ajoute un breakpoint (retourne index) |
| `bool debugger_remove_breakpoint(debugger_t* dbg, int index)` | Supprime un breakpoint |
| `int debugger_add_watchpoint(debugger_t* dbg, uint16_t addr)` | Ajoute un watchpoint (retourne index) |
| `bool debugger_remove_watchpoint(debugger_t* dbg, int index)` | Supprime un watchpoint |
| `bool debugger_is_breakpoint(const debugger_t* dbg, uint16_t pc)` | Verifie si PC est un breakpoint |
| `void debugger_install_watchpoint_trace(debugger_t* dbg, emulator_t* emu)` | Installe le tracage watchpoints |

---

## Sauvegarde d'etat

**Fichier :** `include/savestate.h`

### Constantes

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `SAVESTATE_MAGIC` | `"OST1"` | Signature fichier |
| `SAVESTATE_VERSION` | 1 | Version du format |
| `SAVESTATE_HEADER_SIZE` | 48 | Taille en-tete |

### Fonctions

| Signature | Description |
|-----------|-------------|
| `bool savestate_save(const emulator_t* emu, const char* filename)` | Sauvegarde l'etat complet avec CRC32 |
| `bool savestate_load(emulator_t* emu, const char* filename)` | Restaure l'etat depuis un fichier .ost |

---

## Trace CPU

**Fichier :** `include/utils/trace.h`

### Format de sortie

```
CCCCCCCC  AAAA  XX XX XX  MNEMONIC OPERAND       A=XX X=XX Y=XX SP=XX P=XX
```

### Fonctions

| Signature | Description |
|-----------|-------------|
| `void trace_init(cpu_trace_t* trace)` | Initialise la trace (inactive) |
| `bool trace_open(cpu_trace_t* trace, const char* filename)` | Ouvre un fichier de trace |
| `void trace_attach(cpu_trace_t* trace, FILE* fp)` | Attache un FILE* existant |
| `void trace_log_instruction(cpu_trace_t* trace, const cpu6502_t* cpu)` | Enregistre une instruction (avant cpu_step) |
| `void trace_close(cpu_trace_t* trace)` | Ferme la trace |
| `void trace_set_max(cpu_trace_t* trace, uint64_t max)` | Limite le nombre d'instructions tracees |

---

## Profileur CPU

**Fichier :** `include/utils/profiler.h`

### Constantes

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `PROFILER_ADDR_SPACE` | 65536 | Couverture 64K adresses |
| `PROFILER_OPCODE_COUNT` | 256 | 256 opcodes possibles |

### Fonctions

| Signature | Description |
|-----------|-------------|
| `void profiler_init(cpu_profiler_t* prof)` | Initialise le profileur (inactif) |
| `void profiler_start(cpu_profiler_t* prof)` | Active le profilage |
| `void profiler_stop(cpu_profiler_t* prof)` | Desactive le profilage |
| `void profiler_record_instruction(cpu_profiler_t* prof, const cpu6502_t* cpu)` | Enregistre un hit (avant cpu_step) |
| `void profiler_record_cycles(cpu_profiler_t* prof, uint16_t pc, int cycles)` | Enregistre le cout en cycles |
| `void profiler_reset(cpu_profiler_t* prof)` | Remet les compteurs a zero |
| `void profiler_report(const cpu_profiler_t* prof, FILE* fp)` | Ecrit le rapport sur FILE* |
| `bool profiler_report_to_file(const cpu_profiler_t* prof, const char* filename)` | Ecrit le rapport dans un fichier |

---

## Analyse ROM

**Fichier :** `include/utils/rominfo.h`

### Constantes

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `ROM_BASE_ADDR` | 0xC000 | Adresse de base ROM |
| `ROMINFO_MAX_TARGETS` | 512 | Cibles JSR/JMP max |
| `ROMINFO_MAX_STRINGS` | 256 | Chaines detectees max |
| `ROMINFO_MIN_STRING_LEN` | 4 | Longueur minimale chaine |

### Fonctions

| Signature | Description |
|-----------|-------------|
| `void rominfo_init(rom_analysis_t* analysis)` | Initialise l'analyse |
| `bool rominfo_analyze(rom_analysis_t* a, const uint8_t* rom, size_t size)` | Analyse complete d'une ROM |
| `void rominfo_report(const rom_analysis_t* a, const uint8_t* rom, size_t size, FILE* fp)` | Rapport sur FILE* |
| `bool rominfo_report_to_file(const rom_analysis_t* a, const uint8_t* rom, size_t size, const char* file)` | Rapport dans un fichier |
| `int rominfo_find_pattern(const uint8_t* rom, size_t size, const uint8_t* pat, size_t plen, uint16_t* results, int max)` | Recherche de motif binaire |

---

## Journalisation

**Fichier :** `include/utils/logging.h`

### Niveaux

```c
typedef enum { LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, LOG_LEVEL_WARNING, LOG_LEVEL_ERROR } log_level_t;
```

### Fonctions

| Signature | Description |
|-----------|-------------|
| `void log_init(log_level_t level)` | Initialise le systeme de log |
| `void log_cleanup(void)` | Ferme le systeme de log |
| `void log_debug(const char* format, ...)` | Message debug |
| `void log_info(const char* format, ...)` | Message info |
| `void log_warning(const char* format, ...)` | Avertissement |
| `void log_error(const char* format, ...)` | Erreur |

---

## Cast / Streaming

**Fichier :** `include/network/cast_server.h`

> Disponible uniquement avec `HAS_CAST` (compilation avec `-DHAS_CAST`, necessite pthread + OpenSSL).

### Constantes MJPEG

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `CAST_FRAME_W` | 720 | Largeur upscalee (240 x 3) |
| `CAST_FRAME_H` | 672 | Hauteur upscalee (224 x 3) |
| `CAST_DEFAULT_PORT` | 8080 | Port HTTP par defaut |
| `CAST_MAX_CLIENTS` | 8 | Clients MJPEG simultanes |
| `CAST_JPEG_QUALITY` | 80 | Qualite JPEG |

### Constantes Audio

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `CAST_AUDIO_RATE` | 44100 | Frequence audio WAV |
| `CAST_AUDIO_RING_SAMPLES` | 88200 | Buffer annulaire (2 secondes) |
| `CAST_MAX_AUDIO_CLIENTS` | 4 | Clients audio simultanes |

### Constantes CASTV2

| Constante | Valeur | Description |
|-----------|--------|-------------|
| `CASTV2_PORT` | 8009 | Port natif Google Cast |
| `CASTV2_HEARTBEAT_SEC` | 5 | Intervalle heartbeat |
| `CASTV2_DASHCAST_APPID` | `"5C3F0A3C"` | App ID DashCast |

### Fonctions serveur MJPEG

| Signature | Description |
|-----------|-------------|
| `bool cast_server_init(cast_server_t* server, uint16_t port)` | Demarre le serveur HTTP |
| `void cast_server_push_frame(cast_server_t* server, const uint8_t* fb, unsigned int w, unsigned int h)` | Pousse une frame video |
| `void cast_server_push_audio(cast_server_t* server, const int16_t* samples, size_t n)` | Pousse des echantillons audio |
| `void cast_server_stop(cast_server_t* server)` | Arrete le serveur |
| `int cast_server_discover_devices(int timeout_ms)` | Decouvre les Chromecast via mDNS |
| `void cast_server_upscale_nearest(const uint8_t* src, int sw, int sh, uint8_t* dst, int factor)` | Upscale nearest-neighbor |
| `int cast_server_build_mdns_query(uint8_t* buf, size_t size)` | Construit une requete mDNS |
| `int cast_server_build_wav_header(uint8_t* buf, size_t size)` | Construit un en-tete WAV |

### Fonctions client CASTV2

| Signature | Description |
|-----------|-------------|
| `bool castv2_discover_device(char* ip_out, const char* name, int timeout_ms)` | Decouvre un appareil |
| `bool castv2_connect_and_cast(castv2_client_t* client, const char* ip, const char* url)` | Connecte et caste une URL |
| `void castv2_disconnect(castv2_client_t* client)` | Deconnecte |
| `bool castv2_get_local_ip(char* ip_out)` | Obtient l'IP locale |
| `int castv2_encode_varint(uint8_t* buf, int size, uint64_t value)` | Encode un varint protobuf |
| `int castv2_decode_varint(const uint8_t* buf, int size, uint64_t* value)` | Decode un varint protobuf |
| `int castv2_build_message(uint8_t* buf, int size, const char* src, const char* dst, const char* ns, const char* payload)` | Construit un message CASTV2 |

---

## Statistiques

| Metrique | Valeur |
|----------|--------|
| Fichiers d'en-tete | 24 |
| Fonctions publiques | ~200 |
| Structures publiques | ~40 |
| Enumerations | ~30 |
| Modules | 8 (CPU, Memoire, I/O, Video, Audio, Stockage, Debug, Reseau) |
