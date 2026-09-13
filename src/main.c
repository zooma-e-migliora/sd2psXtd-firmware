#include <serial_input.h>

#if WITH_LED
#include "led.h"
#endif
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "flashmap.h"
#include "hardware/clocks.h"
#include "hardware/structs/bus_ctrl.h"

#include "input.h"
#include "config.h"
#include "debug.h"
#include "pico/time.h"
#include "sd.h"
#include "settings.h"
#if WITH_PSRAM
#include "psram/psram.h"
#endif

#include "ps2.h"
#include "ps1.h"
#include "ps1/ps1_cardman.h"

#if WITH_MSC
#include "usb/msc_mode.h"
#include "usb/usb_device.h"
#endif

#include "game_db/game_db.h"
#include "version.h"


uint flash_capacity = 0;
uint flash_border = 0;


/* reboot to bootloader if either button is held on startup
   to make the device easier to flash when assembled inside case */
static void check_bootloader_reset(void) {
    /* make sure at least DEBOUNCE interval passes or we won't get inputs */
    for (int i = 0; i < 2 * DEBOUNCE_MS; ++i) {
        input_task();
        sleep_ms(1);
    }

    if (input_is_down_raw(0) || input_is_down_raw(1))
        reset_usb_boot(0, 0);
}

/* La modalita' EFFETTIVAMENTE in esecuzione, che non coincide sempre con quella
   configurata: settings_set_mode() cambia solo l'impostazione, e il passaggio
   avviene quando il lato in esecuzione accetta di uscire - ps2_task() lo fa solo
   se ps2_cardman_is_idle(). Senza esporla, un "set mode ps1" che resta appeso e'
   indistinguibile da uno andato a buon fine. La legge il comando "status". */
static volatile int running_mode = MODE_PS1;

int main_get_running_mode(void) {
    return running_mode;
}

static void debug_task(void) {

#if WITH_MSC
    /* Con lo stack USB proprio, TinyUSB avanza a polling: questo e' il punto
       piu' frequentato del main loop in entrambe le modalita'. */
    usb_device_task();
#endif

    bool wrote_debug_output = false;
    while (true) {
        char ch = debug_get();
        if (ch) {
            if (!wrote_debug_output) {
                serial_input_begin_output();
            }
            wrote_debug_output = true;
            #if DEBUG_USB_UART
                putchar(ch);
            #elif WITH_MSC
                /* putchar() passa da stdio, che inoltra a tutti i driver
                   attivi: UART *e* CDC USB. Cosi' il log si puo' leggere
                   anche da PC, senza dover accedere ai pin della UART. */
                putchar(ch);
            #else
                if (ch == '\n')
                    uart_putc_raw(UART_PERIPH, '\r');
                uart_putc_raw(UART_PERIPH, ch);
            #endif
        } else {
            break;
        }
    }

    if (wrote_debug_output) {
        serial_input_notify_output();
    }
    serial_input_process();
}



int main() {
    int mhz = 240;
    input_init();
    update_flash_capacity();
    check_bootloader_reset();

    QPRINTF("prepare...\n");

    set_sys_clock_khz(mhz * 1000, true);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, mhz * 1000000, mhz * 1000000);

#if WITH_MSC
    /* Lo stack USB proprio (CDC + MSC) viene avviato piu' avanti, dopo
       sd_init(): l'unita' di massa ha bisogno della scheda gia' montata. */
    stdio_uart_init_full(UART_PERIPH, UART_BAUD, UART_TX, UART_RX);
#elif DEBUG_USB_UART
    stdio_usb_init();
#else
    stdio_uart_init_full(UART_PERIPH, UART_BAUD, UART_TX, UART_RX);
#endif

#if WITH_LED
    /* Prima di settings_init() e sd_init(): entrambi possono chiamare fatal(),
       e senza i LED gia' inizializzati quell'errore non accenderebbe nulla.
       led_init() ha bisogno solo dei GPIO, quindi puo' stare qui. */
    led_init();
    led_signal_boot();
