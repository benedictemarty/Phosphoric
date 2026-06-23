/**
 * @file serial_backend.h
 * @brief Serial backend abstraction for ACIA 6551
 * @author bmarty <bmarty@mailo.com>
 * @date 2026-03-22
 *
 * Provides pluggable backends for the ACIA serial interface:
 *   - LOOPBACK: Internal buffer for testing (TX feeds back to RX)
 *   - TCP:      TCP socket client for BBS/Minitel/Telnet connections
 *   - PTY:      POSIX pseudo-terminal for local terminal access
 */

#ifndef SERIAL_BACKEND_H
#define SERIAL_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════════
 *  Backend types
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    SERIAL_BACKEND_NONE     = 0,
    SERIAL_BACKEND_LOOPBACK = 1,    /**< Internal loopback for testing */
    SERIAL_BACKEND_TCP      = 2,    /**< TCP client socket */
    SERIAL_BACKEND_PTY      = 3,    /**< POSIX pseudo-terminal */
    SERIAL_BACKEND_MODEM    = 4,    /**< TCP with AT command emulation */
    SERIAL_BACKEND_COM      = 5,    /**< Real serial port (termios) */
    SERIAL_BACKEND_DIGITELEC = 6,   /**< Digitelec DTL 2000 V23/V21 modem */
    SERIAL_BACKEND_PICOWIFI = 7,    /**< sodiumlb PicoWiFiModemUSB (LOCI) */
    SERIAL_BACKEND_FILE     = 8,    /**< File replay (RX) / capture (TX) */
    SERIAL_BACKEND_MIDI     = 9,    /**< Real-time host MIDI port (ALSA/CoreMIDI/WinMM) */
    SERIAL_BACKEND_SMF      = 10    /**< Standard MIDI File → timed MIDI IN replay */
} serial_backend_type_t;

/* ═══════════════════════════════════════════════════════════════════════
 *  Loopback buffer
 * ═══════════════════════════════════════════════════════════════════════ */

#define SERIAL_LOOPBACK_BUFSZ   256
#define SERIAL_MODEM_BUFSZ   65536  /* 64KB RX/TX buffers */

