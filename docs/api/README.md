# API Reference — Phosphoric v1.110.0-alpha

Last updated: 2026-08-30

---

## Table of contents

1. [Emulator (emulator.h)](#emulator)
2. [6502 CPU (cpu6502.h)](#6502-cpu)
3. [Memory (memory.h)](#memory)
4. [VIA 6522 (via6522.h)](#via-6522)
5. [Keyboard (keyboard.h)](#keyboard)
6. [IJK joystick (joystick.h)](#ijk-joystick)
7. [Video (video.h, export.h)](#video)
8. [PSG audio (audio.h)](#psg-audio)
9. [TAP storage (tap.h)](#tap-storage)
10. [WD1793 disk (disk.h)](#wd1793-disk)
11. [Sedoric (sedoric.h)](#sedoric)
12. [Microdisc (microdisc.h)](#microdisc)
13. [Printer (printer.h)](#printer)
14. [MCP-40 plotter (mcp40.h)](#mcp-40-plotter)
15. [Host file system (hostfs.h)](#host-file-system)
16. [Debugger (debugger.h)](#debugger)
17. [Save states (savestate.h)](#save-states)
18. [CPU trace (trace.h)](#cpu-trace)
19. [CPU profiler (profiler.h)](#cpu-profiler)
20. [ROM analysis (rominfo.h)](#rom-analysis)
21. [Logging (logging.h)](#logging)
22. [Cast / Streaming (cast_server.h)](#cast--streaming)

---

## Emulator

**File:** `include/emulator.h`

### Constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `EMU_VERSION` | `"1.110.0-alpha"` | Emulator version |
| `ORIC_CLOCK_HZ` | 1000000 | 1 MHz CPU frequency |
| `ORIC_FRAME_RATE` | 50 | PAL refresh rate |
| `CYCLES_PER_FRAME` | 19968 | CPU cycles per frame |
| `VSYNC_CYCLE` | 16384 | VSync start cycle |

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
    /* ... internal fields (tape, disks, options) ... */
} emulator_t;
```

---

## 6502 CPU

**File:** `include/cpu/cpu6502.h`

### Constants (processor flags)

| Constant | Value | Description |
|-----------|--------|-------------|
| `FLAG_CARRY` | 0x01 | Carry |
| `FLAG_ZERO` | 0x02 | Zero |
| `FLAG_INTERRUPT` | 0x04 | IRQ mask |
| `FLAG_DECIMAL` | 0x08 | BCD mode |
| `FLAG_BREAK` | 0x10 | Break |
| `FLAG_OVERFLOW` | 0x40 | Overflow |
| `FLAG_NEGATIVE` | 0x80 | Negative |

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

### Functions

| Signature | Description |
|-----------|-------------|
| `void cpu_init(cpu6502_t* cpu, memory_t* memory)` | Initialises the CPU and connects the memory |
| `void cpu_reset(cpu6502_t* cpu)` | CPU reset (reads the RESET vector $FFFC) |
| `int cpu_step(cpu6502_t* cpu)` | Executes one instruction, returns the cycles consumed |
| `int cpu_execute_cycles(cpu6502_t* cpu, int cycles)` | Executes N cycles, returns the actual cycles |
| `void cpu_nmi(cpu6502_t* cpu)` | Triggers an NMI (falling edge) |
| `void cpu_irq_set(cpu6502_t* cpu, cpu_irq_source_t source)` | Asserts an IRQ source (level-triggered) |
| `void cpu_irq_clear(cpu6502_t* cpu, cpu_irq_source_t source)` | Releases an IRQ source |
| `void cpu_set_flag(cpu6502_t* cpu, cpu_flags_t flag, bool value)` | Modifies a flag |
| `bool cpu_get_flag(const cpu6502_t* cpu, cpu_flags_t flag)` | Reads a flag |
| `int cpu_disassemble(const cpu6502_t* cpu, uint16_t addr, char* buf, size_t size)` | Disassembles at the address, returns the instruction size |
| `void cpu_get_state_string(const cpu6502_t* cpu, char* buf, size_t size)` | CPU state as a readable string |

### Opcode table (cpu_internal.h)

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

## Memory

**File:** `include/memory/memory.h`

### Constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `MEMORY_SIZE` | 65536 | 64 KB address space |
| `RAM_SIZE` | 49152 | 48 KB RAM ($0000-$BFFF) |
| `ROM_SIZE` | 16384 | 16 KB ROM ($C000-$FFFF) |

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
    /* I/O callbacks, trace */
} memory_t;
```

### Functions

| Signature | Description |
|-----------|-------------|
| `bool memory_init(memory_t* mem)` | Initialises memory (Oricutron RAM pattern) |
| `void memory_cleanup(memory_t* mem)` | Frees resources |
| `bool memory_load_rom(memory_t* mem, const char* filename, uint16_t offset)` | Loads a ROM file at the offset |
| `bool memory_load_charset(memory_t* mem, const char* filename)` | Loads a charset ROM |
| `uint8_t memory_read(memory_t* mem, uint16_t address)` | Read with banking + I/O routing |
| `void memory_write(memory_t* mem, uint16_t address, uint8_t value)` | Write with banking + I/O routing |
| `uint16_t memory_read_word(memory_t* mem, uint16_t address)` | 16-bit word read (little-endian) |
| `void memory_write_word(memory_t* mem, uint16_t address, uint16_t value)` | 16-bit word write |
| `void memory_set_io_callbacks(memory_t* mem, ...)` | Configures the I/O callbacks ($0300-$031F) |
| `void memory_set_trace(memory_t* mem, bool enabled, ...)` | Enables memory tracing |
| `void memory_clear_ram(memory_t* mem, uint8_t pattern)` | Fills RAM with a pattern |
| `uint8_t* memory_get_ptr(memory_t* mem, uint16_t address)` | Direct pointer (bypasses banking) |

---

## VIA 6522

**File:** `include/io/via6522.h`

### Registers (offsets from $0300)

| Constant | Offset | Description |
|-----------|--------|-------------|
| `VIA_ORB` | 0x00 | Port B output |
| `VIA_ORA` | 0x01 | Port A output |
| `VIA_DDRB/DDRA` | 0x02-0x03 | Port directions |
| `VIA_T1CL/T1CH` | 0x04-0x05 | Timer 1 counter |
| `VIA_T1LL/T1LH` | 0x06-0x07 | Timer 1 latch |
| `VIA_T2CL/T2CH` | 0x08-0x09 | Timer 2 counter |
| `VIA_SR` | 0x0A | Shift Register |
| `VIA_ACR` | 0x0B | Auxiliary Control |
| `VIA_PCR` | 0x0C | Peripheral Control |
| `VIA_IFR` | 0x0D | Interrupt Flag |
| `VIA_IER` | 0x0E | Interrupt Enable |

### Interrupt flags

`VIA_INT_CA2` (0x01), `VIA_INT_CA1` (0x02), `VIA_INT_SR` (0x04), `VIA_INT_CB2` (0x08), `VIA_INT_CB1` (0x10), `VIA_INT_T2` (0x20), `VIA_INT_T1` (0x40)

### Functions

| Signature | Description |
|-----------|-------------|
| `void via_init(via6522_t* via)` | Initialises the VIA |
| `void via_reset(via6522_t* via)` | VIA reset |
| `uint8_t via_read(via6522_t* via, uint8_t reg)` | Register read (0x00-0x0F) |
| `void via_write(via6522_t* via, uint8_t reg, uint8_t value)` | Register write |
| `void via_update(via6522_t* via, int cycles)` | Timer update (call every CPU cycle) |
| `void via_set_port_callbacks(via6522_t* via, ...)` | Configures the port A/B callbacks |
| `void via_set_irq_callback(via6522_t* via, ...)` | Configures the IRQ callback |
| `void via_trigger_ca1(via6522_t* via)` | Triggers a CA1 interrupt |
| `void via_trigger_ca2(via6522_t* via)` | Triggers a CA2 interrupt |
| `void via_trigger_cb1(via6522_t* via)` | Triggers a CB1 interrupt |
| `void via_trigger_cb2(via6522_t* via)` | Triggers a CB2 interrupt |
| `void via_set_cb1(via6522_t* via, bool state)` | Changes the CB1 level (edge detection) |

---

## Keyboard

**File:** `include/io/keyboard.h`

### Functions

| Signature | Description |
|-----------|-------------|
| `void oric_keyboard_init(oric_keyboard_t* kb)` | Initialises the keyboard |
| `void oric_keyboard_reset(oric_keyboard_t* kb)` | Keyboard reset |
| `void oric_keyboard_set_layout(oric_keyboard_t* kb, oric_kb_layout_t layout)` | QWERTY or AZERTY |
| `bool oric_keyboard_press_char(oric_keyboard_t* kb, char c)` | Simulates an ASCII key |
| `void oric_keyboard_release_all(oric_keyboard_t* kb)` | Releases all keys |
| `bool oric_keyboard_handle_sdl_event(oric_keyboard_t* kb, const SDL_Event* event)` | Handles an SDL2 event |

---

## IJK joystick

**File:** `include/io/joystick.h`

### Constants (active-low bits)

`IJK_RIGHT` (bit 0), `IJK_LEFT` (bit 1), `IJK_FIRE` (bit 2), `IJK_DOWN` (bit 3), `IJK_UP` (bit 4)

### Functions

| Signature | Description |
|-----------|-------------|
| `void oric_joystick_init(oric_joystick_t* joy)` | Initialises the joystick |
| `void oric_joystick_reset(oric_joystick_t* joy)` | Reset |
| `void oric_joystick_set_mode(oric_joystick_t* joy, oric_joy_mode_t mode)` | Mode DISABLED/SDL_GAMEPAD/KEYBOARD |
| `void oric_joystick_press(oric_joystick_t* joy, uint8_t mask)` | Press direction/fire |
| `void oric_joystick_release(oric_joystick_t* joy, uint8_t mask)` | Release |
| `void oric_joystick_release_all(oric_joystick_t* joy)` | Releases everything |
| `uint8_t oric_joystick_read(const oric_joystick_t* joy)` | Reads the Port A state (active low) |
| `bool oric_joystick_open_sdl(oric_joystick_t* joy, int index)` | Opens an SDL2 gamepad |
| `void oric_joystick_close_sdl(oric_joystick_t* joy)` | Closes the SDL2 gamepad |
| `bool oric_joystick_handle_sdl_event(oric_joystick_t* joy, const SDL_Event* ev)` | Handles an SDL2 event |

---

## Video

**Files:** `include/video/video.h`, `include/video/export.h`

### Constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `ORIC_SCREEN_W` | 240 | Width in pixels |
| `ORIC_SCREEN_H` | 224 | Height in pixels |
| `ORIC_TEXT_COLS` | 40 | Text mode columns |
| `ORIC_TEXT_ROWS` | 28 | Text mode rows |
| `ORIC_HIRES_W` | 240 | HIRES width |
| `ORIC_HIRES_H` | 200 | HIRES height |

### ORIC colours

`ORIC_BLACK` (0), `ORIC_RED` (1), `ORIC_GREEN` (2), `ORIC_YELLOW` (3), `ORIC_BLUE` (4), `ORIC_MAGENTA` (5), `ORIC_CYAN` (6), `ORIC_WHITE` (7)

### Video functions

| Signature | Description |
|-----------|-------------|
| `bool video_init(video_t* vid)` | Initialises the video subsystem |
| `void video_cleanup(video_t* vid)` | Frees resources |
| `void video_reset(video_t* vid)` | Video reset |
| `void video_set_mode(video_t* vid, bool hires)` | Switches text/HIRES |
| `void video_render_frame(video_t* vid, const uint8_t* memory)` | Renders a frame into the framebuffer |
| `void video_get_rgb(uint8_t oric_color, uint8_t* r, uint8_t* g, uint8_t* b)` | Converts an ORIC colour to RGB |

### Export functions

| Signature | Description |
|-----------|-------------|
| `bool video_export_ppm(const video_t* vid, const char* filename)` | PPM export (binary P6) |
| `bool video_export_bmp(const video_t* vid, const char* filename)` | 24-bit BMP export |
| `bool video_export_ascii(const video_t* vid, FILE* fp, unsigned int sx, unsigned int sy)` | ANSI text export |
| `bool video_export_auto(const video_t* vid, const char* filename)` | Auto-detection by extension |

---

## PSG audio

**File:** `include/audio/audio.h`

### Constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `AY_NUM_CHANNELS` | 3 | Tone channels |
| `AY_NUM_REGISTERS` | 16 | PSG registers |
| `AUDIO_SAMPLE_RATE` | 44100 | Sample rate |
| `AUDIO_BUFFER_SIZE` | 2048 | Audio buffer size |

### Functions

| Signature | Description |
|-----------|-------------|
| `void ay_init(ay3891x_t* ay, uint32_t clock_rate)` | Initialises the PSG with a clock frequency |
| `void ay_reset(ay3891x_t* ay)` | PSG reset |
| `void ay_write_address(ay3891x_t* ay, uint8_t addr)` | Selects a register (0-15) |
| `void ay_write_data(ay3891x_t* ay, uint8_t data)` | Writes to the selected register |
| `uint8_t ay_read_data(ay3891x_t* ay)` | Reads the selected register |
| `void ay_generate(ay3891x_t* ay, int16_t* buffer, int num_samples)` | Generates audio samples |
| `bool audio_init(ay3891x_t* psg)` | Initialises the SDL2 audio output |
| `void audio_cleanup(void)` | Closes the audio output |
| `void audio_pause(bool pause)` | Pauses/resumes audio |
| `void audio_set_cast_server(cast_server_t* server)` | Connects the Cast for streaming |

---

## TAP storage

**File:** `include/storage/tap.h`

### Constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `TAP_SYNC_BYTE` | 0x16 | Synchronisation byte |
| `TAP_MARKER` | 0x24 | Header start marker |
| `TAP_NAME_LEN` | 16 | Maximum program name length |

### Types

```c
typedef enum { TAP_BASIC = 0x00, TAP_MACHINE = 0x80, TAP_SCREEN = 0xC0 } tap_program_type_t;
```

### Functions

| Signature | Description |
|-----------|-------------|
| `tap_file_t* tap_open_read(const char* filename, bool fast_load)` | Opens a .TAP for reading |
| `tap_file_t* tap_open_write(const char* filename)` | Opens a .TAP for writing |
| `void tap_close(tap_file_t* tap)` | Closes the file |
| `bool tap_read_header(tap_file_t* tap, tap_header_t* header)` | Reads the TAP header |
| `bool tap_write_header(tap_file_t* tap, const tap_header_t* header)` | Writes the TAP header |
| `int tap_read_data(tap_file_t* tap, uint8_t* buffer, size_t size)` | Reads a data block |
| `bool tap_write_data(tap_file_t* tap, const uint8_t* data, size_t size)` | Writes a data block |
| `void tap_rewind(tap_file_t* tap)` | Rewinds to the start |
| `uint32_t tap_tell(const tap_file_t* tap)` | Current position |
| `uint32_t tap_size(const tap_file_t* tap)` | Total size |
| `bool tap_eof(const tap_file_t* tap)` | End-of-file test |
| `uint8_t tap_checksum(const uint8_t* data, size_t size)` | Checksum computation |
| `bool tap_from_basic(const char* basic, const char* tap, bool auto_run)` | Converts BASIC → TAP |
| `bool tap_from_binary(const char* bin, const char* tap, uint16_t start, uint16_t exec, const char* name)` | Converts binary → TAP |

---

## WD1793 disk

**File:** `include/storage/disk.h`

### FDC registers

| Constant | Offset | Description |
|-----------|--------|-------------|
| `FDC_STATUS/COMMAND` | 0 | Status (read) / Command (write) |
| `FDC_TRACK` | 1 | Track register |
| `FDC_SECTOR` | 2 | Sector register |
| `FDC_DATA` | 3 | Data register |

### Functions

| Signature | Description |
|-----------|-------------|
| `void fdc_init(fdc_t* fdc)` | Initialises the FDC |
| `void fdc_reset(fdc_t* fdc)` | FDC reset |
| `void fdc_set_disk(fdc_t* fdc, uint8_t* data, uint32_t size)` | Connects a disk image |
| `uint8_t fdc_read(fdc_t* fdc, uint8_t reg)` | Reads an FDC register |
| `void fdc_write(fdc_t* fdc, uint8_t reg, uint8_t value)` | Writes an FDC register |
| `void fdc_ticktock(fdc_t* fdc, unsigned int cycles)` | Advances timing (call every cycle) |

---

## Sedoric

**File:** `include/storage/sedoric.h`

### Constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `SEDORIC_SECTOR_SIZE` | 256 | Sector size |
| `SEDORIC_TRACKS` | 42 | Tracks per disk |
| `SEDORIC_SECTORS` | 17 | Sectors per track |

### Functions

| Signature | Description |
|-----------|-------------|
| `sedoric_disk_t* sedoric_create(void)` | Creates a blank disk |
| `sedoric_disk_t* sedoric_load(const char* filename)` | Loads a .DSK |
| `bool sedoric_save(sedoric_disk_t* disk, const char* filename)` | Saves a .DSK |
| `void sedoric_destroy(sedoric_disk_t* disk)` | Frees a disk |
| `uint8_t* sedoric_get_sector(sedoric_disk_t* disk, uint8_t track, uint8_t sector)` | Pointer to a sector |
| `bool sedoric_read_sector(const sedoric_disk_t* disk, uint8_t t, uint8_t s, uint8_t* buf)` | Reads a sector |
| `bool sedoric_write_sector(sedoric_disk_t* disk, uint8_t t, uint8_t s, const uint8_t* buf)` | Writes a sector |

---

## Microdisc

**File:** `include/io/microdisc.h`

### I/O addresses

| Constant | Address | Description |
|-----------|---------|-------------|
| `MICRODISC_FDC_BASE` | $0310 | WD1793 registers ($0310-$0313) |
| `MICRODISC_CTRL` | $0314 | Control / IRQ status |
| `MICRODISC_DRQ` | $0318 | DRQ status |

### Control bits

`MICRODISC_CTRL_INTENA` (0x01), `MICRODISC_CTRL_ROMDIS` (0x02), `MICRODISC_CTRL_DENSITY` (0x08), `MICRODISC_CTRL_SIDE` (0x10), `MICRODISC_CTRL_DRIVE` (0x60), `MICRODISC_CTRL_EPROM` (0x80)

### Functions

| Signature | Description |
|-----------|-------------|
| `void microdisc_init(microdisc_t* md)` | Initialises the Microdisc |
| `void microdisc_reset(microdisc_t* md)` | Microdisc reset |
| `uint8_t microdisc_read(microdisc_t* md, uint16_t addr)` | I/O read |
| `void microdisc_write(microdisc_t* md, uint16_t addr, uint8_t value)` | I/O write |
| `void microdisc_set_disk(microdisc_t* md, uint8_t drive, uint8_t* data, uint32_t size, uint8_t tracks, uint8_t spt)` | Connects a disk to a drive |
| `bool microdisc_load_rom(microdisc_t* md, const char* filename)` | Loads the overlay ROM |
| `void microdisc_cleanup(microdisc_t* md)` | Frees resources |

---

## Printer

**File:** `include/io/printer.h`

### Types

```c
typedef enum { PRINTER_NONE, PRINTER_TEXT, PRINTER_MCP40 } oric_printer_type_t;
```

### Functions

| Signature | Description |
|-----------|-------------|
| `void oric_printer_init(oric_printer_t* printer)` | Initialises the printer |
| `bool oric_printer_open(oric_printer_t* printer, const char* filename)` | Opens the output file |
| `void oric_printer_close(oric_printer_t* printer)` | Closes the output |
| `void oric_printer_check_strobe(oric_printer_t* p, uint8_t old_pcr, uint8_t new_pcr, uint8_t data)` | Detects the STROBE signal (CA2) |
| `void oric_printer_flush(oric_printer_t* printer)` | Flushes the output buffer |
| `bool oric_printer_is_active(const oric_printer_t* printer)` | Checks whether the printer is active |

---

## MCP-40 plotter

**File:** `include/io/mcp40.h`

### Constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `MCP40_WIDTH` | 480 | Plotting area width |
| `MCP40_HEIGHT` | 400 | Plotting area height |

### Colours

`MCP40_BLACK` (0), `MCP40_BLUE` (1), `MCP40_GREEN` (2), `MCP40_RED` (3)

### Functions

| Signature | Description |
|-----------|-------------|
| `void mcp40_init(mcp40_t* mcp)` | Initialises the plotter |
| `void mcp40_reset(mcp40_t* mcp)` | Clears the paper |
| `void mcp40_receive_byte(mcp40_t* mcp, uint8_t byte)` | Receives a Centronics byte |
| `bool mcp40_export_bmp(const mcp40_t* mcp, const char* filename)` | Exports as BMP |
| `void mcp40_set_output(mcp40_t* mcp, const char* filename)` | Configures automatic export |
| `void mcp40_get_pen_rgb(mcp40_color_t color, uint8_t* r, uint8_t* g, uint8_t* b)` | RGB colour of a pen |

---

## Host file system

**File:** `include/hostfs/hostfs.h`

### Constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `HOSTFS_MAX_PATH` | 256 | Maximum path length |
| `HOSTFS_MAX_HANDLES` | 8 | Simultaneously open files |

### Functions

| Signature | Description |
|-----------|-------------|
| `bool hostfs_init(hostfs_t* hfs)` | Initialises the HostFS |
| `void hostfs_cleanup(hostfs_t* hfs)` | Frees resources |
| `bool hostfs_mount(hostfs_t* hfs, const char* path, bool read_only)` | Mounts a directory |
| `void hostfs_unmount(hostfs_t* hfs)` | Unmounts |
| `bool hostfs_is_mounted(const hostfs_t* hfs)` | Checks whether mounted |
| `int hostfs_open(hostfs_t* hfs, const char* name, bool writing)` | Opens a file (returns handle 0-7) |
| `bool hostfs_close(hostfs_t* hfs, int handle)` | Closes a file |
| `int hostfs_read(hostfs_t* hfs, int handle, uint8_t* buf, size_t size)` | Reads data |
| `int hostfs_write(hostfs_t* hfs, int handle, const uint8_t* buf, size_t size)` | Writes data |
| `bool hostfs_seek(hostfs_t* hfs, int handle, uint32_t position)` | Positions the cursor |
| `uint32_t hostfs_size(hostfs_t* hfs, int handle)` | File size |
| `int hostfs_list(hostfs_t* hfs, char* buf, size_t buf_size)` | Lists the files |
| `bool hostfs_delete(hostfs_t* hfs, const char* name)` | Deletes a file |
| `bool hostfs_rename(hostfs_t* hfs, const char* old, const char* new)` | Renames a file |
| `bool hostfs_oric_to_host_path(const hostfs_t* hfs, const char* oric, char* host, size_t size)` | ORIC path → host |
| `bool hostfs_host_to_oric_name(const char* host, char* oric)` | Host name → ORIC |

---

## Debugger

**File:** `include/debugger.h`

### Constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `DEBUGGER_MAX_BREAKPOINTS` | 16 | Maximum breakpoints |
| `DEBUGGER_MAX_WATCHPOINTS` | 8 | Maximum watchpoints |

### Functions

| Signature | Description |
|-----------|-------------|
| `void debugger_init(debugger_t* dbg)` | Initialises the debugger |
| `bool debugger_should_break(debugger_t* dbg, emulator_t* emu)` | Checks whether execution should stop |
| `void debugger_repl(debugger_t* dbg, emulator_t* emu)` | Starts the interactive REPL loop |
| `int debugger_add_breakpoint(debugger_t* dbg, uint16_t addr)` | Adds a breakpoint (returns index) |
| `bool debugger_remove_breakpoint(debugger_t* dbg, int index)` | Removes a breakpoint |
| `int debugger_add_watchpoint(debugger_t* dbg, uint16_t addr)` | Adds a watchpoint (returns index) |
| `bool debugger_remove_watchpoint(debugger_t* dbg, int index)` | Removes a watchpoint |
| `bool debugger_is_breakpoint(const debugger_t* dbg, uint16_t pc)` | Checks whether PC is a breakpoint |
| `void debugger_install_watchpoint_trace(debugger_t* dbg, emulator_t* emu)` | Installs watchpoint tracing |

---

## Save states

**File:** `include/savestate.h`

### Constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `SAVESTATE_MAGIC` | `"OST1"` | File signature |
| `SAVESTATE_VERSION` | 1 | Format version |
| `SAVESTATE_HEADER_SIZE` | 48 | Header size |

### Functions

| Signature | Description |
|-----------|-------------|
| `bool savestate_save(const emulator_t* emu, const char* filename)` | Saves the complete state with CRC32 |
| `bool savestate_load(emulator_t* emu, const char* filename)` | Restores the state from an .ost file |

---

## CPU trace

**File:** `include/utils/trace.h`

### Output format

```
CCCCCCCC  AAAA  XX XX XX  MNEMONIC OPERAND       A=XX X=XX Y=XX SP=XX P=XX
```

### Functions

| Signature | Description |
|-----------|-------------|
| `void trace_init(cpu_trace_t* trace)` | Initialises the trace (inactive) |
| `bool trace_open(cpu_trace_t* trace, const char* filename)` | Opens a trace file |
| `void trace_attach(cpu_trace_t* trace, FILE* fp)` | Attaches an existing FILE* |
| `void trace_log_instruction(cpu_trace_t* trace, const cpu6502_t* cpu)` | Records an instruction (before cpu_step) |
| `void trace_close(cpu_trace_t* trace)` | Closes the trace |
| `void trace_set_max(cpu_trace_t* trace, uint64_t max)` | Limits the number of traced instructions |

---

## CPU profiler

**File:** `include/utils/profiler.h`

### Constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `PROFILER_ADDR_SPACE` | 65536 | Covers 64K addresses |
| `PROFILER_OPCODE_COUNT` | 256 | 256 possible opcodes |

### Functions

| Signature | Description |
|-----------|-------------|
| `void profiler_init(cpu_profiler_t* prof)` | Initialises the profiler (inactive) |
| `void profiler_start(cpu_profiler_t* prof)` | Enables profiling |
| `void profiler_stop(cpu_profiler_t* prof)` | Disables profiling |
| `void profiler_record_instruction(cpu_profiler_t* prof, const cpu6502_t* cpu)` | Records a hit (before cpu_step) |
| `void profiler_record_cycles(cpu_profiler_t* prof, uint16_t pc, int cycles)` | Records the cost in cycles |
| `void profiler_reset(cpu_profiler_t* prof)` | Resets the counters to zero |
| `void profiler_report(const cpu_profiler_t* prof, FILE* fp)` | Writes the report to a FILE* |
| `bool profiler_report_to_file(const cpu_profiler_t* prof, const char* filename)` | Writes the report to a file |

---

## ROM analysis

**File:** `include/utils/rominfo.h`

### Constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `ROM_BASE_ADDR` | 0xC000 | ROM base address |
| `ROMINFO_MAX_TARGETS` | 512 | Maximum JSR/JMP targets |
| `ROMINFO_MAX_STRINGS` | 256 | Maximum detected strings |
| `ROMINFO_MIN_STRING_LEN` | 4 | Minimum string length |

### Functions

| Signature | Description |
|-----------|-------------|
| `void rominfo_init(rom_analysis_t* analysis)` | Initialises the analysis |
| `bool rominfo_analyze(rom_analysis_t* a, const uint8_t* rom, size_t size)` | Full analysis of a ROM |
| `void rominfo_report(const rom_analysis_t* a, const uint8_t* rom, size_t size, FILE* fp)` | Report to a FILE* |
| `bool rominfo_report_to_file(const rom_analysis_t* a, const uint8_t* rom, size_t size, const char* file)` | Report to a file |
| `int rominfo_find_pattern(const uint8_t* rom, size_t size, const uint8_t* pat, size_t plen, uint16_t* results, int max)` | Binary pattern search |

---

## Logging

**File:** `include/utils/logging.h`

### Levels

```c
typedef enum { LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, LOG_LEVEL_WARNING, LOG_LEVEL_ERROR } log_level_t;
```

### Functions

| Signature | Description |
|-----------|-------------|
| `void log_init(log_level_t level)` | Initialises the logging system |
| `void log_cleanup(void)` | Closes the logging system |
| `void log_debug(const char* format, ...)` | Debug message |
| `void log_info(const char* format, ...)` | Info message |
| `void log_warning(const char* format, ...)` | Warning |
| `void log_error(const char* format, ...)` | Error |

---

## Cast / Streaming

**File:** `include/network/cast_server.h`

> Only available with `HAS_CAST` (build with `-DHAS_CAST`, requires pthread + OpenSSL).

### MJPEG constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `CAST_FRAME_W` | 720 | Upscaled width (240 x 3) |
| `CAST_FRAME_H` | 672 | Upscaled height (224 x 3) |
| `CAST_DEFAULT_PORT` | 8080 | Default HTTP port |
| `CAST_MAX_CLIENTS` | 8 | Simultaneous MJPEG clients |
| `CAST_JPEG_QUALITY` | 80 | JPEG quality |

### Audio constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `CAST_AUDIO_RATE` | 44100 | WAV audio frequency |
| `CAST_AUDIO_RING_SAMPLES` | 88200 | Ring buffer (2 seconds) |
| `CAST_MAX_AUDIO_CLIENTS` | 4 | Simultaneous audio clients |

### CASTV2 constants

| Constant | Value | Description |
|-----------|--------|-------------|
| `CASTV2_PORT` | 8009 | Native Google Cast port |
| `CASTV2_HEARTBEAT_SEC` | 5 | Heartbeat interval |
| `CASTV2_DASHCAST_APPID` | `"5C3F0A3C"` | DashCast App ID |

### MJPEG server functions

| Signature | Description |
|-----------|-------------|
| `bool cast_server_init(cast_server_t* server, uint16_t port)` | Starts the HTTP server |
| `void cast_server_push_frame(cast_server_t* server, const uint8_t* fb, unsigned int w, unsigned int h)` | Pushes a video frame |
| `void cast_server_push_audio(cast_server_t* server, const int16_t* samples, size_t n)` | Pushes audio samples |
| `void cast_server_stop(cast_server_t* server)` | Stops the server |
| `int cast_server_discover_devices(int timeout_ms)` | Discovers Chromecasts via mDNS |
| `void cast_server_upscale_nearest(const uint8_t* src, int sw, int sh, uint8_t* dst, int factor)` | Nearest-neighbour upscale |
| `int cast_server_build_mdns_query(uint8_t* buf, size_t size)` | Builds an mDNS query |
| `int cast_server_build_wav_header(uint8_t* buf, size_t size)` | Builds a WAV header |

### CASTV2 client functions

| Signature | Description |
|-----------|-------------|
| `bool castv2_discover_device(char* ip_out, const char* name, int timeout_ms)` | Discovers a device |
| `bool castv2_connect_and_cast(castv2_client_t* client, const char* ip, const char* url)` | Connects and casts a URL |
| `void castv2_disconnect(castv2_client_t* client)` | Disconnects |
| `bool castv2_get_local_ip(char* ip_out)` | Gets the local IP |
| `int castv2_encode_varint(uint8_t* buf, int size, uint64_t value)` | Encodes a protobuf varint |
| `int castv2_decode_varint(const uint8_t* buf, int size, uint64_t* value)` | Decodes a protobuf varint |
| `int castv2_build_message(uint8_t* buf, int size, const char* src, const char* dst, const char* ns, const char* payload)` | Builds a CASTV2 message |

---

## Statistics

| Metric | Value |
|----------|--------|
| Header files | ~58 |
| Public functions | ~200 |
| Public structures | ~40 |
| Enumerations | ~30 |
| Modules | 8 (CPU, Memory, I/O, Video, Audio, Storage, Debug, Network) |
