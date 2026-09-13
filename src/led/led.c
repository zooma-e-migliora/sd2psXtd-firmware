#include "led.h"
#include <stdbool.h>
#include <stdint.h>
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "pico/time.h"
#include "debug.h"
#include "ps1/ps1_mc_data_interface.h"
#include "ps2/card_emu/ps2_mc_data_interface.h"
#include "ps1/ps1_dirty.h"
#include "ps2/mmceman/ps2_mmceman_fs.h"
#include "ps2/ps2_cardman.h"
#include "ps1/ps1_cardman.h"
#include "ps1/ps1_memory_card.h"
#include "settings.h"
#include "ws2812.pio.h"

#define LED_REFRESH_US  (250 * 1000)    // 250 ms

#ifdef WS2812
    #define WS2812_PIN      (16U)
    uint offsetWs2813;
    uint smWs2813;

void ws2812_put_pixel(uint32_t pixel_grb) {
    sleep_ms(1);    // delay to ensure LED latch will hold data
    pio_sm_put(pio1, smWs2813, pixel_grb << 8u);
}

void ws2812_put_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    uint32_t mask = (green << 16) | (red << 8) | (blue << 0);
    ws2812_put_pixel(mask);
}
#else
/* Pin e presenza dei canali sono sovrascrivibili dalla variante: non tutte le
   board hanno un LED RGB. Un canale con LED_HAS_x=0 non viene nemmeno
   inizializzato, cosi' non si pilotano pin di cui non si conosce il
   collegamento. La polarita' e' per canale: alcune board cablano il LED fra
   VCC e GPIO, e allora si accende con il pin a livello basso. */
#ifndef LED_R
#define LED_R           (25U)
#endif
#ifndef LED_G
#define LED_G           (24U)
#endif
#ifndef LED_B
#define LED_B           (23U)
#endif

#ifndef LED_HAS_G
#define LED_HAS_G       1
#endif
#ifndef LED_HAS_B
#define LED_HAS_B       1
#endif

#ifndef LED_R_ACTIVE_LOW
#define LED_R_ACTIVE_LOW 0
#endif
#ifndef LED_G_ACTIVE_LOW
#define LED_G_ACTIVE_LOW 0
#endif
#ifndef LED_B_ACTIVE_LOW
#define LED_B_ACTIVE_LOW 0
#endif

/* Con LED_PWM=1 i tre canali vanno in PWM e i colori intermedi diventano
   possibili (arancione, viola, luminosita' ridotta). Spento, il driver
   arrotonda a acceso/spento e si comporta come sempre.

   LED_GAIN_* compensa il fatto che su un RGB il verde e il blu rendono molto
   piu' del rosso a parita' di corrente: senza correzione il giallo tira al
   verde e il bianco all'azzurro. LED_BRIGHTNESS scala tutto insieme. */
#ifndef LED_PWM
#define LED_PWM         0
#endif
#ifndef LED_GAIN_R
#define LED_GAIN_R      255
#endif
#ifndef LED_GAIN_G
#define LED_GAIN_G      255
#endif
#ifndef LED_GAIN_B
#define LED_GAIN_B      255
#endif
#ifndef LED_BRIGHTNESS
#define LED_BRIGHTNESS  255
#endif

#if LED_PWM
#include "hardware/pwm.h"

/* Livello PWM del canale: valore * guadagno * luminosita', piu' l'inversione
   se il LED e' cablato fra VCC e GPIO. */
static inline uint16_t led_level(uint8_t value, uint16_t gain, bool active_low) {
    uint32_t duty = ((uint32_t)value * gain * LED_BRIGHTNESS) / (255U * 255U);
    if (active_low)
        duty = 255U - duty;
    return (uint16_t)duty;
}

static void led_pwm_init(uint pin) {
    gpio_set_function(pin, GPIO_FUNC_PWM);
    const uint slice = pwm_gpio_to_slice_num(pin);
    /* Wrap a 255 per far coincidere il livello con il valore a 8 bit; il
       divisore porta la portante intorno ai 2 kHz, ben oltre il visibile. */
    pwm_set_wrap(slice, 255);
    pwm_set_clkdiv(slice, 255.f);
    pwm_set_enabled(slice, true);
}
#endif  /* LED_PWM */

static inline void led_put_r(bool on) { gpio_put(LED_R, LED_R_ACTIVE_LOW ? !on : on); }

