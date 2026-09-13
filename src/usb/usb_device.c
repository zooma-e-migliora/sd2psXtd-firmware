/* Stack USB del dispositivo composito CDC + MSC.
 *
 * Sostituisce pico_stdio_usb, che puo' esporre solo CDC e i cui
 * tud_descriptor_*_cb non sono weak (quindi non e' possibile affiancargli
 * descrittori propri). Qui teniamo:
 *   - i descrittori compositi        -> usb_descriptors.c
 *   - il passthrough microSD          -> msc_disk.c
 *   - un driver stdio su CDC          -> questo file
 * cosi' printf()/getchar_timeout_us() e l'interfaccia comandi seriale di
 * serial_input.c continuano a funzionare come prima.
 *
 * tud_task() viene chiamato a polling da usb_device_task(). E' lo stesso
 * schema di PicoMemcard, ed e' compatibile con il main loop di sd2psXtd:
 * ps1_task()/ps2_task() non bloccano (l'emulazione vera gira su core1).
 */

#include "usb/usb_device.h"

#include "pico/bootrom.h"
#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/time.h"
#include "tusb.h"

static bool usb_started = false;

/* tud_task() non e' rientrante. Una printf() emessa da dentro una callback
   TinyUSB finirebbe in usb_stdio_out_chars(), che a buffer pieno vorrebbe far
   avanzare lo stack: qui teniamo traccia di quando siamo gia' dentro. */
static bool in_tud_task = false;

static void usb_pump(void) {
    if (in_tud_task)
        return;
    in_tud_task = true;
    tud_task();
    in_tud_task = false;
}

/* --- driver stdio su CDC ------------------------------------------------- */

/* Un host collegato ma che non svuota il buffer non deve poter bloccare il
   firmware: oltre questo tempo l'output residuo viene scartato. */
#define USB_STDIO_WRITE_TIMEOUT_MS 20

static void usb_stdio_out_chars(const char *buf, int len) {
    if (!tud_cdc_connected())
        return;

    const absolute_time_t deadline = make_timeout_time_ms(USB_STDIO_WRITE_TIMEOUT_MS);

    int sent = 0;
    while (sent < len) {
        if (time_reached(deadline))
            return;

        const uint32_t avail = tud_cdc_write_available();
        if (avail == 0) {
            /* Buffer pieno. Se possiamo, facciamo avanzare lo stack e
               riproviamo; se siamo dentro una callback TinyUSB non possiamo,
               e scartare e' preferibile a bloccare il firmware. Lo stesso vale
               se nel frattempo l'host si e' disconnesso. */
            tud_cdc_write_flush();
            if (in_tud_task)
                return;
            usb_pump();
            if (!tud_cdc_connected())
                return;
            continue;
        }

        uint32_t chunk = (uint32_t)(len - sent);
        if (chunk > avail)
            chunk = avail;

        sent += (int)tud_cdc_write(buf + sent, chunk);
    }

    tud_cdc_write_flush();
}

static void usb_stdio_out_flush(void) {
    if (tud_cdc_connected())
        tud_cdc_write_flush();
}

static int usb_stdio_in_chars(char *buf, int len) {
    if (!tud_cdc_connected() || !tud_cdc_available())
        return PICO_ERROR_NO_DATA;

    const uint32_t count = tud_cdc_read(buf, (uint32_t)len);
    return count ? (int)count : PICO_ERROR_NO_DATA;
}

static stdio_driver_t usb_stdio_driver = {
    .out_chars = usb_stdio_out_chars,
    .out_flush = usb_stdio_out_flush,
    .in_chars = usb_stdio_in_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = PICO_STDIO_DEFAULT_CRLF,
#endif
};

/* --- ciclo di vita ------------------------------------------------------- */

void usb_device_init(void) {
    if (usb_started) {
        tud_connect();
        return;
    }

    tusb_init();
    stdio_set_driver_enabled(&usb_stdio_driver, true);
    usb_started = true;
}

void usb_device_deinit(void) {
    if (!usb_started)
        return;

    stdio_set_driver_enabled(&usb_stdio_driver, false);
    tud_disconnect();
    usb_pump();
}

void usb_device_task(void) {
    if (usb_started)
        usb_pump();
}

bool usb_device_mounted(void) {
    return usb_started && tud_mounted();
}

bool usb_device_cdc_connected(void) {
    return usb_started && tud_cdc_connected();
}

/* Reset in BOOTSEL aprendo la porta CDC a 1200 baud e richiudendola: e' la
   convenzione Arduino/RP2040. Su questa board non ci sono pulsanti fisici,
   quindi e' l'unico modo comodo per rientrare nel bootloader. */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *p_line_coding) {
    (void)itf;
    if (p_line_coding->bit_rate == 1200)
        reset_usb_boot(0, 0);
}
