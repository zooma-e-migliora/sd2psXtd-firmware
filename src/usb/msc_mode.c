/* Modalita' USB MSC: la microSD viene esposta al PC come un lettore di schede.
 *
 * Si entra qui all'avvio, prima di far partire l'emulazione carta: si porta su
 * il device USB e si aspetta fino a MSC_MOUNT_TIMEOUT_MS che un host lo
 * enumeri. Se l'enumerazione avviene siamo collegati a un PC e restiamo in
 * passthrough; altrimenti siamo nello slot della console e si prosegue con
 * l'emulazione normale. Quando l'host espelle il volume o il cavo viene
 * staccato si esce dal passthrough e si passa all'emulazione, senza riavviare.
 *
 * E' la stessa logica del firmware PicoMemcard attualmente sulla board
 * (TUD_MOUNT_TIMEOUT), quindi il comportamento percepito non cambia.
 */

#include "usb/msc_mode.h"

#include "hardware/watchdog.h"
#include "pico/time.h"

#include "debug.h"
#include "sd.h"
#include "serial_input.h"
#include "usb/msc_disk.h"
#include "usb/usb_device.h"

#if WITH_LED
#include "led.h"
#endif

/* Tempo massimo di attesa dell'enumerazione da parte di un host USB. */
#define MSC_MOUNT_TIMEOUT_MS 3000

/* Per quanto tempo il device deve restare smontato prima di considerare
   staccato il cavo e uscire dalla modalita' MSC. */
#define MSC_UNMOUNT_GRACE_MS 500

/* Attesa dopo l'espulsione: finche' il cavo resta collegato l'emulazione non
   parte da sola, serve "start emulation" sulla seriale. Vedi
   msc_mode_wait_for_start(). */
static volatile bool emulation_requested;
static bool emulation_held;

/* Vedi msc_state_t in msc_mode.h. Scritto ai tre passaggi di stato e letto da
   core0 nel gestore dei comandi seriali. */
static volatile msc_state_t msc_state = MSC_STATE_EMULATION;

msc_state_t msc_mode_state(void) {
    return msc_state;
}

const char *msc_mode_state_str(void) {
    switch (msc_state) {
        case MSC_STATE_PASSTHROUGH:
            return "passthrough MSC attivo (volume montato sul PC)";
        case MSC_STATE_HELD:
            return "volume espulso, in attesa di 'start emulation'";
        default:
            return "emulazione attiva";
    }
}

/* Uscita dal passthrough: l'host ha espulso il volume, oppure il cavo e' stato
   staccato. Da qui si prosegue con l'emulazione. */
static void msc_leave(void) {
    sd_sync_device();
    msc_disk_set_enabled(false);

    /* Niente usb_device_deinit(): staccare e riattaccare il device al bus lo
       farebbe riemunerare, e col cavo ancora collegato il disco ricomparirebbe
       sul PC subito dopo che l'utente lo ha espulso. Lasciando su lo stack,
       tud_msc_test_unit_ready_cb() risponde "medium not present" perche'
       msc_enabled e' ormai false: l'host vede sparire il supporto, come da un
       lettore di schede a cui si toglie la scheda. La seriale CDC resta viva. */

#if WITH_LED
    led_mode_msc(false);
    led_clear();
#endif

    /* watchdog_reboot(0, 0, 0);
     *
     * Disabilitato di proposito. Riavviare era il modo piu' semplice per
     * rimontare il filesystem, ma con il cavo attaccato il firmware ripartiva,
     * rivedeva l'host e rientrava in MSC: la scheda spariva e ricompariva
     * subito dopo l'espulsione. Espellere non e' un errore e non deve
     * riavviare niente. In piu' un reset del watchdog non rialimenta la
     * microSD: se sd.begin() fallisse per questo si finirebbe in
     * fatal(ERR_SD_ABSENT), dove il ciclo di attesa riavvia appena la scheda
     * "compare": cioe' un loop di riavvii.
     *
     * Al posto del riavvio si rimonta, che e' la parte che serviva davvero. */
    DPRINTF("MSC: uscita, rimonto la microSD\n");
    if (!sd_remount()) {
        /* Non fatale: si segnala e si prova comunque a proseguire. */
        DPRINTF("MSC: rimontaggio fallito\n");
        error_latch_set(ERR_SDCARD);
    }
}