static inline void led_put_g(bool on) {
#if LED_HAS_G
    gpio_put(LED_G, LED_G_ACTIVE_LOW ? !on : on);
#else
    (void)on;
#endif
}

static inline void led_put_b(bool on) {
#if LED_HAS_B
    gpio_put(LED_B, LED_B_ACTIVE_LOW ? !on : on);
#else
    (void)on;
#endif
}
#endif  /* !WS2812 */

#if !LED_STYLE_ACTIVITY
static uint64_t last_refresh = 0;
#endif

void led_init(void) {
#if WS2812
    offsetWs2813 = pio_add_program(pio1, &ws2812_program);
    smWs2813 = pio_claim_unused_sm(pio1, true);
    ws2812_program_init(pio1, smWs2813, offsetWs2813, 16, 800000, true);
#elif LED_PWM
    led_pwm_init(LED_R);
#if LED_HAS_G
    led_pwm_init(LED_G);
#endif
#if LED_HAS_B
    led_pwm_init(LED_B);
#endif
    led_clear();
#else
    gpio_init(LED_R);
    gpio_set_dir(LED_R, true);
#if LED_HAS_G
    gpio_init(LED_G);
    gpio_set_dir(LED_G, true);
#endif
#if LED_HAS_B
    gpio_init(LED_B);
    gpio_set_dir(LED_B, true);
#endif
    led_clear();
#endif
}

void led_set_color(led_color_t c) {
#if WS2812
    ws2812_put_rgb(c.r, c.g, c.b);
#elif LED_PWM
    pwm_set_gpio_level(LED_R, led_level(c.r, LED_GAIN_R, LED_R_ACTIVE_LOW));
#if LED_HAS_G
    pwm_set_gpio_level(LED_G, led_level(c.g, LED_GAIN_G, LED_G_ACTIVE_LOW));
#endif
#if LED_HAS_B
    pwm_set_gpio_level(LED_B, led_level(c.b, LED_GAIN_B, LED_B_ACTIVE_LOW));
#endif
#else
    /* Senza sfumature: meta' scala fa da soglia. */
    led_put_r(c.r >= 128);
    led_put_g(c.g >= 128);
    led_put_b(c.b >= 128);
#endif
}

void led_fatal(void) {
    led_set_color(LED_RED);
}

void led_clear(void) {
    led_set_color(LED_OFF);
}

void led_set_rgb(bool red, bool green, bool blue) {
    led_set_color((led_color_t){ red ? 255 : 0, green ? 255 : 0, blue ? 255 : 0 });
}

#if LED_STYLE_ACTIVITY

/* ------------------------------------------------------------------------
 * Stile "attivita' + errore", per board con un LED RGB.
 *
 * Sette stati, sette colori, uno ciascuno:
 *
 *   rosso    errore. I tre casi che contano si distinguono dal ritmo:
 *            microSD assente (lento), microSD illeggibile (veloce),
 *            errore runtime (N lampi = codice).
 *   ciano    lettura della memory card
 *   blu      scrittura sulla memory card
 *   magenta  modalita' di cambio card attiva, acceso fisso
 *   giallo   cambio di card o di canale, N lampi contabili
 *   verde    ingresso nella boot card
 *   bianco   pronto all'avvio (lampo), oppure passthrough USB MSC (fisso)
 *
 * A riposo e senza errori il LED e' spento.
 *
 * Nessuna funzione qui dentro blocca, a parte le due segnalazioni di avvio:
 * led_task() e' chiamata dal loop di ps1_task()/ps2_task(), che deve restare
 * reattivo. Le sequenze sono una coda di impulsi consumata un pezzo per volta.
 * --------------------------------------------------------------------- */

#define ACTIVITY_HOLD_US    (300 * 1000)    /* quanto "dura" un accesso ai fini del lampeggio */
#define BLINK_READ_US       (250 * 1000)    /* semiperiodo lampeggio in lettura */
#define BLINK_WRITE_US      ( 60 * 1000)    /* semiperiodo lampeggio in scrittura */

#define PULSE_SHORT_US      (120 * 1000)
#define PULSE_LONG_US       (500 * 1000)
#define PULSE_GAP_US        (180 * 1000)
#define PULSE_TAIL_US       (400 * 1000)    /* pausa finale, separa due sequenze */

#define BOOT_SWEEP_US       (120 * 1000)    /* durata di ogni colore nello sweep di avvio */

