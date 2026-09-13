
#include <input.h>
#include "pico/time.h"
#include "hardware/timer.h"
#include "config.h"
#include "ps1_mc_data_interface.h"
#include "ps1_memory_card.h"
#include "ps1_cardman.h"
#include "ps1_mmce.h"
#include "debug.h"
#include "game_db/game_db.h"
#include "settings.h"

#if WITH_GUI
#include <gui.h>
#endif
#include <string.h>

#define CARD_SWITCH_DELAY_MS    (250)
#define MAX_GAME_ID_LENGTH   (16)

#if LOG_LEVEL_PS1_MMCE == 0
#define log(x...)
#else
#define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_PS1_MMCE, level, fmt, ##x)
#endif
static volatile uint8_t mmce_command;
static volatile uint16_t mmce_cnum;
static volatile uint16_t mmce_chn;
static uint64_t mmce_switching_timeout = 0;

static char received_game_id[MAX_GAME_ID_LENGTH];

/* Uscita a tempo dalla BootCard: passati i secondi indicati da BootCardTimeout
   in settings.ini si torna alla card predefinita. Con -1 (il default) non
   succede niente e si esce solo su iniziativa del software: il Game ID che il
   payload manda appena parte fa scattare il ramo "else" di
   ps1_mmce_set_gameid() e riporta alla card predefinita.

   E' un timeout di INATTIVITA', non assoluto, e la differenza non e' una
   sottigliezza: un conteggio assoluto puo' scadere mentre l'exploit sta ancora
   leggendo la card, e allora la card viene sfilata fra un frame e l'altro,
   sparisce per i 250 ms del cambio e torna come card diversa. Il payload aspetta
   l'ACK con waitCardIRQ(), che non ha timeout: la console non fallisce, si
   pianta e basta. Misurando invece il tempo dall'ULTIMO comando ricevuto, mentre
   l'exploit legge senza sosta il conteggio non arriva mai in fondo, e scade solo
   quando la card resta davvero in silenzio.

   Il riferimento e' l'ingresso nella boot card, spostato in avanti da ogni
   comando. Confronti in aritmetica a 32 bit con segno, per reggere l'avvolgimento
   del timer ogni ~71 minuti. */
static void bootcard_timeout_task(void) {
    static uint32_t idle_since_us;           /* 0 = non siamo sulla BootCard */

#if PS1_MC_TUNING
    const uint8_t forced = ps1_mc_tune_boottimeout();
    const uint8_t timeout_s = (forced != 255u) ? forced
                                              : settings_get_ps1_bootcard_timeout();
#else
    const uint8_t timeout_s = settings_get_ps1_bootcard_timeout();
#endif

    if (timeout_s == SETTINGS_BOOTCARD_TIMEOUT_NEVER
        || ps1_cardman_get_state() != PS1_CM_STATE_BOOT) {
        idle_since_us = 0;
        return;
    }

    const uint32_t now = timer_hw->timerawl;

    if (idle_since_us == 0) {
        idle_since_us = now;
        return;
    }

    /* Ogni comando alla card rimanda indietro il conteggio. */
    const uint32_t last = ps1_mc_last_activity_us();
    if ((int32_t)(last - idle_since_us) > 0)
        idle_since_us = last;

    if ((now - idle_since_us) >= (uint32_t)timeout_s * 1000000u) {
        /* Azzerato subito: il cambio card non e' immediato e senza questo si
           accumulerebbero piu' richieste in attesa. */
        idle_since_us = 0;
        DPRINTF("BootCard: card ferma da %u s, passo alla predefinita\n", timeout_s);
        ps1_mmce_switch_default(false);
    }
}

void ps1_mmce_task(void) {
    if (mmce_command != 0U) {
#if PS1_MMCE_TRACE
        /* Il comando che sta per essere ESEGUITO, qualunque sia l'origine: filo
           o combo del dispositivo. Confrontandolo con gli eventi "ricevuto" si
           capisce da dove e' arrivato. */
        ps1_mc_mmcelog_put(MMCELOG_CMD_RUN, (uint8_t)mmce_command, 0);
#endif
        switch (mmce_command) {
            case MMCE_PS1_GAME_ID: {
                DPRINTF("Received Game ID: %s\n", received_game_id);
                game_db_update_game(received_game_id);
                game_db_get_current_parent(received_game_id);
                ps1_cardman_set_game_id(received_game_id);
                break;
            }
            case MMCE_PS1_NXT_CARD:
                DPRINTF("Received next card.\n");
                ps1_cardman_next_idx();
                break;
            case MMCE_PS1_PRV_CARD:
                DPRINTF("Received prev card.\n");
                ps1_cardman_prev_idx();
                break;
            case MMCE_PS1_NXT_CH:
                DPRINTF("Received next chan.\n");
                ps1_cardman_next_channel();
                break;
            case MMCE_PS1_PRV_CH:
                DPRINTF("Received prev chan.\n");
                ps1_cardman_prev_channel();
                break;
            case MMCE_PS1_SWITCH_BOOTCARD:
                DPRINTF("Received switch boot card.\n");
                ps1_cardman_switch_bootcard();
                break;
            case MMCE_PS1_SWITCH_DEFAULT:
                DPRINTF("Received switch default card.\n");
                ps1_cardman_switch_default();
                break;
            case MMCE_PS1_SET_CARD:
                DPRINTF("Received set card index: %d\n", mmce_cnum);
                ps1_cardman_set_idx(mmce_cnum);
                break;
            case MMCE_PS1_SET_CHANNEL:
                DPRINTF("Received set channel index: %d\n", mmce_chn);
                ps1_cardman_set_channel(mmce_chn);
                break;
            case MMCE_PS1_RESET:
                DPRINTF("Received reset command.\n");
                if (settings_get_mode(false) == MODE_PS2) {
                    settings_set_mode(MODE_PS2);
                } else {
                    ps1_cardman_switch_default();
                }
                break;
            default:
                DPRINTF("Invalid ODE Command received.");
                break;
        }

        mmce_command = 0;
    }

    /* Dopo il dispatch: se c'e' gia' un comando in coda si aspetta il giro
       successivo, invece di sovrascriverlo. */
    if (mmce_command == 0U)
        bootcard_timeout_task();

    if ((mmce_switching_timeout < time_us_64())
        && !input_is_any_down()
        && (ps1_cardman_needs_update())) {

        ps1_memory_card_exit();
        ps1_mc_data_interface_flush();
        ps1_cardman_close();
#ifdef WITH_GUI
        gui_do_ps1_card_switch();
#endif

        sleep_ms(CARD_SWITCH_DELAY_MS); // This delay is required, so ODE can register the card change

        ps1_cardman_open();
        ps1_memory_card_enter();
#if PS1_MMCE_TRACE
        ps1_mc_mmcelog_put(MMCELOG_SWAP_DONE, (uint8_t)ps1_cardman_get_state(),
                           (uint8_t)ps1_cardman_get_idx());
#endif
#ifdef WITH_GUI
        gui_request_refresh();
#endif
    }
}


