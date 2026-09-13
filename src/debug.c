#include "debug.h"

#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>

#if WITH_LED
#include "led.h"
#endif

#if WITH_GUI
#include "oled.h"
#endif
#include "hardware/watchdog.h"
#include "pico/platform.h"
#include "pico/time.h"
#include "sd.h"

const char *log_level_str[] = {
    " ",
    "[ERROR]",
    "[WARN] ",
    "[INFO] ",
    "[TRACE]"
};

/* Ogni quanto ritentare il mount della microSD quando manca all'avvio. */
#define SD_RETRY_MS 1000

static char debug_queue[1024];
static size_t debug_read_pos, debug_write_pos;

void __time_critical_func(debug_put)(char c) {
    debug_queue[debug_write_pos] = c;
    debug_write_pos = (debug_write_pos + 1) % sizeof(debug_queue);
}

char debug_get(void) {
    char ret = debug_queue[debug_read_pos];
    if (ret) {
        debug_queue[debug_read_pos] = 0;
        debug_read_pos = (debug_read_pos + 1) % sizeof(debug_queue);
    }
    return ret;
}

void __time_critical_func(buffered_printf)(const char *format, ...) {
    char buf[128];

    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    for (char *c = buf; *c; ++c)
        debug_put(*c);
}

void fatal(int err, const char *format, ...) {
    char buf[128];

    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    printf("(%i) %s\n", err, buf);
#if WITH_GUI
    static int fatal_reentry;
    if (!fatal_reentry) {
        fatal_reentry = 1;
        oled_init();
        oled_clear();
        oled_draw_text("FATAL ERROR\n\n");
        oled_draw_text(buf);
        oled_show();
    }
#endif
    /* microSD assente e' l'unico errore che l'utente puo' risolvere senza
       togliere corrente: invece di restare bloccati per sempre si continua a
       lampeggiare rosso e si ritenta il mount. Appena la scheda compare si
       riparte da zero, che e' anche il modo piu' sicuro di montarla. */
    if (err == ERR_SD_ABSENT) {
        absolute_time_t next_try = make_timeout_time_ms(SD_RETRY_MS);

        while (true) {
#if WITH_LED
            led_error_tick(err);
#endif
            if (time_reached(next_try)) {
                if (sd_try_mount()) {
                    printf("microSD rilevata, riavvio\n");
                    watchdog_reboot(0, 0, 0);
                }
                next_try = make_timeout_time_ms(SD_RETRY_MS);
            }
            busy_wait_us_32(2000);
        }
    }

#if WITH_LED
    led_error_forever(err);
#endif
    while (true) {
        tight_loop_contents();
    }

}

/* Errori non fatali: l'esecuzione prosegue, ma il primo errore rilevato resta
   memorizzato fino al reset perche' i LED possano segnalarlo. */
static volatile int latched_error;

void error_latch_set(int err) {
    if (latched_error == 0)
        latched_error = err;
}

int error_latch_get(void) {
    return latched_error;
}

void hexdump(const uint8_t *buf, size_t sz) {
    for (size_t i = 0; i < sz; ++i)
        printf("%02X ", buf[i]);
    printf("\n");
}
