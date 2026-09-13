#include <ctype.h>
#include <hardware/watchdog.h>
#include <pico/bootrom.h>
#include <pico/stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if WITH_GUI
#include "gui.h"
#endif
#if WITH_MSC
#include "usb/msc_mode.h"
#endif
#include "ps1/ps1_mmce.h"
#include "ps2/mmceman/ps2_mmceman.h"

#include "ps1/ps1_memory_card.h"
#include "ps1/ps1_cardman.h"

#include "debug.h"
#include "serial_input.h"
#include "settings.h"

#define SERIAL_INPUT_BUFFER_SIZE 256
#define SERIAL_INPUT_MAX_ARGS   8

#define ASCII_BS  0x08
#define ASCII_DEL 0x7F

typedef enum {
    SERIAL_INPUT_CMD_NONE = 0,
    SERIAL_INPUT_CMD_INVALID,
    SERIAL_INPUT_CMD_RESET,
    SERIAL_INPUT_CMD_RESET_TO_BOOTLOADER,
    SERIAL_INPUT_CMD_PS1_MODE,
    SERIAL_INPUT_CMD_PS2_MODE,
    SERIAL_INPUT_CMD_PS2_VARIANT_RETAIL,
    SERIAL_INPUT_CMD_PS2_VARIANT_PROTO,
    SERIAL_INPUT_CMD_PS2_VARIANT_SC2,
    SERIAL_INPUT_CMD_PS2_VARIANT_COH,
    SERIAL_INPUT_CMD_CHANNEL_UP,
    SERIAL_INPUT_CMD_CHANNEL_DOWN,
    SERIAL_INPUT_CMD_CARD_BOOT,
    SERIAL_INPUT_CMD_CARD_UP,
    SERIAL_INPUT_CMD_CARD_DOWN,
    SERIAL_INPUT_CMD_CARD_IDX,
    SERIAL_INPUT_CMD_CHANNEL_IDX,
    SERIAL_INPUT_CMD_GAMEID,
    SERIAL_INPUT_CMD_START_EMULATION,
    SERIAL_INPUT_CMD_STATUS,
#if PS1_MC_TUNING
    SERIAL_INPUT_CMD_TUNE,
    SERIAL_INPUT_CMD_TUNE_SHOW,
    SERIAL_INPUT_CMD_MCWRITES,
    SERIAL_INPUT_CMD_MCGET,
    SERIAL_INPUT_CMD_MCRING,
    SERIAL_INPUT_CMD_MCCLEAR,
#endif
#if PS1_MMCE_TRACE
    SERIAL_INPUT_CMD_MMCELOG,
#endif
    SERIAL_INPUT_CMD_HELP
} serial_input_cmd_t;

typedef struct {
    serial_input_cmd_t cmd;
    int idx;
    int channel;
    char gameid[16];
    const char* error;
} serial_input_cmd_data_t;

static const char prompt[] = "> ";

static const char help_text[] =
    "Serial Input Commands:\n"
    "  help                                - Show this help\n"
    "  reset bl                            - Reset to bootloader\n"
    "  reset dev                           - Reset device\n"
    "  channel up                          - Channel up\n"
    "  channel down                        - Channel down\n"
    "  set channel <idx>                   - Set channel index\n"
    "  card boot                           - Mount the boot card (PS1)\n"
    "  card up                             - Card up\n"
    "  card down                           - Card down\n"
    "  set card <idx> [channel <idx>]      - Set card with optional channel\n"
    "  set game <id> [channel <idx>]       - Set game with optional channel\n"
    "  set mode <mode>                     - Set mode: ps1, ps2\n"
    "  set variant <variant>               - Set PS2 variant: retail, proto, conquest, arcade\n"
    "  start emulation                     - Start card emulation (after eject)\n"
    "  status                              - Mode (PS1/PS2), running mode and USB state\n"