bool __time_critical_func(ps1_mmce_set_gameid)(const uint8_t* const game_id) {
    char sanitized_game_id[11] = {0};
    bool ret = false;
    log(LOG_INFO, "Raw Game ID: %s\n", game_id);

    game_db_extract_title_id(game_id, sanitized_game_id, UINT8_MAX, sizeof(sanitized_game_id));
    log(LOG_INFO, "Game ID: %s\n", sanitized_game_id);
    if (game_db_sanity_check_title_id(sanitized_game_id)) {
        snprintf(received_game_id, sizeof(received_game_id), "%s", sanitized_game_id);
        mmce_command = MMCE_PS1_GAME_ID;
        ret = true;
#if PS1_MMCE_TRACE
        ps1_mc_mmcelog_put(MMCELOG_GID_VALID, 0, 0);
#endif
    } else if ((game_id[0] != 0x00) && (ps1_cardman_get_idx() == PS1_CARD_IDX_SPECIAL)) {
        mmce_command = MMCE_PS1_SWITCH_DEFAULT;
#if PS1_MMCE_TRACE
        ps1_mc_mmcelog_put(MMCELOG_GID_SWITCH, 0, 0);
#endif
#if PS1_MMCE_TRACE
    } else {
        /* Terzo caso, senza codice associato: ID vuoto, oppure non vuoto ma
           mentre siamo su una card normale. Non fa scattare nulla, ed e' il
           sospettato numero uno per la boot card che non viene lasciata. */
        ps1_mc_mmcelog_put(MMCELOG_GID_NONE, 0, 0);
#endif
    }
    return ret;
}

const char* ps1_mmce_get_gameid(void) {
    return received_game_id;
}

void ps1_mmce_next_ch(bool delay) {
    mmce_switching_timeout = time_us_64() + (delay ? 1500 * 1000 : 0);
    mmce_command = MMCE_PS1_NXT_CH;
}

void ps1_mmce_prev_ch(bool delay) {
    mmce_switching_timeout = time_us_64() + (delay ? 1500 * 1000 : 0);
    mmce_command = MMCE_PS1_PRV_CH;
}

void ps1_mmce_next_idx(bool delay) {
    mmce_switching_timeout = time_us_64() + (delay ? 1500 * 1000 : 0);
    mmce_command = MMCE_PS1_NXT_CARD;
}

void ps1_mmce_prev_idx(bool delay) {
    mmce_switching_timeout = time_us_64() + (delay ? 1500 * 1000 : 0);
    mmce_command = MMCE_PS1_PRV_CARD;
}

void ps1_mmce_switch_bootcard(bool delay) {
    mmce_switching_timeout = time_us_64() + (delay ? 1500 * 1000 : 0);
    mmce_command = MMCE_PS1_SWITCH_BOOTCARD;
}

void ps1_mmce_switch_default(bool delay) {
    mmce_switching_timeout = time_us_64() + (delay ? 1500 * 1000 : 0);
    mmce_command = MMCE_PS1_SWITCH_DEFAULT;
}

void ps1_mmce_set_card(uint16_t cnum, bool delay) {
    mmce_switching_timeout = time_us_64() + (delay ? 1500 * 1000 : 0);
    mmce_cnum = cnum;
    mmce_command = MMCE_PS1_SET_CARD;
}

void ps1_mmce_set_channel(uint16_t chn, bool delay) {
    mmce_switching_timeout = time_us_64() + (delay ? 1500 * 1000 : 0);
    mmce_chn = chn;
    mmce_command = MMCE_PS1_SET_CHANNEL;
}

void ps1_mmce_reset(bool delay) {
    mmce_switching_timeout = time_us_64() + (delay ? 1500 * 1000 : 0);
    mmce_command = MMCE_PS1_RESET;
}