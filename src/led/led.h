#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Un colore, 8 bit per canale. I backend acceso/spento arrotondano a >= 128;
   quelli con sfumature (PWM, WS2812) usano il valore pieno. Definire la
   tavolozza a 8 bit anche quando il driver e' binario permette di passare al
   PWM cambiando una riga nella variante, senza toccare la logica. */
typedef struct {
    uint8_t r, g, b;
} led_color_t;

#define LED_OFF      ((led_color_t){   0,   0,   0 })
#define LED_RED      ((led_color_t){ 255,   0,   0 })
#define LED_GREEN    ((led_color_t){   0, 255,   0 })
#define LED_BLUE     ((led_color_t){   0,   0, 255 })
#define LED_YELLOW   ((led_color_t){ 255, 255,   0 })
#define LED_MAGENTA  ((led_color_t){ 255,   0, 255 })
#define LED_CYAN     ((led_color_t){   0, 255, 255 })
#define LED_WHITE    ((led_color_t){ 255, 255, 255 })

void led_init(void);
void led_fatal(void);
void led_clear(void);
void led_task(void);

/* Controllo diretto. Rispetta presenza e polarita' configurate dalla variante:
   un canale assente viene semplicemente ignorato. */
void led_set_color(led_color_t c);
void led_set_rgb(bool red, bool green, bool blue);

/* Segnalazione di un errore fatale. Non ritorna: sceglie il pattern in base al
   codice e lo ripete all'infinito. */
void led_error_forever(int err);

/* Un singolo passo del pattern d'errore, non bloccante: serve a chi deve
   segnalare un errore continuando a fare qualcos'altro, come il ciclo che
   aspetta l'inserimento della microSD. */
void led_error_tick(int err);

/* Segnalazioni a sequenza (attive solo con LED_STYLE_ACTIVITY, altrove no-op).
   Le due di avvio sono bloccanti e brevi; le altre non bloccano: la sequenza
   viene riprodotta dalle chiamate a led_task(). */
void led_signal_boot(void);
void led_signal_mode(void);

/* Colore di fondo della modalita' USB MSC: acceso fisso finche' il passthrough
   e' attivo, con l'attivita' che ci lampeggia sopra. */
void led_mode_msc(bool on);

/* Notifica di attivita' per chi non passa dal data interface della card,
   come il passthrough USB MSC. */
void led_note_read(void);
void led_note_write(void);