/* ═══════════════════════════════════════════════════════════════════════
 *  Backend structure (vtable pattern)
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct serial_backend_s {
    serial_backend_type_t type;

    /**
     * @brief Open/connect the backend
     * @return true on success
     */
    bool (*open)(struct serial_backend_s* self);

    /**
     * @brief Close/disconnect the backend
     */
    void (*close)(struct serial_backend_s* self);

    /**
     * @brief Send one byte to the remote end
     * @return true if byte was accepted
     */
    bool (*send)(struct serial_backend_s* self, uint8_t byte);

    /**
     * @brief Receive one byte from the remote end
     * @param[out] byte  Received byte
     * @return true if a byte was available
     */
    bool (*recv)(struct serial_backend_s* self, uint8_t* byte);

    /**
     * @brief Check if data is available for reading
     * @return true if recv() would return data
     */
    bool (*poll)(struct serial_backend_s* self);

    /**
     * @brief Check if the connection/carrier is active
     * @return true if connected
     */
    bool (*connected)(struct serial_backend_s* self);

    /* ── Backend-specific state (union) ── */
    union {
        /* Loopback */
        struct {
            uint8_t  buf[SERIAL_LOOPBACK_BUFSZ];
            int      head;
            int      tail;
            int      count;
        } loopback;

        /* TCP */
        struct {
            int      sockfd;        /**< Socket file descriptor (-1 = closed) */
            char     host[256];     /**< Remote host */
            uint16_t port;          /**< Remote port */
        } tcp;

        /* PTY */
        struct {
            int      master_fd;     /**< Master side fd (-1 = closed) */
            char     slave_name[256]; /**< Slave device path */
        } pty;

        /* Modem (TCP + AT commands) */
        struct {
            int      sockfd;        /**< Data socket (-1 = closed) */
            int      listen_fd;     /**< Listen socket for server mode (-1 = none) */
            char     host[256];     /**< Remote host for ATD */
            uint16_t port;          /**< Remote/listen port */
            uint8_t* rx_buf;        /**< 64KB receive buffer */
            uint8_t* tx_buf;        /**< 64KB transmit buffer */
            int      rx_head, rx_tail, rx_count;
            int      tx_head, tx_tail, tx_count;
            /* AT command state machine */
            int      mode;          /**< 0=command, 1=data, 2=escape */
            char     cmd_buf[256];  /**< AT command accumulator */
            int      cmd_len;
            /* +++ escape detection */
            int      plus_count;
            int64_t  last_data_time;
            int64_t  last_plus_time;
            /* Auto-answer */
            int      s0_rings;      /**< ATS0= auto-answer ring count (0=disabled) */
            bool     echo;          /**< ATE echo on/off */
            bool     listening;     /**< Server mode active */
        } modem;

        /* COM (real serial port via termios) */
        struct {
            int      fd;            /**< Serial port fd (-1 = closed) */
            char     device[256];   /**< Device path (e.g. /dev/ttyUSB0) */
            int      baud;          /**< Baud rate */
            int      databits;      /**< 5-8 */
            char     parity;        /**< 'N', 'E', 'O' */
            int      stopbits;      /**< 1-2 */
            uint8_t  orig_termios[64]; /**< Saved original termios (opaque) */
            bool     has_orig;      /**< Original termios saved */
        } com;

        /* Digitelec DTL 2000 V23/V21 modem emulation.
         * Autonomous external modem with internal buffering, flow control
         * via CTS/RTS, carrier detect via DCD, and DTR-controlled dialing.
         * No AT commands — controlled entirely via RS232 signal lines. */
        struct {
            int      sockfd;            /**< TCP data socket (-1 = disconnected) */
            char     host[256];         /**< Remote host (BBS/Minitel server) */
            uint16_t port;              /**< Remote port */

            /* Internal modem buffers (the real DTL 2000 had its own RAM) */
            uint8_t  rx_buf[512];       /**< RX ring buffer (line → ORIC) */
            int      rx_head, rx_tail, rx_count;
            uint8_t  tx_buf[512];       /**< TX ring buffer (ORIC → line) */
            int      tx_head, tx_tail, tx_count;

            /* Modem state */
            bool     carrier;           /**< Carrier detected (TCP connected) */
            bool     dtr_was_on;        /**< Previous DTR state (edge detect) */
            int      mode;              /**< 0=V23 (1200/75), 1=V21 (300/300) */

            /* Flow control thresholds */
            int      cts_high_water;    /**< Deassert CTS when RX buf above this */
            int      cts_low_water;     /**< Reassert CTS when RX buf below this */
            bool     cts_active;        /**< Current CTS state driven to ACIA */

            /* Pointer to ACIA for driving signal lines */
            void*    acia_ptr;          /**< acia6551_t* (avoid circular include) */
        } digitelec;

        /* PicoWiFiModemUSB (sodiumlb) emulation. The real device is a
         * Raspberry Pi Pico W bridging USB CDC ↔ WiFi with a Hayes-style AT
         * command set, exposed to the Oric by the LOCI firmware as an ACIA
         * at $0380. The command set is large enough (~30 commands) to live
         * in its own module (serial_picowifi.c); the state is heap-allocated
         * and opaque here to keep this union small. */
        struct {
            void*    impl;              /**< picowifi_t* (serial_picowifi.c) */
        } picowifi;

        /* File replay/capture — a deterministic transparent transport.
         * Bytes read from `in` are delivered to the Oric as RX; bytes the Oric
         * transmits are appended to `out`. Either side may be absent. Used for
         * reproducible protocol tests without a network/peer. */
        struct {
            void*    in;                /**< FILE* replay source (NULL = none) */
            void*    out;               /**< FILE* capture sink   (NULL = none) */
            char     in_path[256];      /**< Replay file path ("" = none) */
            char     out_path[256];     /**< Capture file path ("" = none) */
            int      peeked;            /**< 1-byte RX lookahead (-1 = empty) */
        } file;

        /* ALSA sequencer MIDI port — a *real-time* transparent transport that
         * bridges the Oric's raw MIDI byte stream to the host MIDI graph. We
         * create a named, subscribable duplex ALSA seq port ("Phosphoric MIDI")
         * that shows up in `aconnect -l` / QjackCtl, so the Oric can drive a
         * software synth (FluidSynth) or a DAW, and a MIDI keyboard can play
         * into the Oric. TX bytes are re-assembled into seq events via a midi
         * encoder; RX events are decoded back into bytes. The ALSA handles are
         * opaque here to keep this header free of <alsa/asoundlib.h>. */
        struct {
            void*    seq;               /**< snd_seq_t* / platform handle (NULL=closed) */
            void*    encoder;           /**< snd_midi_event_t* TX byte→event */
            void*    decoder;           /**< snd_midi_event_t* RX event→byte */
            int      port;              /**< our seq port id */
            char     target[128];       /**< auto-connect address ("" = none) */
            uint8_t  rx_buf[1024];      /**< decoded RX byte ring */
            int      rx_head, rx_tail, rx_count;
        } midi;

        /* Standard MIDI File → timed MIDI IN replay. Parses a .mid once, then
         * paces its events out as RX bytes on a real-time (monotonic) clock, so
         * the Oric (behind the Mageco card) receives the song as if a sequencer
         * were playing the MIDI keyboard. TX from the Oric is discarded. */
        struct {
            void*    song;              /**< smf_t* parsed file (NULL = closed) */
            size_t   cursor;            /**< next event index */
            int64_t  start_ns;          /**< monotonic start (ns); 0 = not started */
            bool     loop;              /**< restart at end of song */
            uint8_t  rx_buf[512];       /**< pending RX byte ring */
            int      rx_head, rx_tail, rx_count;
        } smf;
    } state;
} serial_backend_t;

/* ═══════════════════════════════════════════════════════════════════════
 *  Factory functions
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief Create a loopback backend (TX → RX circular buffer)
 */
serial_backend_t* serial_backend_loopback_create(void);

/**
 * @brief Create a TCP client backend
 * @param host  Remote hostname or IP
 * @param port  Remote port number
 */
