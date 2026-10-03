/* SPDX-License-Identifier: EUPL-1.2 */
/**
 * @file cli_args.c
 * @brief Reading the command line (getopt loop) into a cli_opts_t.
 * @author bmarty <bmarty@mailo.com>
 *
 * Sprint C of the architecture plan: main()'s getopt_long loop is moved here
 * VERBATIM (same cases, same order, same messages, same exit codes). Only the
 * access to the options (cfg->…), to the emulator (emu->…) and --loci-menu-at,
 * stored in cfg then copied by main(), change. Behaviour locked by
 * tools/cli_golden.sh and test_cli_parsing.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include "cli/cli_args.h"
#include "cli/cli_options.h"  /* enum OPT_* + long_options[] */
#include "cli/cli_usage.h"    /* cli_print_usage */
#include "card_module.h"      /* options of the cards as modules */
#include "io/bus_timing.h"    /* BUS_LATCH_SUBTICK_DEFAULT */
#include "utils/logging.h"

/* Second parse (card options coming from phosphoric.cfg): getopt starts
 * over from scratch. */
int cli_parse_more(int argc, char* argv[], cli_opts_t* cfg, emulator_t* emu) {
#ifdef __GLIBC__
    optind = 0;          /* full reset of the internal state */
#else
    optind = 1;
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    optreset = 1;
#endif
    return cli_parse_args(argc, argv, cfg, emu);
}