#if PS1_MC_TUNING
    "PS1 boot profile tuning (PS1_MC_TUNING build):\n"
    "  mcstat                              - print the EFFECTIVE state; read it before every test\n"
    "  mcwrites                            - console writes to the boot card + read-backs; prints and clears\n"
    "  mcget                               - full state as key=value lines, for mctest.py\n"
    "  mcring                              - event ring: when each byte arrived\n"
    "  mcclear                             - reset histograms, counters and the event ring\n"
    "  set txdepth 0..8                    - TX FIFO fill cap on long reads; 0 = upstream lock-step\n"
    "  set ackwidth <cycles>               - ACK pulse width 2..264; 0 = computed for the 2us minimum\n"
    "  set ackhead <cycles>                - delay before requesting the ACK 0..31; 255 = per profile\n"
    "  set tracestop 0|1                   - freeze the event ring on the first long silence\n"
    "  set ackdelay <us>                   - delay before the ACK (only when the PIO waits for us)\n"
    "  set ackwait 0|1                     - wait for the 8th rising edge before the next pull (default 1)\n"
    "  set ackwidth <cycles>               - force the ACK width; 0 = computed from the divider (default)\n"
    "  set datpio|rxpio|ctrlpio <div>      - PIO dividers, 0 = profile default\n"
    "  set profile 0|1|2                   - 0 auto, 1 force standard, 2 force boot (whole profile)\n"
    "  set mmcenodelay 0|1                 - MMCE commands skip the ACK delay (default 1)\n"
    "  set delayall 0|1                    - apply the delay on any profile, not just boot (default 0)\n"
    "  set boottimeout <s>                 - BootCard inactivity timeout, 255 = use settings.ini\n"
#endif
#if PS1_MMCE_TRACE
    "MMCE command trace (PS1_MMCE_TRACE build):\n"
    "  mmcelog                             - MMCE commands the console sent; prints and clears\n"
#endif
    ;

static char in_buffer[SERIAL_INPUT_BUFFER_SIZE];
static size_t in_len;
static bool prompt_started;
static bool prompt_needs_redraw;
static bool ignore_next_lf;

static void cmd_data_init(serial_input_cmd_data_t* cmd_data) {
    cmd_data->cmd = SERIAL_INPUT_CMD_NONE;
    cmd_data->idx = -1;
    cmd_data->channel = -1;
    cmd_data->gameid[0] = '\0';
    cmd_data->error = NULL;
}

static void terminal_prompt(void) {
    printf("%s", prompt);
    prompt_started = true;
    prompt_needs_redraw = false;
}

static void terminal_redraw_input_line(void) {
    if (!prompt_started) {
        terminal_prompt();
        return;
    }

    printf("\r\n%s%s", prompt, in_buffer);
    prompt_needs_redraw = false;
}

static void terminal_erase_last_char(void) {
    /*
     * Most terminals send either BS (^H, 0x08) or DEL (^?, 0x7F) for the
     * Backspace key.  Erase visually with ANSI cursor-left instead of \b so
     * terminals that don't treat BS as a cursor movement still redraw cleanly.
     */
    printf("\x1b[D \x1b[D");
}

void serial_input_notify_output(void) {
    if (prompt_started) {
        prompt_needs_redraw = true;
    }
}

void serial_input_begin_output(void) {
    if (prompt_started) {
        printf("\r\n");
        prompt_started = false;
        prompt_needs_redraw = true;
    }
}

static int tokenize(char* input, char* argv[], int max_args) {
    int argc = 0;
    char* p = input;

    while (*p != '\0') {
        while (isspace((unsigned char)*p)) {
            *p++ = '\0';
        }

        if (*p == '\0') {
            break;
        }

        if (argc >= max_args) {
            return -1;
        }

        argv[argc++] = p;

        while ((*p != '\0') && !isspace((unsigned char)*p)) {
            ++p;
        }
    }

    return argc;
}

static bool parse_int_arg(const char* text, int* value) {
    char* end = NULL;
    long parsed = strtol(text, &end, 10);

    if ((text == end) || (*end != '\0') || (parsed < 0) || (parsed > UINT16_MAX)) {
        return false;
    }

    *value = (int)parsed;
    return true;
}

static bool parse_optional_channel(int argc, char* argv[], int start_arg, int* channel, const char** error) {
    *channel = -1;

    if (argc == start_arg) {
        return true;
    }

    if ((argc != start_arg + 2) || (strcmp(argv[start_arg], "channel") != 0)) {
        *error = "Expected optional syntax: channel <idx>";
        return false;
    }

    if (!parse_int_arg(argv[start_arg + 1], channel)) {
        *error = "Invalid channel index";
        return false;
    }

    return true;
}

