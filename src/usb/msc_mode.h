#pragma once

#include <stdbool.h>

/* Avvia lo stack USB e attende brevemente l'enumerazione da parte di un host.
 *
 * Se un host risponde entra in passthrough microSD e resta nel loop MSC finche'
 * il volume non viene espulso o il cavo staccato; a quel punto rimonta la
 * microSD e ritorna false, cosi' il chiamante prosegue con l'emulazione. Il
 * dispositivo non viene riavviato e lo stack USB resta su: dopo l'espulsione
 * l'host vede semplicemente un lettore senza supporto, e la seriale CDC
 * continua a funzionare.
 *
 * Se nessun host risponde ritorna false subito: lo stack USB resta comunque
 * attivo per l'interfaccia comandi CDC, ma l'unita' MSC e' dichiarata non
 * pronta e il chiamante puo' procedere con l'emulazione carta. */
bool msc_mode_try_enter(void);

/* Attesa dopo l'uscita dal passthrough.
 *
 * Se un host USB e' ancora enumerato, cioe' il volume e' stato espulso ma il
 * cavo e' ancora collegato, l'emulazione non parte da sola: quasi sempre
 * un'espulsione e' solo un'espulsione, e far partire l'emulazione mentre si e'
 * attaccati al PC non serve. Si resta qui, con la seriale viva, finche' non
 * arriva "start emulation" (msc_mode_request_emulation()) o finche' il cavo non
 * viene staccato: quel caso significa che siamo alimentati dalla console.
 *
 * Senza host ritorna subito, quindi nella console non cambia niente. */
void msc_mode_wait_for_start(void);

/* Chiamata dal comando seriale "start emulation": sblocca l'attesa. */
void msc_mode_request_emulation(void);

/* True mentre msc_mode_wait_for_start() sta aspettando, cosi' il comando
   seriale sa distinguere fra avviare l'emulazione e dire che gira gia'. */
bool msc_mode_emulation_held(void);

/* Stato osservabile dall'esterno.
 *
 * Serve perche' senza di questo lo stato NON si puo' dedurre dalla seriale, e
 * il comando "start emulation" finiva per dire "Emulation already running"
 * anche in passthrough, che e' il caso opposto: emulation_held e' true solo
 * dentro wait_for_start(), quindi false copriva sia "emulazione in corso" sia
 * "volume montato sul PC". */
typedef enum {
    MSC_STATE_EMULATION = 0,  /* loop di main: emulazione attiva */
    MSC_STATE_PASSTHROUGH,    /* volume montato sul PC */
    MSC_STATE_HELD,           /* espulso, cavo attaccato, attesa comando */
} msc_state_t;

msc_state_t msc_mode_state(void);

/* Testo per l'utente, usato da "status" e dai rifiuti dei comandi. */
const char *msc_mode_state_str(void);