bool msc_mode_try_enter(void) {
    usb_device_init();

    const absolute_time_t deadline = make_timeout_time_ms(MSC_MOUNT_TIMEOUT_MS);
    while (!time_reached(deadline)) {
        usb_device_task();
        if (usb_device_mounted())
            break;
    }

    if (!usb_device_mounted()) {
        /* Nessun host: siamo nella console. Lo stack USB resta attivo per
           l'interfaccia comandi CDC, ma l'unita' MSC resta "non pronta". */
        DPRINTF("MSC: nessun host USB, avvio emulazione\n");
        msc_disk_set_enabled(false);
        msc_state = MSC_STATE_EMULATION;
        return false;
    }

    DPRINTF("MSC: host USB rilevato, passthrough microSD attivo\n");
    msc_disk_set_enabled(true);
    msc_state = MSC_STATE_PASSTHROUGH;

#if WITH_LED
    /* Bianco fisso per tutta la durata del passthrough: dice a colpo d'occhio
       che la scheda e' entrata in modalita' MSC e non in emulazione.
       L'attivita' ci lampeggia sopra. */
    led_mode_msc(true);
#endif

    absolute_time_t unmounted_since = nil_time;

    while (true) {
        usb_device_task();

        /* La seriale deve rispondere anche qui. Senza, col volume montato la
           scheda sembra piantata: la porta c'e' e nessuno risponde, e non
           esiste modo di chiederle in che stato si trova. I comandi che
           toccherebbero la microSD - che adesso e' dell'host - li rifiuta
           execute_command() guardando msc_mode_state(). */
        serial_input_process();

        if (msc_disk_host_ejected()) {
            /* L'host ha espulso il volume. */
            msc_leave();
            return false;
        }

        if (usb_device_mounted()) {
            unmounted_since = nil_time;
        } else if (is_nil_time(unmounted_since)) {
            unmounted_since = make_timeout_time_ms(MSC_UNMOUNT_GRACE_MS);
        } else if (time_reached(unmounted_since)) {
            /* Cavo staccato. */
            msc_leave();
            return false;
        }

#if WITH_LED
        /* Stessa semantica dell'emulazione: blu in scrittura e ciano in
           lettura, qui provocate dall'host invece che dalla console. */
        if (msc_disk_write_occurred())
            led_note_write();
        else if (msc_disk_read_occurred())
            led_note_read();
        led_task();
#endif
    }
}

void msc_mode_request_emulation(void) {
    emulation_requested = true;
}

bool msc_mode_emulation_held(void) {
    return emulation_held;
}

/* Il messaggio va sia all'espulsione sia a ogni apertura della CDC: chi apre il
   terminale dopo aver espulso troverebbe altrimenti solo un prompt muto.
   printf() e non DPRINTF(): in questa variante il buffer di debug e' disattivo,
   mentre printf() passa dal driver stdio su CDC di usb_device.c. */
static void print_hold_banner(void) {
    serial_input_begin_output();
    printf("Volume ejected. Card emulation is on hold while USB stays connected.\n"
           "Type 'start emulation' to start the memory card emulation.\n");
    serial_input_notify_output();
}

void msc_mode_wait_for_start(void) {
    if (!usb_device_mounted()) {
        /* Nessun host: o non c'e' mai stato, o il cavo e' stato staccato. In
           entrambi i casi siamo alimentati dalla console e l'emulazione deve
           partire da sola. */
        msc_state = MSC_STATE_EMULATION;
        return;
    }

    emulation_requested = false;
    emulation_held = true;
    msc_state = MSC_STATE_HELD;

    /* Niente colore di fondo per l'attesa: il LED resta spento. msc_leave() ha
       gia' fatto led_clear(), e senza modalita' MSC ne' boot card led_task()
       ricade su LED_OFF. Continua a girare lo stesso perche' un errore
       agganciato, per esempio un rimontaggio della microSD fallito, deve
       restare visibile in rosso anche qui. */
    bool cdc_was_connected = usb_device_cdc_connected();
    print_hold_banner();

    while (!emulation_requested && usb_device_mounted()) {
        usb_device_task();

        const bool cdc_connected = usb_device_cdc_connected();
        if (cdc_connected && !cdc_was_connected)
            print_hold_banner();
        cdc_was_connected = cdc_connected;

        serial_input_process();
#if WITH_LED
        led_task();
#endif
    }

    emulation_held = false;
    msc_state = MSC_STATE_EMULATION;
}