static void parse_command(char* input, serial_input_cmd_data_t* cmd_data) {
    char* argv[SERIAL_INPUT_MAX_ARGS];
    int argc;

    cmd_data_init(cmd_data);
    argc = tokenize(input, argv, SERIAL_INPUT_MAX_ARGS);

    if (argc == 0) {
        return;
    }

    if (argc < 0) {
        cmd_data->cmd = SERIAL_INPUT_CMD_INVALID;
        cmd_data->error = "Too many arguments";
        return;
    }

    if ((argc == 1) && (strcmp(argv[0], "help") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_HELP;
    } else if ((argc == 2) && (strcmp(argv[0], "reset") == 0) && (strcmp(argv[1], "bl") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_RESET_TO_BOOTLOADER;
    } else if ((argc == 2) && (strcmp(argv[0], "reset") == 0) && (strcmp(argv[1], "dev") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_RESET;
    } else if ((argc == 2) && (strcmp(argv[0], "start") == 0) && (strcmp(argv[1], "emulation") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_START_EMULATION;
    } else if ((argc == 1) && (strcmp(argv[0], "status") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_STATUS;
#if PS1_MC_TUNING
    } else if ((argc == 1) && (strcmp(argv[0], "mcstat") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_TUNE_SHOW;
    } else if ((argc == 1) && (strcmp(argv[0], "mcwrites") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_MCWRITES;
    } else if ((argc == 1) && (strcmp(argv[0], "mcring") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_MCRING;
    } else if ((argc == 1) && (strcmp(argv[0], "mcget") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_MCGET;
    } else if ((argc == 1) && (strcmp(argv[0], "mcclear") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_MCCLEAR;
#endif
#if PS1_MMCE_TRACE
    } else if ((argc == 1) && (strcmp(argv[0], "mmcelog") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_MMCELOG;
#endif
#if PS1_MC_TUNING
    } else if ((argc == 3) && (strcmp(argv[0], "set") == 0)
               && ((strcmp(argv[1], "ackdelay") == 0)
                   || (strcmp(argv[1], "ackwidth") == 0)
                   || (strcmp(argv[1], "datpio") == 0)
                   || (strcmp(argv[1], "rxpio") == 0)
                   || (strcmp(argv[1], "ctrlpio") == 0)
                   || (strcmp(argv[1], "profile") == 0)
                   || (strcmp(argv[1], "mmcenodelay") == 0)
                   || (strcmp(argv[1], "longnodelay") == 0)
                   || (strcmp(argv[1], "ackwait") == 0)
                   || (strcmp(argv[1], "txdepth") == 0)
                   || (strcmp(argv[1], "ackhead") == 0)
                   || (strcmp(argv[1], "tracestop") == 0)
                   || (strcmp(argv[1], "delayall") == 0)
                   || (strcmp(argv[1], "boottimeout") == 0))) {
        cmd_data->cmd = SERIAL_INPUT_CMD_TUNE;
        cmd_data->gameid[0] = 0;
        strncpy(cmd_data->gameid, argv[1], sizeof(cmd_data->gameid) - 1);
        if (!parse_int_arg(argv[2], &cmd_data->idx)) {
            cmd_data->cmd = SERIAL_INPUT_CMD_INVALID;
            cmd_data->error = "Usage: set <knob> <value>";
        }
#endif
    } else if ((argc == 2) && (strcmp(argv[0], "channel") == 0) && (strcmp(argv[1], "up") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_CHANNEL_UP;
    } else if ((argc == 2) && (strcmp(argv[0], "channel") == 0) && (strcmp(argv[1], "down") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_CHANNEL_DOWN;
    } else if ((argc == 2) && (strcmp(argv[0], "card") == 0) && (strcmp(argv[1], "boot") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_CARD_BOOT;
    } else if ((argc == 2) && (strcmp(argv[0], "card") == 0) && (strcmp(argv[1], "up") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_CARD_UP;
    } else if ((argc == 2) && (strcmp(argv[0], "card") == 0) && (strcmp(argv[1], "down") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_CARD_DOWN;
    } else if ((argc >= 3) && (strcmp(argv[0], "set") == 0) && (strcmp(argv[1], "channel") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_CHANNEL_IDX;
        if ((argc != 3) || !parse_int_arg(argv[2], &cmd_data->idx)) {
            cmd_data->cmd = SERIAL_INPUT_CMD_INVALID;
            cmd_data->error = "Usage: set channel <idx>";
        }
    } else if ((argc >= 3) && (strcmp(argv[0], "set") == 0) && (strcmp(argv[1], "card") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_CARD_IDX;
        if (!parse_int_arg(argv[2], &cmd_data->idx)) {
            cmd_data->cmd = SERIAL_INPUT_CMD_INVALID;
            cmd_data->error = "Invalid card index";
        } else if (!parse_optional_channel(argc, argv, 3, &cmd_data->channel, &cmd_data->error)) {
            cmd_data->cmd = SERIAL_INPUT_CMD_INVALID;
        }
    } else if ((argc >= 3) && (strcmp(argv[0], "set") == 0) && (strcmp(argv[1], "game") == 0)) {
        cmd_data->cmd = SERIAL_INPUT_CMD_GAMEID;
        if (strlen(argv[2]) >= sizeof(cmd_data->gameid)) {
            cmd_data->cmd = SERIAL_INPUT_CMD_INVALID;
            cmd_data->error = "Game ID is too long";
        } else if (!parse_optional_channel(argc, argv, 3, &cmd_data->channel, &cmd_data->error)) {
            cmd_data->cmd = SERIAL_INPUT_CMD_INVALID;
        } else {
            strcpy(cmd_data->gameid, argv[2]);
        }
    } else if ((argc == 3) && (strcmp(argv[0], "set") == 0) && (strcmp(argv[1], "mode") == 0)) {
        if (strcmp(argv[2], "ps1") == 0) {
            cmd_data->cmd = SERIAL_INPUT_CMD_PS1_MODE;
        } else if (strcmp(argv[2], "ps2") == 0) {
            cmd_data->cmd = SERIAL_INPUT_CMD_PS2_MODE;
        } else {
            cmd_data->cmd = SERIAL_INPUT_CMD_INVALID;
            cmd_data->error = "Mode must be one of: ps1, ps2";
        }
    } else if ((argc == 3) && (strcmp(argv[0], "set") == 0) && (strcmp(argv[1], "variant") == 0)) {
        if (strcmp(argv[2], "retail") == 0) {
            cmd_data->cmd = SERIAL_INPUT_CMD_PS2_VARIANT_RETAIL;
        } else if (strcmp(argv[2], "proto") == 0) {
            cmd_data->cmd = SERIAL_INPUT_CMD_PS2_VARIANT_PROTO;
        } else if (strcmp(argv[2], "conquest") == 0) {
            cmd_data->cmd = SERIAL_INPUT_CMD_PS2_VARIANT_SC2;
        } else if (strcmp(argv[2], "arcade") == 0) {
            cmd_data->cmd = SERIAL_INPUT_CMD_PS2_VARIANT_COH;
        } else {
            cmd_data->cmd = SERIAL_INPUT_CMD_INVALID;
            cmd_data->error = "Variant must be one of: retail, proto, conquest, arcade";
        }
    } else {
        cmd_data->cmd = SERIAL_INPUT_CMD_INVALID;
        cmd_data->error = "Unknown command. Type 'help' for available commands.";
    }
}

#if WITH_MSC
/* Comandi ammessi mentre il volume e' montato sul PC: solo quelli che NON
   toccano la microSD, che in passthrough e' dell'host. Eseguire gli altri
   vorrebbe dire leggerla o riscriverla sotto il naso di Windows.
 *
 * Il cancello nasce insieme alla seriale viva in passthrough: prima il loop MSC
 * non chiamava serial_input_process(), quindi il problema non si poneva ma la
 * scheda sembrava piantata. Renderla viva senza questo avrebbe reso eseguibili
 * proprio i comandi pericolosi. */
static bool cmd_safe_in_passthrough(serial_input_cmd_t cmd) {
    switch (cmd) {
        case SERIAL_INPUT_CMD_NONE:
        case SERIAL_INPUT_CMD_INVALID:
        case SERIAL_INPUT_CMD_HELP:
        case SERIAL_INPUT_CMD_STATUS:
        case SERIAL_INPUT_CMD_RESET:
        case SERIAL_INPUT_CMD_RESET_TO_BOOTLOADER:
        /* Passa il cancello per poter dare il suo messaggio, che dice cosa fare
           invece del rifiuto generico. */
        case SERIAL_INPUT_CMD_START_EMULATION:
#if PS1_MC_TUNING
        case SERIAL_INPUT_CMD_TUNE:
        case SERIAL_INPUT_CMD_TUNE_SHOW:
        case SERIAL_INPUT_CMD_MCWRITES:
        case SERIAL_INPUT_CMD_MCGET:
        case SERIAL_INPUT_CMD_MCRING:
        case SERIAL_INPUT_CMD_MCCLEAR:
#endif
#if PS1_MMCE_TRACE
        case SERIAL_INPUT_CMD_MMCELOG:
#endif
            return true;
        default:
            return false;
    }
}
#endif

/* Le tre righe che mancavano, ed e' la loro assenza che ha fatto perdere un
   giro: senza, dalla seriale non si distingue "modalita' PC" da "emulazione
   attiva", e i numeri del PIO risultano tutti zero senza spiegazione. */
static void print_status(void) {
    const int configured = settings_get_mode(true);
    const int running = main_get_running_mode();

    printf("mode: %s (in esecuzione: %s)%s\n",
           (configured == MODE_PS2) ? "PS2" : "PS1",
           (running == MODE_PS2) ? "PS2" : "PS1",
           (configured == MODE_PS2) ? "  <-- in una PS1 DANNEGGIA LA SCHEDA" : "");
    if (configured != running)
        printf("      cambio di modalita' IN ATTESA che la card sia idle\n");
#if WITH_MSC
    printf("usb:  %s\n", msc_mode_state_str());
#else
    printf("usb:  emulazione attiva\n");
#endif
    /* Card e canale: senza, una prova puo' misurare l'immagine sbagliata senza
       che nessuno se ne accorga. In BOOT il canale sceglie quale payload. */
    {
        static const char *st[] = { "NAMED", "BOOT", "GAMEID", "NORMAL" };
        const unsigned k = (unsigned)ps1_cardman_get_state();
        printf("card: %s  indice %d  canale %d\n",
               (k < 4u) ? st[k] : "?", ps1_cardman_get_idx(),
               ps1_cardman_get_channel());
    }
}

static void execute_command(const serial_input_cmd_data_t* cmd_data) {
#if WITH_MSC
    if ((msc_mode_state() == MSC_STATE_PASSTHROUGH)
        && !cmd_safe_in_passthrough(cmd_data->cmd)) {
        printf("volume montato, smonta il volume per continuare\n");
        return;
    }
#endif
    switch (cmd_data->cmd) {
        case SERIAL_INPUT_CMD_NONE:
            break;
        case SERIAL_INPUT_CMD_INVALID:
            printf("%s\n", cmd_data->error ? cmd_data->error : "Invalid command");
            break;
        case SERIAL_INPUT_CMD_RESET_TO_BOOTLOADER:
            printf("Resetting to Bootloader\n");
            reset_usb_boot(0, 0);
            break;
        case SERIAL_INPUT_CMD_RESET:
            printf("Resetting\n");
            watchdog_reboot(0, 0, 0);
            break;
        case SERIAL_INPUT_CMD_CHANNEL_UP:
            printf("Channel Up\n");
            if (settings_get_mode(true) == MODE_PS2) {
                ps2_mmceman_next_ch(false);
            } else {
                ps1_mmce_next_ch(false);
            }
            break;
        case SERIAL_INPUT_CMD_CHANNEL_DOWN:
            printf("Channel Down\n");
            if (settings_get_mode(true) == MODE_PS2) {
                ps2_mmceman_prev_ch(false);
            } else {
                ps1_mmce_prev_ch(false);
            }
            break;
        case SERIAL_INPUT_CMD_CHANNEL_IDX:
            printf("Set Channel Index: %d\n", cmd_data->idx);
            if (settings_get_mode(true) == MODE_PS2) {
                ps2_mmceman_set_channel((uint16_t)cmd_data->idx, false);
            } else {
                ps1_mmce_set_channel((uint16_t)cmd_data->idx, false);
            }
            break;
        /* La boot card si poteva montare solo dall'accensione della scheda o
           da un comando MMCE, cioe' da UniROM. Durante una campagna di misura
           questo significa un giro di cavi per ogni prova sull'exploit:
           scollegare l'USB, resettare la console, aspettare il LED verde,
           ricollegare. Da qui si fa in un comando. */
        case SERIAL_INPUT_CMD_CARD_BOOT:
            if (settings_get_mode(true) == MODE_PS1) {
                ps1_cardman_switch_bootcard();
                printf("Boot card montata\n");
            } else {
                printf("Solo in modalita' PS1\n");
            }
            break;
        case SERIAL_INPUT_CMD_CARD_UP:
            printf("Card Up\n");
            if (settings_get_mode(true) == MODE_PS2) {
                ps2_mmceman_next_idx(false);
            } else {
                ps1_mmce_next_idx(false);
            }
            break;
        case SERIAL_INPUT_CMD_CARD_DOWN:
            printf("Card Down\n");
            if (settings_get_mode(true) == MODE_PS2) {
                ps2_mmceman_prev_idx(false);
            } else {
                ps1_mmce_prev_idx(false);
            }
            break;
        case SERIAL_INPUT_CMD_CARD_IDX:
            printf("Set Card Index: %d\n", cmd_data->idx);
            if (settings_get_mode(true) == MODE_PS2) {
                ps2_mmceman_set_card((uint16_t)cmd_data->idx, false);
                if (cmd_data->channel >= 0) {
                    ps2_mmceman_set_channel((uint16_t)cmd_data->channel, false);
                }
            } else {
                ps1_mmce_set_card((uint16_t)cmd_data->idx, false);
                if (cmd_data->channel >= 0) {
                    ps1_mmce_set_channel((uint16_t)cmd_data->channel, false);
                }
            }
            break;
        case SERIAL_INPUT_CMD_GAMEID:
            printf("Set Game ID: %s\n", cmd_data->gameid);
            if (settings_get_mode(true) == MODE_PS2) {
                if (!ps2_mmceman_set_gameid((const uint8_t*)cmd_data->gameid)) {
                    printf("Invalid Game ID: %s\n", cmd_data->gameid);
                } else if (cmd_data->channel >= 0) {
                    ps2_mmceman_set_channel((uint16_t)cmd_data->channel, false);
                }
            } else {
                if (!ps1_mmce_set_gameid((const uint8_t*)cmd_data->gameid)) {
                    printf("Invalid Game ID: %s\n", cmd_data->gameid);
                } else if (cmd_data->channel >= 0) {
                    ps1_mmce_set_channel((uint16_t)cmd_data->channel, false);
                }
            }
            break;
        case SERIAL_INPUT_CMD_PS1_MODE:
            printf("Set Mode: PS1\n");
            settings_set_mode(MODE_PS1);
            #if WITH_GUI
            gui_request_refresh();
            #endif
            break;
        case SERIAL_INPUT_CMD_PS2_MODE:
            printf("Set Mode: PS2\n");
            settings_set_mode(MODE_PS2);
            #if WITH_GUI
            gui_request_refresh();
            #endif
            break;
        case SERIAL_INPUT_CMD_PS2_VARIANT_RETAIL:
            printf("Set PS2 Variant: Retail\n");
            settings_set_ps2_variant(PS2_VARIANT_RETAIL);
            settings_set_mode(MODE_PS2);
            #if WITH_GUI
            gui_request_refresh();
            #endif
            break;
        case SERIAL_INPUT_CMD_PS2_VARIANT_PROTO:
            printf("Set PS2 Variant: Proto\n");
            settings_set_ps2_variant(PS2_VARIANT_PROTO);
            settings_set_mode(MODE_PS2);
            #if WITH_GUI
            gui_request_refresh();
            #endif
            break;
        case SERIAL_INPUT_CMD_PS2_VARIANT_SC2:
            printf("Set PS2 Variant: Conquest\n");
            settings_set_ps2_variant(PS2_VARIANT_SC2);
            settings_set_mode(MODE_PS2);
            #if WITH_GUI
            gui_request_refresh();
            #endif
            break;
        case SERIAL_INPUT_CMD_PS2_VARIANT_COH:
            printf("Set PS2 Variant: Arcade\n");
            settings_set_ps2_variant(PS2_VARIANT_COH);
            settings_set_mode(MODE_PS2);
            #if WITH_GUI
            gui_request_refresh();
            #endif
            break;
#if PS1_MC_TUNING
        case SERIAL_INPUT_CMD_TUNE:
            ps1_mc_tune_set(cmd_data->gameid, (uint32_t)cmd_data->idx);
            /* Si rilegge SEMPRE tutto, non solo il valore appena scritto:
               e' il controllo che mancava e che e' costato tre prove
               annotate con parametri diversi da quelli reali. */
            ps1_mc_tune_print();
            break;

        case SERIAL_INPUT_CMD_TUNE_SHOW:
            ps1_mc_tune_print();
            break;
        case SERIAL_INPUT_CMD_MCWRITES:
            ps1_mc_writes_print();
            break;
        case SERIAL_INPUT_CMD_MCGET:
            ps1_mc_get_print();
            break;
        case SERIAL_INPUT_CMD_MCRING:
            ps1_mc_ring_print();
            break;
        case SERIAL_INPUT_CMD_MCCLEAR:
            ps1_mc_counters_clear();
            break;
#endif
#if PS1_MMCE_TRACE
        case SERIAL_INPUT_CMD_MMCELOG:
            ps1_mc_mmcelog_print();
            break;
#endif

        case SERIAL_INPUT_CMD_START_EMULATION:
#if WITH_MSC
            /* Tre stati, tre risposte. Prima se ne distinguevano due - "in
               attesa" e "tutto il resto" - e "tutto il resto" comprendeva anche
               il passthrough, dove l'emulazione NON gira: il comando rispondeva
               "already running" proprio quando non era vero. */
            switch (msc_mode_state()) {
                case MSC_STATE_PASSTHROUGH:
                    printf("volume montato, smonta il volume per continuare\n");
                    break;
                case MSC_STATE_HELD:
                    printf("Starting emulation\n");
                    msc_mode_request_emulation();
                    break;
                default:
                    printf("Emulation already running\n");
                    break;
            }
#else
            printf("Emulation already running\n");
#endif
            break;
        case SERIAL_INPUT_CMD_STATUS:
            print_status();
            break;
        case SERIAL_INPUT_CMD_HELP:
            printf("%s", help_text);
            break;
    }
}

static void submit_line(void) {
    char parse_buffer[SERIAL_INPUT_BUFFER_SIZE];
    serial_input_cmd_data_t cmd_data;

    printf("\r\n");

    if (in_len == 0) {
        terminal_prompt();
        return;
    }

    memcpy(parse_buffer, in_buffer, in_len + 1);
    parse_command(parse_buffer, &cmd_data);

    in_len = 0;
    in_buffer[0] = '\0';

    execute_command(&cmd_data);
    terminal_prompt();
}

void serial_input_process(void) {
    int charin;

    if (!prompt_started) {
        terminal_prompt();
    } else if (prompt_needs_redraw && (in_len > 0)) {
        terminal_redraw_input_line();
    } else {
        prompt_needs_redraw = false;
    }

    charin = getchar_timeout_us(0);
    while (charin != PICO_ERROR_TIMEOUT) {
        if ((charin == '\n') && ignore_next_lf) {
            ignore_next_lf = false;
        } else if ((charin == '\n') || (charin == '\r')) {
            ignore_next_lf = (charin == '\r');
            submit_line();
        } else if ((charin == ASCII_BS) || (charin == ASCII_DEL)) {
            if (in_len > 0) {
                --in_len;
                in_buffer[in_len] = '\0';
                terminal_erase_last_char();
            }
        } else if ((charin >= 0x20) && (charin <= 0x7E)) {
            if (in_len < sizeof(in_buffer) - 1) {
                in_buffer[in_len++] = (char)charin;
                in_buffer[in_len] = '\0';
                printf("%c", (char)charin);
            } else {
                printf("\a");
            }
        }

        charin = getchar_timeout_us(0);
    }
}