serial_backend_t* serial_backend_tcp_create(const char* host, uint16_t port);

/**
 * @brief Create a PTY backend (pseudo-terminal)
 *
 * After open(), the slave device path is available in state.pty.slave_name
 * for connecting minicom, screen, etc.
 */
serial_backend_t* serial_backend_pty_create(void);

/**
 * @brief Create a modem backend (TCP + AT command emulation)
 * @param host        Remote host for ATD, or bind address for listen mode
 * @param port        Remote/listen port
 * @param listen_mode If true, start as a listening server (ATA to accept)
 */
serial_backend_t* serial_backend_modem_create(const char* host, uint16_t port, bool listen_mode);

/**
 * @brief Create a COM backend (real serial port via termios)
 * @param config  Configuration string: "baud,databits,parity,stopbits,device"
 *                Example: "115200,8,N,1,/dev/ttyUSB0"
 */
serial_backend_t* serial_backend_com_create(const char* config);

/**
 * @brief Create a Digitelec DTL 2000 modem backend
 *
 * Emulates the Digitelec DTL 2000 external V23/V21 modem.
 * Connection is controlled via DTR (assert DTR = dial/connect).
 * Flow control via CTS (modem deasserts CTS when buffer full).
 * Carrier detect via DCD (TCP connection = carrier).
 * No AT commands — the real Digitelec predates the Hayes standard.
 *
 * @param host  Remote host (BBS/Minitel server)
 * @param port  Remote port
 * @param acia  Pointer to ACIA for driving DCD/DSR/CTS signals
 */
serial_backend_t* serial_backend_digitelec_create(const char* host, uint16_t port, void* acia);

/**
 * @brief Create a PicoWiFiModemUSB backend (sodiumlb, LOCI WiFi modem)
 *
 * Emulates the sodiumlb PicoWiFiModemUSB: a Raspberry Pi Pico W that the
 * LOCI firmware exposes to the Oric as an ACIA serial modem at $0380.
 * Implements the device's Hayes-style AT command set (AT$SSID/AT$PASS for
 * WiFi, ATDT for TCP dial, ATNET telnet, AT&Z speed dial, S-registers,
 * Q/V/X result-code formatting, &K/&D flow control). WiFi association is
 * simulated; data connections are real TCP sockets.
 *
 * @param ssid  Pre-set WiFi SSID, or NULL (set later via AT$SSID=)
 * @param pass  Pre-set WiFi password, or NULL (set later via AT$PASS=)
 */
serial_backend_t* serial_backend_picowifi_create(const char* ssid, const char* pass);

/**
 * @brief Create a file replay/capture backend (deterministic transport)
 *
 * Bytes read from @p in_path are delivered to the host program as received
 * data (RX); bytes the program transmits are appended to @p out_path (TX).
 * Either path may be NULL/empty: replay-only (no capture), capture-only (no
 * input, RX stays empty), or — degenerate — neither.
 *
 * Being a transparent byte pipe it works behind any UART (ACIA 6551 / 6850),
 * so it is available to both --serial and --dtl2000. Ideal for reproducible
 * protocol tests: feed a recorded server stream as input, capture the program's
 * replies for diffing.
 *
 * @param in_path   Replay source path, or NULL for none
 * @param out_path  Capture sink path, or NULL for none
 */
serial_backend_t* serial_backend_file_create(const char* in_path, const char* out_path);

/**
 * @brief Create an ALSA sequencer MIDI backend (real-time host MIDI port)
 *
 * Opens an ALSA sequencer client and creates a named, subscribable duplex port
 * ("Phosphoric MIDI") that appears in `aconnect -l`. The Oric's raw MIDI byte
 * stream is re-assembled into seq events on TX and decoded back to bytes on RX,
 * so the emulated Oric (e.g. behind the Mageco card) can drive a software synth
 * (FluidSynth) or a DAW, and a MIDI keyboard can play into the Oric.
 *
 * Only available when built with MIDI=1 (links -lasound); otherwise this returns
 * NULL with an explanatory log line so the caller degrades gracefully.
 *
 * @param target  Optional ALSA address to auto-connect to (e.g. "128:0" or a
 *                client name like "FLUID"), or NULL/"" to only create the port
 *                (connect later with aconnect).
 */
serial_backend_t* serial_backend_midi_create(const char* target);

/**
 * @brief Create a Standard MIDI File (.mid) replay backend (timed MIDI IN)
 *
 * Parses @p path as an SMF and paces its events out as received MIDI bytes on a
 * real-time clock, so the Oric (behind the Mageco card) is fed the song at its
 * musical tempo as if a sequencer were playing the keyboard. TX from the Oric is
 * discarded. Always available (pure C parser, no external dependency).
 *
 * @param path  Path to a .mid file
 * @param loop  Restart from the top when the song ends
 */
serial_backend_t* serial_backend_smf_create(const char* path, bool loop);

/**
 * @brief Destroy a backend and free resources
 */
void serial_backend_destroy(serial_backend_t* backend);

#endif /* SERIAL_BACKEND_H */