int cli_parse_args(int argc, char* argv[], cli_opts_t* cfg, emulator_t* emu) {
    int opt;
    int option_index = 0;

    /* Core table extended with the options of the cards as modules (card_module.h). */
    const struct option* opts = card_modules_long_options(long_options);
    while ((opt = getopt_long(argc, argv, CLI_SHORT_OPTIONS, opts, &option_index)) != -1) {
        if (opt >= CARD_OPT_BASE) {
            card_modules_set_option(cfg->card_cfg, opt, optarg);
            continue;
        }
        switch (opt) {
            case 't': cfg->tape_file = optarg; break;
            case 'd': cfg->disk_files[0] = optarg; break;
            case OPT_DISK1: cfg->disk_files[1] = optarg; break;
            case OPT_DISK2: cfg->disk_files[2] = optarg; break;
            case OPT_DISK3: cfg->disk_files[3] = optarg; break;
            case OPT_DISK_WRITEBACK: cfg->disk_writeback = true; break;
            case OPT_DISK_WRITE_PROTECT: cfg->disk_write_protect = true; break;
            case OPT_DISK_CREATE: cfg->disk_create_file = optarg; cfg->disk_writeback = true; break;
            case OPT_DISK_WEB: cfg->disk_web_url = optarg; break;
            case 'r': cfg->rom_file = optarg; break;
            case 'h': cfg->hostfs_path = optarg; break;
            case 'f': cfg->fast_load = true; break;
            case 'n': cfg->headless = true; break;
            case 'c': cfg->max_cycles = atoll(optarg); break;
            case 'v': cfg->verbose = true; break;
            case OPT_SCREENSHOT: cfg->screenshot_file = optarg; break;
            case OPT_SCREENSHOT_TEXT: cfg->screenshot_text_file = optarg; break;
            case OPT_SCREENSHOT_ANSI: cfg->screenshot_ansi_file = optarg; break;
            case OPT_SCREENSHOT_TEXT_AT: if (cfg->tcap_cli_count<TIMED_CAPTURE_MAX){cfg->tcap_cli[cfg->tcap_cli_count].arg=optarg;cfg->tcap_cli[cfg->tcap_cli_count++].type=TCAP_TEXT;} break;
            case OPT_SCREENSHOT_ANSI_AT: if (cfg->tcap_cli_count<TIMED_CAPTURE_MAX){cfg->tcap_cli[cfg->tcap_cli_count].arg=optarg;cfg->tcap_cli[cfg->tcap_cli_count++].type=TCAP_ANSI;} break;
            case OPT_SCREENSHOT_AT: if (cfg->tcap_cli_count<TIMED_CAPTURE_MAX){cfg->tcap_cli[cfg->tcap_cli_count].arg=optarg;cfg->tcap_cli[cfg->tcap_cli_count++].type=TCAP_IMAGE;} break;
            case OPT_SCREENSHOT_WHEN: cfg->screenshot_when_arg = optarg; break;
            case OPT_SCREENSHOT_TEXT_WHEN: cfg->screenshot_text_when_arg = optarg; break;
            case OPT_DUMP_RAM_WHEN: cfg->dump_ram_when_arg = optarg; break;
            case OPT_POKE_AT:
            case OPT_POKE_WHEN:
                if (cfg->poke_arg_count >= POKE_MAX) {
                    log_error("Too many --poke-at/--poke-when (max %d)", POKE_MAX);
                    return 1;
                }
                cfg->poke_args[cfg->poke_arg_count].arg = optarg;
                cfg->poke_args[cfg->poke_arg_count].is_when = (opt == OPT_POKE_WHEN);
                cfg->poke_arg_count++;
                break;
            case OPT_TYPE_KEYS_WHEN: cfg->type_keys_when_arg = optarg; break;
            case OPT_FRAME_DUMP: cfg->frame_dump_dir = optarg; break;
            case OPT_FRAME_DUMP_INTERVAL: cfg->frame_dump_interval = atoi(optarg); break;
            case OPT_VIDEO: cfg->video_avi_file = optarg; break;
            case OPT_VIDEO_FPS: cfg->video_avi_fps = atoi(optarg); break;
            case OPT_VIDEO_QUALITY: cfg->video_avi_quality = atoi(optarg); break;
            case OPT_GDB:
                cfg->gdb_enabled = true;
                if (optarg) cfg->gdb_port = atoi(optarg);
                break;
            case OPT_GDB_BIND: cfg->gdb_bind = optarg; break;
            case OPT_NO_CONFIG_CARDS: cfg->no_config_cards = true; break;
            case OPT_RECORD: cfg->movie_record_file = optarg; break;
            case OPT_REPLAY: cfg->movie_replay_file = optarg; break;
            case 'k': cfg->keyboard_layout = optarg; break;
            case OPT_TYPE_KEYS:
                if (cfg->type_keys_arg_count < TYPE_KEYS_SEQ_MAX) {
                    cfg->type_keys_args[cfg->type_keys_arg_count++] = optarg;
                } else {
                    log_warning("Too many --type-keys (max %d), ignoring extra",
                                TYPE_KEYS_SEQ_MAX);
                }
                break;
            case OPT_DISK_ROM: cfg->disk_rom_file = optarg; break;
            case OPT_JASMIN_ROM: cfg->jasmin_rom_file = optarg; break;
            case 'b': emu->breakpoint = (int32_t)strtol(optarg, NULL, 16); break;
            case 'D': cfg->debug_mode = true; break;
            case OPT_DEBUG_BREAK: cfg->debug_break_addr = optarg; break;
            case OPT_CAST_SERVER:
                cfg->cast_server_enabled = true;
                if (optarg) cfg->cast_server_port = (uint16_t)atoi(optarg);
                break;
            case OPT_HTTP_API:
                cfg->http_api_enabled = true;
                if (optarg) cfg->http_api_port = (uint16_t)atoi(optarg);
                break;
            case OPT_HTTP_API_BIND: cfg->http_api_bind = optarg; break;
            case OPT_HTTP_API_ROOT: cfg->http_api_root = optarg; break;
            case OPT_CAST_TO:
                cfg->cast_to_enabled = true;
                if (optarg) cfg->cast_to_device = optarg;
                break;
            case OPT_CAST_DISCOVER: cfg->cast_discover = true; break;
            case OPT_SAVE_STATE: cfg->save_state_file = optarg; break;
            case OPT_LOAD_STATE: cfg->load_state_file = optarg; break;
            case 'm': cfg->model_arg = optarg; break;
            case 'j': cfg->joystick_mode = optarg; break;
            case 'p': cfg->printer_file = optarg; break;
            case OPT_PRINTER_TYPE: cfg->printer_type_arg = optarg; break;
            case OPT_SCALE:
                cfg->scale_factor = atoi(optarg);
                if (cfg->scale_factor < 1 || cfg->scale_factor > 4) {
                    fprintf(stderr, "Invalid scale factor: %s (must be 1-4)\n", optarg);
                    return 1;
                }
                break;
            case OPT_RENDER_SOFTWARE: cfg->render_software = true; break;
            case OPT_NO_BORDER: emu->no_border = true; break;
            case OPT_EXPORT_BORDER: emu->export_border = true; break;
            case OPT_REALTIME: emu->realtime = true; break;
            case OPT_TAPE_SIGNAL: cfg->tape_signal = true; break;
            case OPT_TAPE_SIGNAL_FREE: cfg->tape_signal = true; cfg->tape_signal_free = true; break;
            case OPT_TAPE_OUT_CAPTURE: cfg->tape_out_capture_arg = optarg; break;
            case OPT_TRACE: cfg->trace_file = optarg; break;
            case OPT_CPU_MICROSEQ: cfg->cpu_microseq = true; break;   /* default, kept for scripts */
            case OPT_CPU_LEGACY: cfg->cpu_microseq = false; break;
            case OPT_ULA_CYCLE: cfg->ula_per_cycle = true; break;   /* default, kept for scripts */
            case OPT_ULA_LINE: cfg->ula_per_cycle = false; break;
            case OPT_ULA_FETCH_OFFSET: cfg->ula_fetch_offset = atoi(optarg); break;
            case OPT_CYCLE_TRACE: cfg->cycle_trace_file = optarg; break;
            case OPT_CYCLE_TRACE_MAX: cfg->cycle_trace_max = strtoull(optarg, NULL, 10); break;
            case OPT_TRACE_MAX: cfg->trace_max = atoll(optarg); break;
            case OPT_TRACE_RING: cfg->trace_ring = atoll(optarg); break;
            case OPT_PROFILE: cfg->profile_file = optarg; break;
            case OPT_ROM_INFO:
                cfg->rom_info_enabled = true;
                if (optarg) cfg->rom_info_file = optarg;
                break;
            case OPT_SERIAL:
                cfg->serial_arg = optarg;
                break;
            case OPT_SERIAL_V23:
                cfg->serial_v23 = true;
                break;
            case OPT_SERIAL_BUFFER:
                cfg->serial_buffer_size = atoi(optarg);
                break;
            case OPT_SERIAL_BAUD:
                cfg->serial_baud = atoi(optarg);
                if (cfg->serial_baud < 0) cfg->serial_baud = 0;
                break;
            case OPT_SERIAL_IRQ_RDRF:
                cfg->serial_irq_on_rdrf = true;
                break;
            case OPT_SERIAL_TRACE:
                cfg->serial_trace_file = optarg;
                break;
            case OPT_SERIAL_TCP_BACKPRESSURE:
                cfg->serial_tcp_backpressure = true;
                if (optarg) {
                    cfg->serial_tcp_rcvbuf = atoi(optarg);
                    if (cfg->serial_tcp_rcvbuf < 0) cfg->serial_tcp_rcvbuf = 0;
                }
                break;
            case OPT_LOCI_IRQ_LATENCY:
                cfg->loci_irq_latency_us = atol(optarg);
                if (cfg->loci_irq_latency_us < 0) cfg->loci_irq_latency_us = 0;
                break;
            case OPT_DUMP_RAM_AT: if (cfg->tcap_cli_count<TIMED_CAPTURE_MAX){cfg->tcap_cli[cfg->tcap_cli_count].arg=optarg;cfg->tcap_cli[cfg->tcap_cli_count++].type=TCAP_DUMP_RAM;} break;
            case OPT_BAD_SECTOR:
                if (cfg->bad_sector_arg_count < FDC_MAX_BAD_SECTORS)
                    cfg->bad_sector_args[cfg->bad_sector_arg_count++] = optarg;
                else
                    log_error("--bad-sector: map full (%d max), ignoring %s",
                              FDC_MAX_BAD_SECTORS, optarg);
                break;
            case OPT_FDC_TIMING: cfg->fdc_timing_arg = optarg; break;
            case OPT_TRACE_IRQ: cfg->trace_irq_file = optarg; break;
            case OPT_PSG_TRACE: cfg->psg_trace_file = optarg; break;
            case OPT_KBD_TRACE: cfg->kbd_trace_file = optarg; break;
            case OPT_AUDIO_WAV: cfg->audio_wav_file = optarg; break;
            case OPT_SYMBOLS: cfg->symbols_file = optarg; break;
            case OPT_TUI: cfg->tui_mode = true; cfg->debug_mode = true; break;
            case OPT_CONTROL:
                cfg->control_mode = true;
                cfg->debug_mode = true;
                cfg->headless = true;
                /* Redirect logs to stderr as early as possible so the
                 * init banner doesn't pollute the protocol channel. */
                log_set_stream(stderr);
                break;
            case OPT_BENCH:
                cfg->bench_mode = true;
                cfg->headless = true;
                /* Logs to stderr so the single-line BENCH report on
                 * stdout is easy to grep / pipe / parse. */
                log_set_stream(stderr);
                break;
            case OPT_LOCI: cfg->loci_enabled = true; break;
            case OPT_LOCI_FLASH: cfg->loci_flash_root = optarg; cfg->loci_enabled = true; break;
            case OPT_LOCI_EMU: cfg->loci_emu_path = optarg; cfg->loci_enabled = true; break;
            case OPT_LOCI_EMU_USB_IMAGE: cfg->loci_emu_usb_image = optarg; break;
            case OPT_LOCI_EMU_CDC: cfg->loci_emu_cdc_dev = optarg; break;
            case OPT_LOCI_EMU_FLASH: cfg->loci_emu_flash = optarg; break;
            case OPT_LOCI_HW: cfg->loci_hw_dev = optarg; cfg->loci_enabled = true; break;
            case OPT_LOCI_MENU_AT: cfg->loci_menu_at = strtoull(optarg, NULL, 0); break;
            case OPT_LOCI_SDIMG: cfg->loci_sdimg_path = optarg; cfg->loci_enabled = true; break;
            case OPT_LOCI_WEB: cfg->loci_web_url = optarg; cfg->loci_enabled = true; break;
            case OPT_LOCI_WEB_BASE: cfg->loci_web_base = optarg; cfg->loci_enabled = true; break;
            case OPT_LOCI_USB:
                if (strcmp(optarg, "none") == 0) {
                    cfg->loci_usb_autoscan = false;
                } else if (cfg->loci_usb_count < LOCI_USB_DEV_MAX) {
                    cfg->loci_usb_args[cfg->loci_usb_count++] = optarg;
                    cfg->loci_enabled = true;
                } else {
                    log_error("--loci-usb: table full (%d max), ignoring %s",
                              LOCI_USB_DEV_MAX, optarg);
                }
                break;
            case OPT_LOCI_MIA_WINDOW: {
                /* Models the reliable tior range of a real LOCI/Oric pairing:
                 * "LO-HI" (0-31). picowifi ACIA accesses outside it corrupt. */
                int lo = 0, hi = 31;
                if (sscanf(optarg, "%d-%d", &lo, &hi) == 2) {
                    cfg->loci_mia_win_lo = lo;
                    cfg->loci_mia_win_hi = hi;
                } else {
                    log_error("--loci-mia-window: expected LO-HI (e.g. 12-18)");
                    return 1;
                }
                break;
            }
            case OPT_LOCI_SERVE_TIMING: {
                /* Sub-cycle PHI2 race model (bus_timing.h, epic B):
                 * "SERVE[,LATCH]" in PHI2×30 subticks. The serve arrives at
                 * (tior + SERVE); clean iff ≤ LATCH (default 27). A short SERVE
                 * (-Os build ≈ 26) passes, a long one (-O2 ≈ 36) misses. */
                int serve = 0, latch = BUS_LATCH_SUBTICK_DEFAULT;
                int n = sscanf(optarg, "%d,%d", &serve, &latch);
                if (n >= 1 && serve >= 0) {
                    cfg->loci_serve_subticks = serve;
                    cfg->loci_latch_subtick = (n == 2) ? latch : BUS_LATCH_SUBTICK_DEFAULT;
                } else {
                    log_error("--loci-serve-timing: expected SERVE[,LATCH] (e.g. 26,27)");
                    return 1;
                }
                break;
            }
            case OPT_LOCI_SERVE_JITTER: {
                /* "AMP[,SEED]": jitter amplitude (subticks) + PRNG seed.
                 * Makes the occasional misses near the latch reproducible. */
                int amp = 0; unsigned seed = 0;
                int n = sscanf(optarg, "%d,%u", &amp, &seed);
                if (n >= 1 && amp >= 0) {
                    cfg->loci_serve_jitter = amp;
                    cfg->loci_jitter_seed = (n == 2) ? seed : 0;
                } else {
                    log_error("--loci-serve-jitter: expected AMP[,SEED] (e.g. 3,12345)");
                    return 1;
                }
                break;
            }
            case OPT_CONFIG: cfg->config_path = optarg; break;
            case OPT_NO_CONFIG: cfg->no_config = true; break;
            case OPT_MENU_SCREENSHOT: cfg->menu_screenshot = optarg; break;
            case OPT_ACIA_ADDR:
                cfg->acia_addr_arg = optarg;
                break;
            case '?':
            default:
                cli_print_usage(argv[0]);
                return 0;
        }
    }
    return -1;
}