#define MAX_PULSES          24

/* Coda di impulsi. Ogni elemento e' una durata; gli elementi di indice pari
   sono acceso, quelli dispari spento. Il colore vale per tutta la sequenza e
   lo imposta chi la costruisce. */
static uint16_t pulse_ms[MAX_PULSES];
static uint8_t pulse_count;
static uint8_t pulse_pos;
static uint64_t pulse_started_us;
static led_color_t pulse_color;

/* Colore delle fasi spente. Di norma e' il colore di fondo, cosi' i lampi si
   staccano da cio' che c'era prima; ma una sequenza il cui colore coincide col
   fondo sarebbe invisibile, e allora serve imporre uno sfondo proprio. */
static led_color_t pulse_off_color;
static bool pulse_off_is_base;

static void pulse_reset(void) {
    pulse_count = 0;
    pulse_pos = 0;
    pulse_started_us = 0;
    pulse_off_is_base = true;
}

static void pulse_add(uint32_t on_us, uint32_t off_us) {
    if (pulse_count + 2 > MAX_PULSES)
        return;
    pulse_ms[pulse_count++] = (uint16_t)(on_us / 1000);
    pulse_ms[pulse_count++] = (uint16_t)(off_us / 1000);
}

static void pulse_add_n(uint8_t n, uint32_t on_us) {
    for (uint8_t i = 0; i < n; i++)
        pulse_add(on_us, PULSE_GAP_US);
}

/* Restituisce true se una sequenza e' in corso, e in *on lo stato del LED. */
static bool pulse_active(uint64_t now, bool *on) {
    if (pulse_pos >= pulse_count)
        return false;

    if (pulse_started_us == 0)
        pulse_started_us = now;

    uint64_t elapsed_ms = (now - pulse_started_us) / 1000;
    while (pulse_pos < pulse_count && elapsed_ms >= pulse_ms[pulse_pos]) {
        elapsed_ms -= pulse_ms[pulse_pos];
        pulse_pos++;
        pulse_started_us = now - elapsed_ms * 1000;
    }

    if (pulse_pos >= pulse_count) {
        pulse_reset();
        return false;
    }

    *on = ((pulse_pos & 1) == 0);
    return true;
}

/* I due segnali di avvio sono lampi *bloccanti*, non passano dalla coda.
   Vengono emessi in punti dove nulla e' time-critical (prima dell'avvio di
   core1 e dell'emulazione), e soprattutto fra i due c'e' di mezzo l'init della
   SD e l'attesa dell'host USB: una coda verrebbe azzerata dal secondo segnale
   prima ancora che il primo sia stato riprodotto. */

/* Sweep rosso -> verde -> blu: dice che il firmware e' partito e insieme fa da
   autotest dei tre canali a ogni accensione. */
void led_signal_boot(void) {
    static const led_color_t sweep[] = { LED_RED, LED_GREEN, LED_BLUE };

    for (size_t i = 0; i < sizeof(sweep) / sizeof(sweep[0]); i++) {
        led_set_color(sweep[i]);
        sleep_ms(BOOT_SWEEP_US / 1000);
    }
    led_clear();
}

/* microSD montata, si entra in emulazione. */
void led_signal_mode(void) {
    led_set_color(LED_WHITE);
    sleep_ms(PULSE_SHORT_US / 1000);
    led_clear();
}

/* Codifica della card corrente: le numerate sono N lampi brevi, le altre hanno
   un prefisso di lampi lunghi che ne indica il tipo. */
static void led_signal_card(void) {
    pulse_reset();
    pulse_color = LED_YELLOW;

    switch (ps1_cardman_get_state()) {
        case PS1_CM_STATE_BOOT:
            /* Due lampi verdi lunghi, su fondo spento imposto a mano: il verde
               e' gia' il colore di fondo della boot card, quindi lampeggiare
               verde su verde non si vedrebbe. Lo sfondo proprio serve anche a
               farli emergere dal magenta quando si arriva qui dalla combo, che
               e' il caso normale. Finita la sequenza resta il verde fisso. */
            pulse_color = LED_GREEN;
            pulse_off_color = LED_OFF;
            pulse_off_is_base = false;
            pulse_add_n(2, PULSE_LONG_US);
            break;
        case PS1_CM_STATE_GAMEID:
            pulse_add_n(2, PULSE_LONG_US);
            break;
        case PS1_CM_STATE_NAMED:
            pulse_add_n(3, PULSE_LONG_US);
            pulse_add_n((uint8_t)ps1_cardman_get_idx(), PULSE_SHORT_US);
            break;
        case PS1_CM_STATE_NORMAL:
        default: {
            int idx = ps1_cardman_get_idx();
            if (idx < 1)
                idx = 1;
            if (idx > 10)
                idx = 10;   /* oltre non sarebbe contabile a occhio */
            pulse_add_n((uint8_t)idx, PULSE_SHORT_US);
            break;
        }
    }

    /* pausa finale, per non incollare la sequenza all'attivita' successiva */
    if (pulse_count)
        pulse_ms[pulse_count - 1] = PULSE_TAIL_US / 1000;
}