#endif

    /* set up core1 as high priority bus access */
    bus_ctrl_hw->priority |= BUSCTRL_BUS_PRIORITY_PROC1_BITS;
    while (!bus_ctrl_hw->priority_ack) {}

    QPRINTF("\n\n\nStarted! Clock %d; bus priority 0x%X\n", (int)clock_get_hz(clk_sys), (unsigned)bus_ctrl_hw->priority);
    QPRINTF("SD2PSX Version %s\n", sd2psx_version);
    QPRINTF("SD2PSX HW Variant: %s\n", sd2psx_variant);
    QPRINTF("Flash size: %d MB\n", flash_capacity / (1024 * 1024));
    QPRINTF("EEPROM base: 0x%X\n", WEAR_LEVELING_RP2040_FLASH_BASE);
    QPRINTF("CIV base: 0x%X\n", FLASH_OFF_CIV);
    QPRINTF("Splash base: 0x%X\n", FLASH_OFF_SPLASH);

    settings_init();
#if WITH_PSRAM
    psram_init();
#endif
    game_db_init();

    sd_init();

    /* Prima del bivio fra passthrough ed emulazione: l'albero delle cartelle e
       settings.ini nascevano solo dentro l'emulazione, quindi chi collegava la
       scheda al PC senza averla mai messa nella console trovava la microSD
       vuota. Qui invece esistono gia' al primo collegamento.

       settings_load_sd() fa "leggi se c'e', scrivi se non c'e'". Chiamarla ora
       ha anche l'effetto di rendere effettive subito le modifiche fatte
       all'ini dal PC: girando solo dopo ps1_cardman_init(), come faceva prima,
       serviva un riavvio in piu' perche' il valore passasse per la flash. */
    ps1_cardman_ensure_base_layout();
    settings_load_sd();

#if WITH_MSC
    /* Se all'accensione siamo collegati a un PC, la microSD viene esposta come
       unita' di massa. Si torna qui quando l'host espelle o stacca il cavo. */
    msc_mode_try_enter();
    /* Dopo un'espulsione con il cavo ancora attaccato non si prosegue subito:
       si aspetta "start emulation" dalla seriale. Senza host ritorna subito. */
    msc_mode_wait_for_start();
#endif

    while (1) {
#if WITH_LED
        /* Secondo lampo: la microSD e' stata montata e si entra in emulazione.
           Se non arriva, il firmware e' partito ma si e' fermato prima. */
        led_signal_mode();
#endif
        if (settings_get_mode(true) == MODE_PS2) {
            QPRINTF("Starting PS2 mode...\n");
            running_mode = MODE_PS2;
            ps2_init();
            settings_load_sd();
#if WITH_MSC
            /* Lo stack composito e' gia' su da msc_mode_try_enter(): questa e'
               una no-op che si limita a riattaccare il device al bus. */
            usb_device_init();
#elif DEBUG_USB_UART == 0
            stdio_usb_init();
#endif
            do {
                debug_task();
            } while(ps2_task());
            ps2_deinit();

        } else {
            QPRINTF("Starting PS1 mode...\n");
            running_mode = MODE_PS1;
            ps1_init();
            settings_load_sd();

#if WITH_MSC
            /* Lo stack composito e' gia' su da msc_mode_try_enter(): questa e'
               una no-op che si limita a riattaccare il device al bus. */
            usb_device_init();
#elif DEBUG_USB_UART == 0
            stdio_usb_init();
#endif
            do {
                debug_task();
            } while(ps1_task());
            ps1_deinit();
        }
#if WITH_MSC
        /* Con MSC lo stack USB resta attivo anche fra un cambio di modalita' e
           l'altro: la CDC dei comandi non deve cadere. */
#elif DEBUG_USB_UART == 0
        stdio_usb_deinit();
#endif
    }
}