/* Cambio di canale: stesso giallo della card, N lampi brevi = numero canale.
   Card e canale si distinguono perche' la card ha il prefisso lungo per gli
   stati speciali e in genere un conteggio diverso. */
static void led_signal_channel(void) {
    int chan = ps1_cardman_get_channel();
    if (chan < 1)
        chan = 1;
    if (chan > 10)
        chan = 10;

    pulse_reset();
    pulse_color = LED_YELLOW;
    pulse_add_n((uint8_t)chan, PULSE_SHORT_US);

    if (pulse_count)
        pulse_ms[pulse_count - 1] = PULSE_TAIL_US / 1000;
}

/* --- attivita' memory card ---------------------------------------------- */

static uint64_t last_write_us;
static uint64_t last_read_us;

void led_note_write(void) {
    last_write_us = time_us_64();
}

void led_note_read(void) {
    last_read_us = time_us_64();
}

/* Colore di fondo su cui tutto il resto lampeggia: bianco fisso mentre il
   passthrough USB MSC e' attivo, altrimenti spento. */
static bool msc_active;

void led_mode_msc(bool on) {
    msc_active = on;
}

/* Il verde fisso che segnala la boot card. Quanto resta acceso lo decide
   LedBootCardTimeout in settings.ini: FOLLOW (default) per tutta la durata
   della boot mode, 0 per non segnalarla affatto, N per N secondi. Anche con 0
   i due lampi verdi del passaggio si vedono lo stesso: quelli arrivano dalla
   coda di impulsi, che non passa di qui. */
static bool boot_indicator_on(uint64_t now) {
    static uint64_t boot_since_us;   /* 0 = non siamo sulla boot card */

    if (ps1_cardman_get_state() != PS1_CM_STATE_BOOT) {
        boot_since_us = 0;
        return false;
    }

    if (boot_since_us == 0)
        boot_since_us = now;

    const uint8_t timeout_s = settings_get_ps1_led_bootcard_timeout();

    if (timeout_s == SETTINGS_LED_BOOTCARD_FOLLOW)
        return true;

    return (now - boot_since_us) < (uint64_t)timeout_s * 1000000;
}

/* --- errori -------------------------------------------------------------- */

#define ERR_BLINK_SLOW_US   (1000 * 1000)
#define ERR_BLINK_FAST_US   ( 150 * 1000)

static bool red_state_for(int err, uint64_t now) {
    switch (err) {
        case 0:
            return false;
        case ERR_SD_ABSENT:
            return ((now / ERR_BLINK_SLOW_US) & 1) == 0;
        case ERR_SD_FORMAT:
            return ((now / ERR_BLINK_FAST_US) & 1) == 0;
        default: {
            /* N lampi brevi + pausa di 1 s: il codice resta contabile. */
            const uint64_t slot = ERR_BLINK_FAST_US * 2;
            const uint64_t cycle = slot * err + 1000000;
            const uint64_t pos = now % cycle;
            if (pos >= slot * (uint64_t)err)
                return false;
            return (pos % slot) < ERR_BLINK_FAST_US;
        }
    }
}

void led_error_tick(int err) {
    led_set_color(red_state_for(err, time_us_64()) ? LED_RED : LED_OFF);
}

void led_error_forever(int err) {
    /* Un errore runtime latched si vede come rosso fisso; qui siamo in un
       errore fatale, quindi il pattern lo scegliamo dal codice. */
    while (true) {
        led_error_tick(err);
        busy_wait_us_32(2000);
    }
}

void led_task(void) {
    const uint64_t now = time_us_64();

    if (ps1_mc_data_interface_write_occured() || ps2_mc_data_interface_write_occured())
        led_note_write();
    else if (ps1_mc_data_interface_read_occured())
        led_note_read();

    static int last_idx = -1, last_chan = -1;
    const int idx = ps1_cardman_get_idx();
    const int chan = ps1_cardman_get_channel();
    if (last_idx == -1) {
        last_idx = idx;
        last_chan = chan;
    } else if (idx != last_idx) {
        last_idx = idx;
        last_chan = chan;
        led_signal_card();
    } else if (chan != last_chan) {
        last_chan = chan;
        led_signal_channel();
    }

    /* Scala di priorita', dalla piu' alta. Il colore di fondo e' quello su cui
       ricadono sia le fasi spente delle sequenze sia quelle dei lampeggi:
       cosi' l'attivita' lampeggia sul bianco in modalita' MSC e sul verde
       mentre e' montata la boot card, invece che sul nero.

       Le due modalita' persistenti hanno un colore di fondo proprio perche'
       vanno riconosciute a colpo d'occhio in qualunque momento, non solo
       quando ci si entra: un lampo all'ingresso non basterebbe, e all'avvio
       non ci sarebbe nemmeno (led_task() segnala i cambi di card, e
       all'accensione non c'e' nessun cambio). */
    led_color_t base = LED_OFF;
    if (msc_active)
        base = LED_WHITE;
    else if (boot_indicator_on(now))
        base = LED_GREEN;

    led_color_t out = base;

    bool pulse_on = false;
    const int latched = error_latch_get();

    if (pulse_active(now, &pulse_on)) {
        out = pulse_on ? pulse_color
                       : (pulse_off_is_base ? base : pulse_off_color);
    } else if (ps1_memory_card_combo_active()) {
        /* Fisso per tutta la durata della modalita': si vede che il firmware
           sta ascoltando il dpad. Il cambio card ci lampeggia sopra in giallo
           e poi si torna qui. */
        out = LED_MAGENTA;
    } else if (latched != 0 && red_state_for(latched, now)) {
        out = LED_RED;
    } else if (last_write_us && (now - last_write_us) < ACTIVITY_HOLD_US) {
        out = (((now / BLINK_WRITE_US) & 1) == 0) ? LED_BLUE : base;
    } else if (last_read_us && (now - last_read_us) < ACTIVITY_HOLD_US) {
        out = (((now / BLINK_READ_US) & 1) == 0) ? LED_CYAN : base;
    }

    led_set_color(out);
}

#else   /* !LED_STYLE_ACTIVITY: comportamento originale di sd2psXtd */

void led_error_forever(int err) {
    while (true) {
        for (int i = 0; i < err; i++) {
            led_fatal();
            sleep_ms(250);
            led_clear();
            sleep_ms(250);
        }
        sleep_ms(1000);
    }
}

void led_error_tick(int err) {
    /* Senza lo stile ad attivita' non c'e' un pattern non bloccante da
       riprodurre: si tiene il rosso acceso e basta. */
    (void)err;
    led_fatal();
}

void led_signal_boot(void) {}
void led_signal_mode(void) {}
void led_mode_msc(bool on) { (void)on; }
void led_note_read(void) {}
void led_note_write(void) {}

void led_task(void) {
    uint64_t time = time_us_64();
    static bool red_active = false, green_active = false, blue_active = false;
    static int last_idx = 0, last_ch = 0;

    int idx = (settings_get_mode(true) == MODE_PS2) ? ps2_cardman_get_idx() : ps1_cardman_get_idx();
    int ch = (settings_get_mode(true) == MODE_PS2) ? ps2_cardman_get_channel() : ps1_cardman_get_channel();

    if ((idx != last_idx) || (ch != last_ch)) {
        red_active |= true;
        green_active |= true;
        blue_active |= true;
        last_idx = idx;
        last_ch = ch;
    } else {
        green_active |= !ps2_mmceman_fs_idle();
        blue_active |= (settings_get_mode(true) == MODE_PS2) ? ps2_mc_data_interface_write_occured() : ps1_mc_data_interface_write_occured();
    }

    if (time - last_refresh > LED_REFRESH_US) {
        led_set_rgb(red_active, green_active, blue_active);

        red_active = false;
        green_active = false;
        blue_active = false;
        last_refresh = time;
    }
}

#endif  /* LED_STYLE_ACTIVITY */
