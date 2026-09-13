#include "hardware/gpio.h"
#include "hardware/structs/iobank0.h"
#include "hardware/pio.h"
#include "hardware/timer.h"
#include "pico.h"
#include "pico/multicore.h"
#include "ps1_mc_data_interface.h"
#include "ps1_mmce.h"
#include "string.h"
#include <settings.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "ps1_mc_spi.pio.h"
#include "debug.h"
#include "ps1/ps1_cardman.h"
#include "ps1/ps1_memory_card.h"
#include "game_db/game_db.h"
#if WITH_MSC
#include "usb/msc_mode.h"
#endif

#if LOG_LEVEL_PS1_MC == 0
    #define log(x...)
#else
    #define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_PS1_MC, level, fmt, ##x)
#endif


static uint64_t us_startup;
static volatile int reset;
static uint8_t flag;
static uint8_t* curr_page = NULL;
static bool ps2_multitap = false;
static volatile bool card_active = false;

typedef struct {
    uint32_t offset;
    uint32_t sm;
} pio_t;

static pio_t cmd_reader, dat_writer, cntrl_reader, ack_pulser;
static volatile int mc_exit_request, mc_exit_response, mc_enter_request, mc_enter_response;

/* Profili di timing PS1.
 *
 * Il profilo "boot" vale solo mentre e' montata la boot card, ed esiste per il
 * payload FreePSXBoot "superfast", che raddoppia il clock del bus a ~500 kHz.
 * Cambia due cose rispetto allo standard, e sono legate fra loro:
 *
 *   - un divisore di clock PIO piu' basso su dat_writer e cmd_reader, perche' a
 *     quella velocita' il bit va presentato prima sul DAT (PS1_CLKDIV_BOOT in
 *     ps1_mc_spi.pio). Il lettore del pad NON lo segue: parla sempre a 250 kHz;
 *   - la larghezza dell'impulso di ACK, che si conta in cicli PIO e quindi si
 *     accorcia insieme al divisore. E' la causa dell'issue 59, e per questo la
 *     larghezza segue la velocita' del bus invece di essere fissa: vedi il
 *     commento sopra ack_cycles.
 *
 * Lo governa FastMode_Enable, acceso di default, ed e' l'unico interruttore
 * esposto: il profilo di boot non ha altre opzioni, ne' nell'ini ne' a menu.
 *
 * Attenzione a non leggerlo come "adesso non c'e' piu' nessun ritardo": e'
 * sparita l'OPZIONE, non il ritardo. Un ritardo resta, ma e' interno, fisso, e
 * applicato in un punto solo - le letture singole del BIOS (0x52). La fase dati
 * della lettura lunga (0x42) e i comandi MMCE ne sono esenti, e le due esenzioni
 * sono misurate, non prudenziali: senza la prima l'avvio costa ~2,9 s in piu',
 * senza la seconda il Game ID arriva 1 byte su 7 (prova M1 del 04/09).
 *
 * Fuori dalla boot card non serve nulla di tutto cio' e si usa il profilo di
 * upstream. Il racconto completo e' in docs/ISSUE-59.md. */
static volatile bool pio_ready = false;
static bool timing_boot = false;
static bool timing_applied = false;

/* --- L'ACK del profilo di boot: PRIMA la posizione, poi la durata ---------
 *
 * Il PIO emette l'ACK nell'ISTANTE IN CUI IL "pull" RIESCE. Da qui discende
 * tutto, e ci sono due casi che vanno tenuti separati:
 *
 *   (a) TX FIFO vuota. Il PIO si ferma sul "pull block" e riparte solo quando
 *       core1 gli scrive un byte; core1 lo scrive solo dopo aver letto il byte
 *       della console, che arriva dopo l'ottava salita. L'ACK non puo' che
 *       essere dopo la fine del byte: il PIO sta aspettando noi.
 *
 *   (b) TX FIFO gia' piena. Il PIO finisce l'ottavo bit sull'ottava DISCESA, fa
 *       .wrap, e il pull riesce subito: l'ACK esce ~0,6 us PRIMA dell'ottava
 *       salita, cioe' prima che la console abbia finito il byte. E' la norma
 *       nella fase dati della lettura lunga 'B', da quando c'e' long_read_put().
 *
 * psx-spx mette /ACK interamente nella pausa fra un byte e il successivo e
 * ancora entrambi i suoi vincoli all'ULTIMO SCK: il caso (b) e' fuori da tutto
 * cio' che la specifica descrive. Upstream lo impediva su due livelli, il
 * lock-step del firmware e la coda "nop [7]" del dat_writer standard; il
 * profilo di boot le aveva rimosse entrambe.
 *
 * La correzione e' la "wait 1 gpio PIN_PSX_CLK" in coda a dat_writer, che
 * aspetta l'ottava salita prima del pull successivo. Vale a qualunque divisore
 * e velocita' del bus perche' aspetta il fronte, non un numero di cicli.
 *
 * IL RITARDO QUI SOTTO NON CORREGGE IL CASO (b). Nel caso (b) il PIO non sta
 * aspettando core1, quindi l'ACK esce comunque all'istante: il ritardo frena
 * solo core1 quanto basta a tenere la FIFO vuota, cioe' a ricadere nel caso (a).
 * Lo dicono i numeri: il pavimento misurato e' 16 us e un byte a 498 kHz dura
 * 16,06 us - il ritardo non era tarato sulla finestra cieca della console, era
 * tarato per essere piu' lento del byte. Resta perche' serve al caso (a), dove
 * l'ACK arriverebbe ~1 us dopo l'ultimo SCK ed e' il driver del kernel a
 * ignorarlo (il loop del browser di UniROM); il valore giusto per quello va
 * rimisurato, e ci si aspetta pochi us.
 *
 * L'attesa ha una via d'uscita su reset/card_active: senza, core1 bruciava i
 * microsecondi anche a card deselezionata e il pad restava senza nessuno che lo
 * ascoltasse. E' una TERZA cosa, e non c'entra con l'ACK.
 *
 * La LARGHEZZA non si scrive piu' a mano: si calcola dal divisore, perche'
 * l'impulso si conta in cicli PIO. Vedi ps1_ack_cycles_for_div() in
 * ps1_mc_spi.pio.
 *
 * Il racconto completo, misure comprese, e' in docs/ISSUE-59.md. */
#if PS1_MC_TUNING
/* --- DEFAULT = la configurazione della release 77336ba -------------------
 *
 * Non e' un dettaglio: il firmware non deve mai accendersi in una
 * configurazione che nessuno ha validato. La build precedente aveva come
 * default la larghezza calcolata dalla specifica (15 cicli a divisore 32) e
 * l'attesa accesa, e in quella configurazione l'exploit non parte - misurato,
 * corsa 1 del 03/09. Chi vuole quei valori li chiede esplicitamente. */

/* Larghezza in CICLI PIO. 0 = calcolata dal divisore per stare nei 2 us di
   specifica; >0 la forza. Default 8, che e' il valore della release. */
static volatile uint16_t ack_cycles   = PS1_ACK_CYCLES_RELEASE;
static volatile uint32_t ack_delay_us = PS1_ACK_DELAY_BOOT_US;
/* Attesa dell'ottava salita prima del pull successivo. In cinque corse non ha
   mostrato nessun effetto (due coppie A/B), quindi default SPENTA finche' non
   c'e' una misura che la giustifichi. */
static volatile uint8_t  ack_wait_on = 0;
/* Tetto al riempimento della TX FIFO nella fase dati della lettura lunga.
   0 = lock-step come upstream, cioe' una risposta e poi si aspetta il byte
   della console: e' la configurazione in cui la FIFO non puo' portarsi avanti.
   1..8 = quanti byte al massimo si lasciano accumulare. */
static volatile uint8_t  tx_depth = 0;
/* Ritardo di testa prima di chiedere l'impulso, in cicli PIO.
   255 = segui il profilo (PS1_ACK_HEAD_STD o _BOOT), che e' il normale. */
static volatile uint8_t  ack_head = 255;
static volatile uint8_t  trace_stop = 0;
/* Il ritardo su qualunque profilo, non solo su quello di boot. Spento di
   default perche' acceso e' la trappola che e' gia' costata una diagnosi
   sbagliata: una prova con "set profile 1" si portava dietro il ritardo del
   profilo di boot senza dirlo. */
static volatile uint8_t  delay_all = 0;
static volatile uint16_t div_dat = 0, div_cmd = 0, div_ctrl = 0;  /* 0 = profilo */
/* Forza il profilo INTERO - programma PIO, divisore, pause interne e ritardo -
   invece dei singoli parametri. Serve perche' fra i due profili le differenze
   sono sei, non tre: i knob da soli non possono riprodurre la forma del
   programma standard (la pausa "pull block [3]" prima dell'ACK, quella dopo il
   rilascio, e la coda "jmp !osre ack"). 0 = automatico, 1 = standard, 2 = boot. */
static volatile uint8_t profile_force = 0;
/* Override di BootCardTimeout senza passare dall'ini. 255 = usa l'ini. */
static volatile uint8_t boottimeout_force = 255;
/* 1 = i comandi MMCE saltano il ritardo (corretto, default). 0 = lo pagano come
   le letture, cioe' com'era prima: serve per il confronto diretto. */
static volatile uint8_t mmce_nodelay = 1;
/* 1 = la fase dati della lettura lunga salta il ritardo. 0 = lo paga come le
   letture del BIOS, cioe' com'era prima: serve per il confronto diretto. */
static volatile uint8_t longnodelay = 1;

uint8_t ps1_mc_tune_boottimeout(void)      { return boottimeout_force; }
#else
#define ack_delay_us  PS1_ACK_DELAY_BOOT_US
#define div_dat       0u
#define div_cmd       0u
#define div_ctrl      0u
#define ack_cycles    PS1_ACK_CYCLES_RELEASE
#define ack_wait_on   false
/* Nella release long_read_put() non esiste (e' in #if PS1_MC_TUNING) e la
   fase dati e' sempre in lock-step, quindi questo simbolo non pilota
   niente: resta solo perche' il codice della taratura lo nomina. Non
   leggerlo come "la pipeline e' profonda 8": la pipeline non c'e'. */
#define tx_depth      0u
#define ack_head      255u
#define longnodelay   1u
#endif

/* Larghezza dell'ACK effettivamente programmata nel PIO adesso. Non e' un knob:
   e' il risultato del calcolo, e va guardata invece del valore chiesto. */
static volatile uint16_t ack_width_cycles;
static volatile uint32_t ack_width_ns;
static volatile uint8_t  head_applied;
/* Durata in ns del ritardo di testa programmato: i cicli da soli non bastano,
   perche' cambiano durata col divisore - e' la trappola gia' costata misure. */
static volatile uint32_t head_ns_applied;

/* Numero di pagine della card emulata: oltre questo get_page() leggerebbe
   fuori dal buffer. */
#define PS1_CARD_PAGE_COUNT 1024

/* --- Il numero di settore va mascherato, non creduto -----------------------
 *
 * psx-spx, «Reading Data from Memory Card»: il settore sta in 0..3FFh, e per un
 * numero fuori range le card originali Sony rispondono FFFFh come indirizzo
 * confermato e abortiscono, mentre quelle di terze parti «respond with the
 * sector number ANDed with 3FFh (and transfer the data for that adjusted sector
 * number)». Facciamo la seconda: e' il comportamento della classe di card che
 * emuliamo, ed e' gia' quello che il ciclo della lettura lunga assume.
 *
 * Senza maschera l'indice arriva dalla console senza nessun controllo. Con
 * WITH_PSRAM spento - questa board - get_page() ritorna &card[page*128] su un
 * array di PS1_CARD_PAGE_COUNT pagine e write_byte() fa card[address] = byte:
 * con page fino a FFFFh sono megabyte oltre il buffer, in lettura e IN
 * SCRITTURA. La lettura lunga era gia' protetta dal suo "page <
 * PS1_CARD_PAGE_COUNT", la singola e la scrittura no.
 *
 * Non e' un caso di laboratorio: una console sana non manda mai un settore
 * fuori range, ma un fronte di clock perso manda cmd_reader fuori passo - e' il
 * guasto inseguito in tutta l'issue 59 - e da li' i due byte dell'indirizzo
 * valgono qualunque cosa.
 *
 * Va applicata SUBITO DOPO la ricezione, prima di qualunque uso: cosi' l'eco
 * dell'indirizzo confermato riporta il settore rettificato, com'e' descritto, e
 * il checksum e' coerente con i dati che mandiamo davvero. L'eco intermedio
 * cambia anche lui, ma psx-spx lo dichiara don't care («Responses in brackets
 * are don't care»). Per un indirizzo valido e' un no-op. */
#define PS1_PAGE_MSB_MASK ((uint8_t)(((PS1_CARD_PAGE_COUNT) - 1u) >> 8))
#define PS1_CLAMP_PAGE_MSB(msb)  do { (msb) &= PS1_PAGE_MSB_MASK; } while (0)

#if PS1_MC_TUNING
/* --- Registro delle scritture della console -------------------------------
 *
 * La console scrive sulla boot card, e la stessa osservazione ha due letture
 * opposte che vanno separate:
 *
 *   (a) l'exploit e' fallito e il BIOS "formatta" la card perche' la crede
 *       corrotta -> la scrittura e' un SINTOMO, non una causa;
 *   (b) il software scrive qualcosa che poi RILEGGE (una copia di se', un
 *       salvataggio) -> negandogliela con la sola lettura gli cambiamo i dati
 *       sotto i piedi, e le prove non valgono.
 *
 * Sul filo la sola lettura non cambia niente: mc_cmd_write() clocka comunque i
 * 128 byte e risponde comunque 0x47. Su questa board (WITH_PSRAM spento) toglie
 * a core1 una sola store per byte, e il flush su microSD sta in ps1_dirty_task()
 * su core0: NON puo' sballare i tempi. Quindi (b) e' una dipendenza sui DATI, e
 * l'unico modo di verificarla e' vedere se una pagina scritta viene poi riletta.
 *
 * Da qui il contatore "readback": se resta a 0, (b) e' esclusa e la sola
 * lettura e' provatamente innocua. La sequenza delle pagine dice il resto -
 * 0 poi 1..15 in ordine e' la riscrittura di header e directory, cioe' (a);
 * 64-75 sono i frame di stage2, cioe' qualcuno che scrive sopra il payload. */
#define MCWRITES_SLOTS 24u

typedef struct {
    uint32_t t_us;
    uint16_t page;
    uint8_t  chk_ok;      /* 1 = abbiamo risposto 0x47, 0 = 0x4E */
    uint8_t  readback;    /* 1 = voce di rilettura, non di scrittura */
    uint8_t  d[4];        /* primi 4 byte dei dati */
} mcwrite_entry_t;

static mcwrite_entry_t mcwrites[MCWRITES_SLOTS];
static volatile uint32_t mcwrites_written;    /* totale, non indice */
static volatile uint32_t mcwrites_readback;   /* IL numero che decide */
/* Una pagina per bit: 1024 bit = 128 byte. */
static uint8_t written_pages[PS1_CARD_PAGE_COUNT / 8];

static void __time_critical_func(mcwrite_put)(uint16_t page, uint8_t chk_ok,
                                              uint8_t readback, const uint8_t *d) {
    const uint32_t i = mcwrites_written++;
    mcwrite_entry_t *e = &mcwrites[i % MCWRITES_SLOTS];
    e->t_us = timer_hw->timerawl;
    e->page = page;
    e->chk_ok = chk_ok;
    e->readback = readback;
    for (uint j = 0; j < 4; j++)
        e->d[j] = d ? d[j] : 0u;
}

/* Chiamata a ogni lettura: se la pagina era gia' stata scritta e' una
   rilettura, ed e' esattamente il caso che la sola lettura falserebbe. */
static void __time_critical_func(mcwrite_note_read)(uint16_t page) {
    if (page >= PS1_CARD_PAGE_COUNT)
        return;
    if (written_pages[page >> 3] & (1u << (page & 7u))) {
        mcwrites_readback++;
        mcwrite_put(page, 0, 1, NULL);
    }
}

void ps1_mc_writes_print(void) {
    const uint32_t total = mcwrites_written;
    const uint32_t shown = (total > MCWRITES_SLOTS) ? MCWRITES_SLOTS : total;

    printf("scritture: %u eventi%s, riletture di pagine scritte: %u%s\n",
           (unsigned)total, (total > MCWRITES_SLOTS) ? " (i piu' vecchi persi)" : "",
           (unsigned)mcwrites_readback,
           mcwrites_readback ? "  <-- la sola lettura CAMBIA i dati" : "  (sola lettura innocua)");

    for (uint32_t k = 0; k < shown; k++) {
        const mcwrite_entry_t *e = &mcwrites[(total - shown + k) % MCWRITES_SLOTS];
        if (e->readback) {
            printf("  %10u us  RILETTURA pagina %u\n", (unsigned)e->t_us, (unsigned)e->page);
        } else {
            printf("  %10u us  scrive pagina %4u  %s  %02X %02X %02X %02X  %s\n",
                   (unsigned)e->t_us, (unsigned)e->page,
                   e->chk_ok ? "0x47" : "0x4E",
                   e->d[0], e->d[1], e->d[2], e->d[3],
                   (e->page < 16u) ? "header/directory: la console sta formattando"
                   : (e->page >= 64u && e->page <= 75u) ? "SOPRA STAGE2"
                   : "");
        }
    }
    mcwrites_written = 0;
    mcwrites_readback = 0;
    memset(written_pages, 0, sizeof(written_pages));
}
#endif /* PS1_MC_TUNING */

#if PS1_MMCE_TRACE
/* --- Registro degli eventi MMCE ------------------------------------------
 *
 * Serve a rispondere a una domanda che dal nostro lato non si puo' dedurre:
 * quali comandi manda davvero la console dopo che l'exploit e' partito, e
 * quando. Da li' dipende se il ping e' un segnale utilizzabile per lasciare la
 * boot card.
 *
 * Scritto da core1 dentro __time_critical_func: nessuna formattazione, nessun
 * blocco, nessuna printf - solo un indice e cinque byte. Lo stampa core0 quando
 * glielo si chiede da seriale con "mmcelog". Le log() del progetto non servono
 * allo scopo: nella variante MSC sono spente a compile time. */
#define MMCELOG_SLOTS 48u

typedef struct {
    uint32_t t_us;      /* timer_hw->timerawl, basso a 32 bit: ~71 minuti */
    uint8_t  cmd;       /* 0x20-0x27, oppure gli pseudo-codici qui sotto */
    uint8_t  state;     /* ps1_cardman_get_state() al momento dell'evento */
    uint8_t  a, b;      /* dipendono dal comando: vedi ps1_mc_mmcelog_print() */
} mmcelog_entry_t;

/* Pseudo-codici: non stanno sul filo, dicono che strada ha preso il Game ID. */

/* Il Game ID puo' interrompersi a meta': i macro respondOrNextCmd /
   receiveOrNextCmd fanno "return" appena la console deseleziona la card. Senza
   questo, di una transazione abortita non resta niente - ed e' esattamente il
   caso che ci interessa. Qui si accumula quello che arriva, byte per byte, e
   "mmcelog" lo stampa comunque sia finita. */
static volatile uint8_t  gid_len_expected;   /* lunghezza annunciata */
static volatile uint8_t  gid_n;              /* byte effettivamente arrivati */
static volatile uint8_t  gid_buf[24];
/* Quando e' arrivato l'annuncio. Serve per un motivo concreto: mcclear NON
   azzera il Game ID, e il 04/09 questo ha salvato la prova M1 da un
   azzeramento fatto a sproposito. Ma la stessa proprieta' e' una trappola
   al contrario - un valore vecchio letto come fresco - e stampando l'eta'
   si vede subito quale dei due si sta guardando. */
static volatile uint32_t gid_ts;

static mmcelog_entry_t mmcelog[MMCELOG_SLOTS];
static volatile uint32_t mmcelog_written;   /* totale, non indice: dice se e' straripato */

void __time_critical_func(ps1_mc_mmcelog_put)(uint8_t cmd, uint8_t a, uint8_t b) {
    const uint32_t i = mmcelog_written++;
    mmcelog[i % MMCELOG_SLOTS] = (mmcelog_entry_t){
        .t_us  = timer_hw->timerawl,
        .cmd   = cmd,
        .state = (uint8_t)ps1_cardman_get_state(),
        .a = a, .b = b,
    };
}
#endif /* PS1_MMCE_TRACE */

/* Aggiornato a ogni comando indirizzato alla card. Vedi ps1_mc_last_activity_us(). */
static volatile uint32_t mc_last_activity_us;

/* --- Il ritardo dell'ACK non vale per i comandi MMCE ----------------------
 *
 * Sono due consumatori diversi sullo stesso filo, con esigenze OPPOSTE, e
 * misurate entrambe il 02/09 sulla stessa console:
 *
 *   letture di frame (BIOS, exploit, browser di UniROM)
 *       passano dal driver del kernel, che IGNORA un ACK arrivato entro 2-3 us
 *       dall'ultimo SCK (psx-spx). Senza ritardo il browser ricarica
 *       all'infinito: e' l'issue 59.
 *
 *   comandi MMCE 0x20-0x27 (ping, Game ID, cambio card e canale)
 *       UniROM pilota il SIO per conto suo, senza passare dal kernel: la
 *       finestra cieca non esiste, e il ritardo rompe la transazione. Con 24 us
 *       il Game ID si tronca dopo un byte (1 su 7), con 0 arriva intero.
 *       Misurato con divisore 96 e 32 e con entrambi i programmi PIO: dipende
 *       solo dal ritardo.
 *
 * Finche' il ritardo era globale una delle due perdeva per forza, ed e' il
 * motivo per cui ogni configurazione provata rompeva qualcosa: la larghezza
 * dell'ACK e il divisore, su cui era stata spesa la giornata, non c'entravano. */
static volatile bool mmce_cmd_in_progress = false;
/* Vero durante la FASE DATI della lettura lunga 'B'. Distingue le due fasi
   dell'avvio, che hanno requisiti diversi e finora condividevano lo stesso
   ritardo:
     0x52, letture singole  il BIOS carica stage2, ~1280 byte. E' QUI che
                            stanno tutti i fallimenti sotto il pavimento -
                            nove corse su nove, su entrambi i payload.
     0x42, lettura lunga    superfast carica stage3, ~121000 byte. Non ha mai
                            mostrato di aver bisogno del ritardo: non si
                            poteva provare, perche' abbassando l'offset
                            globale il BIOS cedeva prima di arrivarci.
   A 24 us per byte la fase lunga costa ~2,9 s, quella del BIOS ~31 ms. */
static volatile bool long_read_in_progress = false;

uint32_t ps1_mc_last_activity_us(void) {
    return mc_last_activity_us;
}

#if PS1_MC_TUNING
/* --- Dove si e' fermato, e quando ----------------------------------------
 *
 * Quando la console si pianta NOI non usciamo da nessuna parte: restiamo in
 * recv_cmd() ad aspettare un byte che non arriva. Quindi lo stato non si puo'
 * registrare all'uscita, va aggiornato mentre si procede. Da qui i campi
 * last_*: sono l'ultimo punto raggiunto, ed e' quello il punto di blocco.
 *
 * "freeze" senza questi campi e' un dato binario e non dice se l'exploit si sia
 * fermato nella scansione dei frame, nel caricamento di stage3, o prima ancora. */
typedef enum {
    PH_SCAN = 0,   /* prima che UniROM si annunci: scansione dell'exploit */
    PH_STAGE3,     /* dentro una lettura lunga 'B' */
    PH_POST,       /* dopo il primo comando MMCE: UniROM gira */
} mc_phase_t;

static volatile uint8_t  mc_phase;
static volatile uint8_t  mc_last_cmd;       /* opcode dopo lo 0x81 */
static volatile uint16_t mc_last_page;
static volatile uint16_t mc_max_page;
static volatile uint8_t  mc_last_offset;    /* byte dentro la pagina */
static volatile uint32_t mc_frames;         /* pagine servite per intero */
static volatile uint16_t mc_exit_site;      /* __LINE__ dell'ultima uscita */

#define MC_NOTE_EXIT()  do { mc_exit_site = (uint16_t)__LINE__; } while (0)

/* --- Le tre misure temporali ---------------------------------------------
 *
 * H1  push - ultimo RX      quando parte davvero l'ACK dopo l'ultimo byte
 * H2  RX successivo - push  quanto ci mette la console a rispondere all'ACK
 * H3  livello TX FIFO       se la FIFO si porta avanti (decide su ackwait)
 *
 * H1 e H2 divisi per fase, perche' la domanda e' proprio quale parametro agisca
 * su quale fase. Bucket in us: 0, <=1, <=2, <=4, <=8, <=16, <=32, <=64, oltre. */
#define MC_BUCKETS 9u

static volatile uint32_t h_ack_phase[3][MC_BUCKETS];   /* H1 */
static volatile uint32_t h_con_resp[3][MC_BUCKETS];    /* H2 */
static volatile uint32_t h_tx_level[9];                /* H3 */
static volatile uint32_t mc_timeouts;    /* H2 oltre 90 us: ACK non visto */
static volatile uint32_t mc_pushes, mc_rx;

static volatile uint32_t mc_last_rx_us;
static volatile uint32_t mc_last_push_us;

static inline uint8_t mc_bucket(uint32_t us) {
    if (us == 0u)  return 0u;
    if (us <= 1u)  return 1u;
    if (us <= 2u)  return 2u;
    if (us <= 4u)  return 3u;
    if (us <= 8u)  return 4u;
    if (us <= 16u) return 5u;
    if (us <= 32u) return 6u;
    if (us <= 64u) return 7u;
    return 8u;
}

/* --- Anello di eventi -----------------------------------------------------
 *
 * Serve a distinguere due blocchi che dai contatori sono identici: se l'ultima
 * riga e' una PUSH senza RX dopo, la console non ha visto il nostro ACK; se e'
 * una RX senza PUSH dopo, il passo l'abbiamo perso noi.
 *
 * Con "set tracestop 1" si congela al primo silenzio lungo dopo una push,
 * altrimenti l'anello si riavvolge mentre lo si legge. */
#define MCRING_SLOTS 256u
#define MCRING_SILENCE_US 200u

typedef struct {
    uint32_t t_us;
    uint8_t  type;
    uint8_t  a, b, c;
} mcring_entry_t;

enum { EV_RX = 0, EV_PUSH, EV_SELDN, EV_SELUP, EV_CMD, EV_PAGE, EV_ABORT };

static mcring_entry_t mcring[MCRING_SLOTS];
static volatile uint32_t mcring_written;
static volatile uint8_t  mcring_frozen;

/* Inizio del ciclo di /SEL corrente, per poterlo ANNULLARE.
 *
 * La console interroga il pad 51 volte al secondo - una per fotogramma, 20 ms -
 * e ogni interrogazione produce 5-6 eventi. L'anello ne tiene 256, quindi
 * qualunque transazione della memory card sparisce in meno di un secondo, e
 * durante le prove si finisce per guardare il polling del pad credendo di
 * guardare la card. E' successo per tre misure di fila.
 *
 * Qui, appena si vede che il primo byte del ciclo NON e' 0x81, si riporta
 * indietro il puntatore di scrittura all'inizio del ciclo: il traffico del
 * controller non entra proprio, e i 256 posti restano tutti per la card. */
static volatile uint32_t mcring_cycle_start;
static volatile uint8_t  mcring_skip;

static void __time_critical_func(mcring_put)(uint8_t type, uint8_t a, uint8_t b, uint8_t c) {
    if (mcring_frozen)
        return;
    if (type == EV_SELDN) {
        mcring_cycle_start = mcring_written;
        mcring_skip = 0;
    } else if (mcring_skip) {
        return;
    } else if ((type == EV_RX) && (mcring_written == mcring_cycle_start + 1u)) {
        /* primo byte dopo SEL: e' l'indirizzo, e dice di chi e' il ciclo */
        if (a != 0x81u) {
            mcring_written = mcring_cycle_start;
            mcring_skip = 1;
            return;
        }
    }
    const uint32_t i = mcring_written++;
    mcring_entry_t *e = &mcring[i % MCRING_SLOTS];
    e->t_us = timer_hw->timerawl;
    e->type = type;
    e->a = a; e->b = b; e->c = c;
}
#else
#define MC_NOTE_EXIT()  do { } while (0)
#endif /* PS1_MC_TUNING */

enum { RECEIVE_RESET, RECEIVE_EXIT, RECEIVE_OK };


static void __time_critical_func(reset_pio)(void) {
    const uint32_t mask = (1 << cmd_reader.sm) | (1 << dat_writer.sm)
                        | (1 << cntrl_reader.sm) | (1 << ack_pulser.sm);

    pio_set_sm_mask_enabled(pio0, mask, false);
    pio_restart_sm_mask(pio0, mask);

    pio_sm_exec(pio0, cmd_reader.sm, pio_encode_jmp(cmd_reader.offset));
    pio_sm_exec(pio0, dat_writer.sm, pio_encode_jmp(dat_writer.offset));
    pio_sm_exec(pio0, cntrl_reader.sm, pio_encode_jmp(cntrl_reader.offset));
    pio_sm_exec(pio0, ack_pulser.sm, pio_encode_jmp(ack_pulser.offset));
    /* Una richiesta rimasta appesa farebbe partire un impulso senza byte. */
    pio_interrupt_clear(pio0, 4);

    pio_sm_clear_fifos(pio0, cmd_reader.sm);
    pio_sm_clear_fifos(pio0, cntrl_reader.sm);
    pio_sm_drain_tx_fifo(pio0, dat_writer.sm);

    pio_enable_sm_mask_in_sync(pio0, mask);

    reset = 1;
}

/* Ricarica le tre SM con il profilo richiesto. Le lascia disabilitate, come
   faceva init_pio(): le riabilita il primo reset_pio() sul fronte di SEL. */
static void ps1_mc_apply_timing_profile(bool boot) {
    const uint clkdiv = boot ? PS1_CLKDIV_BOOT : PS1_CLKDIV;
    /* Ognuna prende il suo, e chi non e' stato impostato segue il profilo. */
    /* Tutte e tre seguono il divisore del profilo, come da sempre: provato che
       il pad funziona anche a 32, quindi il caso speciale non serviva. Le
       variabili di override esistono solo nella build di taratura. */
    const uint div_c = div_cmd  ? div_cmd  : clkdiv;
    const uint div_t = div_ctrl ? div_ctrl : clkdiv;
    const uint div_d = div_dat  ? div_dat  : clkdiv;

    cmd_reader_program_init(pio0, cmd_reader.sm, cmd_reader.offset, div_c);
    controller_program_init(pio0, cntrl_reader.sm, cntrl_reader.offset, div_t);

    /* Larghezza dell'ACK: si CALCOLA dal divisore che stiamo per applicare,
       perche' l'impulso si conta in cicli PIO e quindi la sua durata cambia con
       il divisore. Va riscritta dopo ogni caricamento, perche' pio_add_program()
       rimette le parole del sorgente. Vedi ps1_ack_cycles_for_div(). */
    const uint want = ack_cycles ? (uint)ack_cycles : ps1_ack_cycles_for_div(div_d);

    /* Un solo programma per entrambi i profili: cambia il divisore, il ritardo
       di testa e i parametri dell'impulso, non la forma del codice. */
    dat_writer_program_init(pio0, dat_writer.sm, dat_writer.offset, div_d);
    const uint head = (ack_head != 255u) ? (uint)ack_head
                    : (boot ? PS1_ACK_HEAD_BOOT : PS1_ACK_HEAD_STD);
    /* Ritorna i cicli DAVVERO programmati, che possono essere meno di quelli
       chiesti: la coppia (M, d) del ciclo di testa non rappresenta ogni
       valore. head_applied riporta quelli, perche' e' su questi che si
       ragiona leggendo mcget. */
    const uint head_got = dat_writer_set_ack_head(pio0, dat_writer.offset, head);
    head_applied = (uint8_t)((head_got > 255u) ? 255u : head_got);
    head_ns_applied = (uint32_t)((uint64_t)head_got * 1000000000u
                                 / (clock_get_hz(clk_sys) / div_d));
    /* La POSIZIONE dell'ACK: senza questa il pull successivo puo' riuscire prima
       che la console abbia finito di clockare il byte, a TX FIFO gia' piena. */
    dat_writer_set_ack_wait(pio0, dat_writer.offset, ack_wait_on);

    /* L'impulso lo genera la SM dedicata, allo stesso divisore. */
    ack_pulser_program_init(pio0, ack_pulser.sm, ack_pulser.offset, div_d);
    const uint got = ack_pulser_set_cycles(pio0, ack_pulser.offset, want);

    ack_width_cycles = (uint16_t)got;
    ack_width_ns = (uint32_t)((uint64_t)got * 1000000000u / (clock_get_hz(clk_sys) / div_d));

    /* Un impulso piu' corto di quello chiesto non deve poter partire in
       silenzio: succede quando il valore non e' rappresentabile dalla terna
       (d0, N, d1) del pulser, o quando il programma standard sfonda i suoi 8
       cicli. */
    if (got < want)
        log(LOG_ERROR, "PS1 ACK width: chiesti %u cicli, programmati %u (%u ns): "
                       "SOTTO il minimo di %u us, divisore %u troppo basso\n",
            want, got, (unsigned)ack_width_ns, PS1_ACK_MIN_US, div_d);

    gpio_set_slew_rate(PIN_PSX_DAT, GPIO_SLEW_RATE_SLOW);
    gpio_set_drive_strength(PIN_PSX_DAT, GPIO_DRIVE_STRENGTH_4MA);

    timing_boot = boot;
    timing_applied = true;
    /* Risolto qui, non a ogni byte: ps1_mc_respond() e' __time_critical_func e
       il profilo cambia solo al montaggio della card. Include gia' la
       condizione di boot, quindi la' basta un bool. */
    log(LOG_INFO, "PS1 timing profile: %s, divisore %u, ACK %u cicli (%u ns)\n",
        boot ? "boot" : "standard", div_d, got, (unsigned)ack_width_ns);
}

/* Il profilo boot vale solo sulla boot card, e solo se la chiave e' accesa. */
static bool ps1_mc_wants_boot_timing(void) {
#if PS1_MC_TUNING
    /* L'override vale anche ai rimontaggi della card, altrimenti basterebbe un
       cambio canale per riportare il profilo a quello vero e la prova
       misurerebbe altro senza dirlo. */
    if (profile_force == 1u) return false;
    if (profile_force == 2u) return true;
#endif
    return settings_get_ps1_fast_mode()
        && (ps1_cardman_get_state() == PS1_CM_STATE_BOOT);
}

static void __time_critical_func(init_pio)(void) {
    /* Set all pins as floating inputs */
    gpio_set_dir(PIN_PSX_ACK, 0);
    gpio_set_dir(PIN_PSX_SEL, 0);
    gpio_set_dir(PIN_PSX_CLK, 0);
    gpio_set_dir(PIN_PSX_CMD, 0);
    gpio_set_dir(PIN_PSX_DAT, 0);
    gpio_disable_pulls(PIN_PSX_ACK);
    gpio_disable_pulls(PIN_PSX_SEL);
    gpio_disable_pulls(PIN_PSX_CLK);
    gpio_disable_pulls(PIN_PSX_CMD);
    gpio_disable_pulls(PIN_PSX_DAT);

    cmd_reader.offset = pio_add_program(pio0, &cmd_reader_program);
    cmd_reader.sm = pio_claim_unused_sm(pio0, true);
    log(LOG_TRACE, "cmd_reader offset %d, sm %d\n", cmd_reader.offset, cmd_reader.sm);

    dat_writer.offset = pio_add_program(pio0, &dat_writer_program);
    dat_writer.sm = pio_claim_unused_sm(pio0, true);
    log(LOG_TRACE, "dat_writer offset %d, sm %d\n", dat_writer.offset, dat_writer.sm);

    /* Stesso programma di cmd_reader, e girano sullo STESSO offset: due SM
       possono condividere una copia caricata, cambia solo la configurazione dei
       pin. Caricarlo due volte costava 8 istruzioni per un programma di 4. */
    cntrl_reader.offset = cmd_reader.offset;
    cntrl_reader.sm = pio_claim_unused_sm(pio0, true);
    log(LOG_TRACE, "cntrl_reader offset %d, sm %d\n", cntrl_reader.offset, cntrl_reader.sm);

    /* La SM che genera l'impulso di /ACK. E' quella che tiene separati i due
       segnali: dat_writer chiede l'impulso e va dritto in ascolto del clock,
       invece di restare a pilotare /ACK mentre la console ricomincia a clockare.
       Senza di lei non usciremmo nessun ACK, quindi e' un errore fatale. */
    /* Non puo' succedere - 4 + 12 + 4 = 20 istruzioni su 32 - ma senza il pulser
       non usciremmo nessun ACK e la console dichiarerebbe la card assente, che e'
       un guasto silenzioso. Meglio fermarsi e dirlo. */
    if (!pio_can_add_program(pio0, &ack_pulser_program))
        fatal(ERR_MC_DATA, "PIO senza spazio per ack_pulser: nessun ACK possibile");
    ack_pulser.offset = pio_add_program(pio0, &ack_pulser_program);
    ack_pulser.sm = pio_claim_unused_sm(pio0, true);
    log(LOG_TRACE, "ack_pulser offset %d, sm %d\n", ack_pulser.offset, ack_pulser.sm);

    ps1_mc_apply_timing_profile(false);
    pio_ready = true;
}

static void __time_critical_func(card_deselected)(uint gpio, uint32_t event_mask) {
    if (gpio != PIN_PSX_SEL)
        return;

    if (event_mask & GPIO_IRQ_EDGE_RISE)
        reset_pio();

    /* Lo stato si prende dal livello reale del pin, non dal fronte.
     *
     * Il codice originale era un if/else sui due fronti: se salita e discesa
     * finiscono nella stessa interruzione - e capita, perche' il gestore
     * legge il registro degli eventi una volta sola e li acquisisce insieme -
     * vinceva il ramo della salita e card_active restava false per sempre.
     * Da li' in poi ogni respondOrNextCmd() esce subito, la card non manda piu'
     * ACK, e il payload che aspetta l'ACK senza timeout congela la console.
     *
     * card_active e' arrivato in sd2psXtd 1.2.1, la stessa versione in cui si e'
     * rotto il superfast (issue #59). Leggere il pin non puo' rimanere
     * disallineato. */
    card_active = (gpio_get(PIN_PSX_SEL) == 0);
#if PS1_MC_TUNING
    mcring_put(card_active ? EV_SELDN : EV_SELUP, 0, 0, 0);
#endif
}

static uint8_t __time_critical_func(recv_cmd)(uint8_t* cmd, uint32_t sm) {
    while (pio_sm_is_rx_fifo_empty(pio0, sm) && pio_sm_is_rx_fifo_empty(pio0, sm))  {
        if (mc_exit_request)
            return RECEIVE_EXIT;
        if (reset)
            return RECEIVE_RESET;
#if PS1_MC_TUNING
        /* Congela l'anello al primo silenzio lungo dopo una push: e' l'istante
           del blocco, e senza questo verrebbe sovrascritto dal traffico del pad
           mentre si legge il registro. */
        if (trace_stop && !mcring_frozen && mc_last_push_us
            && ((timer_hw->timerawl - mc_last_push_us) > MCRING_SILENCE_US))
            mcring_frozen = 1;
#endif
    }
    *cmd = (pio_sm_get(pio0, sm) >> 24);

#if PS1_MC_TUNING
    /* Solo il canale della memory card: il lettore del pad falserebbe tutto. */
    if (sm == cmd_reader.sm) {
        const uint32_t now = timer_hw->timerawl;
        mc_rx++;
        if (mc_last_push_us) {
            const uint32_t d = now - mc_last_push_us;
            h_con_resp[mc_phase % 3u][mc_bucket(d)]++;
            if (d > 90u)
                mc_timeouts++;
        }
        mc_last_rx_us = now;
        mcring_put(EV_RX, *cmd, 0, 0);
    }
#endif
    return RECEIVE_OK;
}

#define recv_cntrl(cmd) recv_cmd(cmd, cntrl_reader.sm)
#define recv_mc(cmd)    recv_cmd(cmd, cmd_reader.sm)

#define receiveOrNextCmd(cmd)              if ((recv_cmd(cmd, cmd_reader.sm) == RECEIVE_RESET) || !card_active)     { MC_NOTE_EXIT();    return;}

#define receiveOrNextCntrl(cmd)              if ((recv_cmd(cmd, cntrl_reader.sm) == RECEIVE_RESET) || !card_active)     {     return;}

/* Microsecondi di attesa prima di consegnare il byte al PIO, e quindi prima che
 * parta l'ACK. Solo sul profilo di boot.
 *
 * Serve al payload superfast, e la prova e' netta: senza, l'exploit si pianta.
 * Non e' un doppione della larghezza adattiva, che risolve un'altra cosa - il
 * loop del browser di UniROM, causato dall'impulso troppo stretto a divisore 32.
 * Servono tutte e due.
 *
 * Va detto come si e' rischiato di perderlo, perche' e' un errore facile da
 * rifare: nelle prove da seriale il ritardo veniva ricalcolato dall'ini a ogni
 * riapplicazione del profilo, quindi un "set ackdelay 0" seguito da qualunque
 * altro comando lo riaccendeva. La configurazione che avevo dichiarato
 * "validata a ritardo spento" girava in realta' con i 20 us attivi, e toglierli
 * ha rotto l'exploit.
 *
 * Se SEL risale durante l'attesa, reset_pio() ha gia' svuotato la coda del PIO:
 * scriverci dentro adesso lascerebbe un byte di troppo, che alla transazione
 * successiva farebbe partire un ACK prima ancora del comando. */
static void __time_critical_func(ps1_mc_respond)(uint8_t ch) {
#if PS1_MC_TUNING
    /* Stessa regola della release. "set delayall 1" la allarga a qualunque
       profilo per costruire configurazioni miste, ma e' spento di default:
       acceso, una prova con "set profile 1" si porterebbe dietro il ritardo del
       profilo di boot senza che si veda. "set mmcenodelay 0" riporta il ritardo
       anche sui comandi MMCE, cioe' al comportamento sbagliato, per confronto. */
    const bool profile_wants_delay = delay_all || timing_boot;
    const bool exempt = (mmce_cmd_in_progress && mmce_nodelay)
                     || (long_read_in_progress && longnodelay);
    const uint32_t delay = (profile_wants_delay && !exempt) ? ack_delay_us : 0u;
#else
    const uint32_t delay = (timing_boot && !mmce_cmd_in_progress
                                        && !long_read_in_progress)
                           ? ack_delay_us : 0u;
#endif
    if (delay) {
        const uint32_t start = timer_hw->timerawl;
        while ((timer_hw->timerawl - start) < delay) {
            if (reset || !card_active)
                return;
        }
    }
#if PS1_MC_TUNING
    {
        const uint32_t now = timer_hw->timerawl;
        /* H3: quanti byte ci sono gia' in coda. Se e' sempre 0 la FIFO non si
           porta mai avanti, e l'attesa dell'ottava salita e' inerte. */
        const uint level = pio_sm_get_tx_fifo_level(pio0, dat_writer.sm);
        h_tx_level[(level > 8u) ? 8u : level]++;
        /* H1: quanto dopo l'ultimo byte ricevuto parte l'ACK. */
        if (mc_last_rx_us)
            h_ack_phase[mc_phase % 3u][mc_bucket(now - mc_last_rx_us)]++;
        mc_last_push_us = now;
        mc_pushes++;
        mcring_put(EV_PUSH, ch, (uint8_t)level, 0);
    }
#endif
    pio_sm_put_blocking(pio0, dat_writer.sm, ~ch & 0xFF);
}

#define respondOrNextCmd(cmd)          \
    if (card_active) ps1_mc_respond(cmd);\
    else {DPRINTF("!RR: %s:%u\n", __func__, __LINE__); MC_NOTE_EXIT(); return;}

/*
  Send Reply Comment
  81h  N/A   Memory card address
  53h  FLAG  Send Get ID Command (ASCII "S"), Receive FLAG Byte
  00h  5Ah   Receive Memory Card ID1
  00h  5Dh   Receive Memory Card ID2
  00h  5Ch   Receive Command Acknowledge 1
  00h  5Dh   Receive Command Acknowledge 2
  00h  04h   Receive 04h
  00h  00h   Receive 00h
  00h  00h   Receive 00h
  00h  80h   Receive 80h
   */

static void __time_critical_func(mc_cmd_get_card_id)(void) {
    uint8_t _;
    respondOrNextCmd(0x5A); receiveOrNextCmd(&_);
    respondOrNextCmd(0x5D); receiveOrNextCmd(&_);
    respondOrNextCmd(0x5C); receiveOrNextCmd(&_);
    respondOrNextCmd(0x5D); receiveOrNextCmd(&_);
    respondOrNextCmd(0x04); receiveOrNextCmd(&_);
    respondOrNextCmd(0x00); receiveOrNextCmd(&_);
    respondOrNextCmd(0x00); receiveOrNextCmd(&_);
    respondOrNextCmd(0x80); receiveOrNextCmd(&_);
}

/*
  Send Reply Comment
  81h  N/A   Memory card address
  52h  FLAG  Send Read Command (ASCII "R"), Receive FLAG Byte
  00h  5Ah   Receive Memory Card ID1
  00h  5Dh   Receive Memory Card ID2
  MSB  (00h) Send Address MSB  ;\sector number (0..3FFh)
  LSB  (pre) Send Address LSB  ;/
  00h  5Ch   Receive Command Acknowledge 1  ;<-- late /ACK after this byte-pair
  00h  5Dh   Receive Command Acknowledge 2
  00h  MSB   Receive Confirmed Address MSB
  00h  LSB   Receive Confirmed Address LSB
  00h  ...   Receive Data Sector (128 bytes)
  00h  CHK   Receive Checksum (MSB xor LSB xor Data bytes)
  00h  47h   Receive Memory End Byte (should be always 47h="G"=Good for Read)
  */
/* Fase dati della lettura lunga: mette un byte nella TX FIFO senza pretendere
   in cambio il byte della PS1.
 *
 * Nella fase dati quei byte sono filler 0x00 che non servono a niente, e
 * pretenderli e' proprio il modo in cui un singolo fronte di clock perso
 * diventa un blocco definitivo: cmd_reader va fuori passo, recv_cmd() gira a
 * vuoto, il firmware smette di alimentare la TX FIFO, il PIO si ferma su "pull
 * block" e non emette piu' ACK. Il payload superfast aspetta l'ACK a ogni byte
 * senza timeout, quindi la console resta li' per sempre.
 *
 * La cadenza la fa da sola la stretta di mano: il PIO manda l'ACK e poi aspetta
 * i clock, la PS1 aspetta l'ACK. Se la FIFO si svuota il PIO si ferma su pull e
 * riparte da solo al byte successivo.
 *
 * Ritorna false quando la transazione e' finita (SEL risalito).
 *
 * ESISTE SOLO NELLA BUILD DI TARATURA, e solo per poter rimettere il difetto:
 * riempire la FIFO in anticipo fa uscire l'ACK per un byte che la console non
 * ha ancora chiesto, e superfast si desincronizza. Vedi il commento in
 * mc_cmd_read(). La release usa il lock-step e non chiama mai questa. */
#if PS1_MC_TUNING
static bool __time_critical_func(long_read_put)(uint8_t byte) {
#if PS1_MC_TUNING
    /* Il tetto al riempimento, e insieme l'unico riferimento temporale che qui
       si puo' avere: in questa fase core1 non legge la RX FIFO, quindi senza
       drenarla non si sa quando la console ha finito un byte e H1/H2 non
       misurerebbero niente. I byte sono filler gia' scartati per progetto. */
    const uint cap = (tx_depth > 8u) ? 8u : tx_depth;
    while (pio_sm_get_tx_fifo_level(pio0, dat_writer.sm) >= ((cap == 0u) ? 1u : cap)) {
        uint8_t discard;
        if (!pio_sm_is_rx_fifo_empty(pio0, cmd_reader.sm))
            (void)recv_cmd(&discard, cmd_reader.sm);
        if (reset || !card_active)
            return false;
    }
#else
    while (pio_sm_is_tx_fifo_full(pio0, dat_writer.sm)) {
        if (reset || !card_active)
            return false;   /* SEL risalito: transazione finita */
    }
#endif
    ps1_mc_respond(byte);
    return true;
}
#endif /* PS1_MC_TUNING */

static void __time_critical_func(mc_cmd_read)(bool long_read) {
    uint8_t page_msb = 0U, page_lsb = 0U;
    uint8_t offset = 0;
    uint8_t chk = 0;
    uint16_t page = 0U;
    uint8_t _;

    respondOrNextCmd(0x5A);       receiveOrNextCmd(&_);
    respondOrNextCmd(0x5D);       receiveOrNextCmd(&_);
    respondOrNextCmd(0x00);       receiveOrNextCmd(&page_msb);
    PS1_CLAMP_PAGE_MSB(page_msb);
    respondOrNextCmd(page_msb);   receiveOrNextCmd(&page_lsb);
    chk = page_msb ^ page_lsb;
    respondOrNextCmd(0x5C);       receiveOrNextCmd(&_);
    respondOrNextCmd(0x5D);       receiveOrNextCmd(&_);
    respondOrNextCmd(page_msb);   receiveOrNextCmd(&_);
    page = (page_msb << 8) | page_lsb;
#if PS1_MC_TUNING
    mcwrite_note_read(page);
    mc_last_page = page;
    if (page > mc_max_page) mc_max_page = page;
    mc_last_offset = 0;
    /* Una lettura lunga e' il caricamento di stage3; una corta prima che UniROM
       si annunci e' la scansione dei frame. Sono le due fasi che "freeze" da
       solo non distingue. */
    if (long_read && (mc_phase != PH_POST)) mc_phase = PH_STAGE3;
    mcring_put(EV_PAGE, (uint8_t)(page >> 8), (uint8_t)page, long_read ? 1u : 0u);
#endif
    ps1_mc_data_interface_setup_read_page(page);
    respondOrNextCmd(page_lsb);   receiveOrNextCmd(&_);

    if (!long_read) {
        curr_page = ps1_mc_data_interface_get_page(page);
        for (offset = 0; offset < 128; offset++) {
            ps1_mc_data_interface_wait_for_byte(offset);
#if PS1_MC_TUNING
            mc_last_offset = offset;
#endif
            respondOrNextCmd(curr_page[offset]);
            chk ^= curr_page[offset];
            receiveOrNextCmd(&_)
        }
        respondOrNextCmd(chk);        receiveOrNextCmd(&_);
        respondOrNextCmd(0x47);
        curr_page = NULL;
        DPRINTF("Read page %d done\n", page);
        return;
    }

    /* Il clamp non e' teorico: senza PSRAM get_page() ritorna &card[page*128] su
       un buffer di PS1_CARD_PAGE_COUNT pagine, e proseguire oltre significa
       leggere fuori, alla lunga fuori dalla RAM. */
    /* La fase dati va guidata dalla CONSOLE, un byte fuori per ogni byte
     * dentro, esattamente come la lettura singola qui sopra.
     *
     * Riempire la TX FIFO in anticipo sembra gratis e non lo e': il PIO emette
     * l'ACK quando il suo "pull" riesce, non quando la console ha finito di
     * clockare. Con un byte gia' in coda il pull riesce subito dopo l'ottavo
     * bit - prima ancora dell'ottava salita - e diciamo "pronto, prendi il
     * prossimo" per un byte che la console non ha chiesto. waitCardIRQ() del
     * payload lo vede e riparte in anticipo: da li' in poi i dati sono sfasati.
     *
     * Misurato il 04/09: con txdepth 0 superfast avvia e UniROM parte col Game
     * ID completo; con 1, 2, 4 e 8 si pianta sempre allo stesso punto - 948
     * frame caricati e poi crash eseguendo stage3. Il classico non ne soffriva
     * perche' usa letture singole 'R', che erano gia' in lock-step: era l'unico
     * percorso di codice che distingueva i due payload.
     *
     * Non e' una perdita di prestazioni: la lettura e' comunque scandita
     * dall'ACK, quindi la pipeline non faceva guadagnare un microsecondo. */
    /* Alzato SOLO intorno alla fase dati, non su tutto il comando: i byte
       dell'handshake e dell'indirizzo passano dal percorso normale, e solo il
       flusso dei 128 byte per pagina e' esente. */
    long_read_in_progress = true;
    bool running = true;
    while (running && (page < PS1_CARD_PAGE_COUNT)) {
        curr_page = ps1_mc_data_interface_get_page(page);
        for (offset = 0; offset < 128; offset++) {
            uint8_t discard;
            ps1_mc_data_interface_wait_for_byte(offset);
#if PS1_MC_TUNING
            mc_last_offset = offset;
            /* Il difetto resta riproducibile dalla build di taratura, perche'
               serve poterlo rimettere per un A/B. Nella release non esiste. */
            if (tx_depth > 0u) {
                if (!long_read_put(curr_page[offset])) {
                    running = false;
                    break;
                }
                continue;
            }
#endif
            respondOrNextCmd(curr_page[offset]);
            receiveOrNextCmd(&discard);
        }
        if (running) {
            page++;
#if PS1_MC_TUNING
            mc_frames++;
            mc_last_page = page;
            if (page > mc_max_page) mc_max_page = page;
#endif
            ps1_mc_data_interface_setup_read_page(page);
        }
    }

    long_read_in_progress = false;
    curr_page = NULL;
}


/**
  Send Reply Comment
  81h  N/A   Memory card address
  57h  FLAG  Send Write Command (ASCII "W"), Receive FLAG Byte
  00h  5Ah   Receive Memory Card ID1
  00h  5Dh   Receive Memory Card ID2
  MSB  (00h) Send Address MSB  ;\sector number (0..3FFh)
  LSB  (pre) Send Address LSB  ;/
  ...  (pre) Send Data Sector (128 bytes)
  CHK  (pre) Send Checksum (MSB xor LSB xor Data bytes)
  00h  5Ch   Receive Command Acknowledge 1
  00h  5Dh   Receive Command Acknowledge 2
  00h  4xh   Receive Memory End Byte (47h=Good, 4Eh=BadChecksum, FFh=BadSector)
   */
static void __time_critical_func(mc_cmd_write)(void) {
    uint8_t page_msb = 0U, page_lsb = 0U;
    uint8_t offset = 0;
    uint8_t in = 0;
    uint8_t prev;
    uint8_t chk = 0;
    uint8_t _;

    flag = 0;

    respondOrNextCmd(0x5A);       receiveOrNextCmd(&_);
    respondOrNextCmd(0x5D);       receiveOrNextCmd(&_);
    respondOrNextCmd(0x00);       receiveOrNextCmd(&page_msb);
    PS1_CLAMP_PAGE_MSB(page_msb);
    respondOrNextCmd(page_msb);   receiveOrNextCmd(&page_lsb);
    chk = page_msb ^ page_lsb;
    prev = page_lsb;
#if PS1_MC_TUNING
    uint8_t first4[4] = {0, 0, 0, 0};
#endif
    for (offset = 0; offset < 128; offset++) {
        respondOrNextCmd(prev);   receiveOrNextCmd(&in);
        ps1_mc_data_interface_write_byte(((page_msb * 256) + page_lsb) * 128 + offset, in);
#if PS1_MC_TUNING
        if (offset < 4u) first4[offset] = in;
#endif
        chk ^= in;
        prev = in;
    }
    respondOrNextCmd(prev);           receiveOrNextCmd(&in);
    respondOrNextCmd(0x5C);           receiveOrNextCmd(&_);
    respondOrNextCmd(0x5D);           receiveOrNextCmd(&_);
#if PS1_MC_TUNING
    {
        const uint16_t wpage = (uint16_t)((page_msb * 256) + page_lsb);
        if (wpage < PS1_CARD_PAGE_COUNT)
            written_pages[wpage >> 3] |= (uint8_t)(1u << (wpage & 7u));
        mcwrite_put(wpage, (in == chk) ? 1u : 0u, 0u, first4);
    }
#endif
    if (in == chk) {
        ps1_mc_data_interface_write_mc((page_msb * 256) + page_lsb);
        respondOrNextCmd(0x47);
    } else {
        respondOrNextCmd(0x4E);
    }
}

static void __time_critical_func(mc_mmce_ping)(void) {
    uint8_t _;
    respondOrNextCmd(0x00);   receiveOrNextCmd(&_);
    respondOrNextCmd(0x00);   receiveOrNextCmd(&_);
    respondOrNextCmd(0x27);   receiveOrNextCmd(&_);
    respondOrNextCmd(0xFF);   receiveOrNextCmd(&_);

#if PS1_MMCE_TRACE
    /* Solo qui il ping e' davvero concluso: sopra ci sono quattro punti in cui
       si esce se la console deseleziona la card. */
    ps1_mc_mmcelog_put(MMCELOG_PING_DONE, 0, 0);
#endif
}

/* --- Game ID (comando MMCE 0x21) ------------------------------------------
 *
 * Serve a montare una memory card diversa per ogni gioco. Il punto da capire e'
 * che la PS1 non dice MAI alla memory card cosa sta girando: sul bus passano
 * solo comandi da memory card. Il titolo lo deve annunciare qualcun altro,
 * PRIMA che il gioco parta, e i mittenti possibili sono tre:
 *
 *   - una BIOS patchata, che legge l'identificativo del disco e lo manda da se'
 *     (vedi github.com/jdfr228/PS1-Disc-Based-Game-ID). E' l'unico modo per un
 *     gioco normale da CD;
 *   - un ODE o un launcher che parla MMCE, che sa quale immagine sta montando;
 *   - il comando seriale "set game <id>", a mano dal PC.
 *
 * Senza nessuno dei tre non c'e' nessun Game ID, il firmware salta la card per
 * gioco e serve la card predefinita. La chiave GameId dell'ini resta allora
 * semplicemente inattiva: non fa danni.
 *
 * Struttura sul filo, dopo l'indirizzo 0x81 e l'opcode 0x21:
 *
 *     0x00        byte riservato
 *     length      strlen(ID) + 3
 *     payload     ID + ';' + numero + 0x00      es. i byte di "SLUS-00594;1" piu' il terminatore
 *
 * length NON e' la lunghezza dell'ID: conta anche il separatore, il numero e il
 * terminatore. La stessa macchina (game_db, stato GAMEID del cardman, cartella
 * col nome dell'ID) e' condivisa con il lato PS2, dove il mittente e' OPL e la
 * funzione e' di uso quotidiano. */
static void __time_critical_func(mc_mmce_set_game_id)(void) {
    uint8_t length = 0;
    uint8_t game_id[UINT8_MAX] = {0};
    char received_game_id[16];
    uint8_t prev = 0;
    uint8_t _;
    memset(received_game_id, 0, sizeof(received_game_id));
    respondOrNextCmd(0x00);   receiveOrNextCmd(&_);       // Reserved
    respondOrNextCmd(0x00);   receiveOrNextCmd(&length);  // length
    prev = length;
#if PS1_MMCE_TRACE
    gid_len_expected = length;
    gid_n = 0;
    gid_ts = timer_hw->timerawl;
#endif

    for (uint8_t i = 0; i < length; i++) {
        respondOrNextCmd(prev);   receiveOrNextCmd(&game_id[i]);
        prev = game_id[i];
#if PS1_MMCE_TRACE
        if (gid_n < sizeof(gid_buf))
            gid_buf[gid_n++] = game_id[i];
#endif
    }
    game_id[length == UINT8_MAX ? UINT8_MAX - 1 : length] = 0x00;

#if PS1_MMCE_TRACE
    /* Codice distinto da 0x21: quello dice solo che il comando e' arrivato,
       questo che la stringa e' stata letta per intero. Lunghezza e primo byte
       distinguono "ID vuoto" da "ID che non supera il controllo". */
    ps1_mc_mmcelog_put(MMCELOG_GID_DONE, length, game_id[0]);
#endif
    ps1_mmce_set_gameid(game_id);
}

static void __time_critical_func(mc_mmce_prev_channel)(void) {
    uint8_t _;
    respondOrNextCmd(0x00);   receiveOrNextCmd(&_);
    respondOrNextCmd(0x00);   receiveOrNextCmd(&_);
    respondOrNextCmd(0x20);   receiveOrNextCmd(&_);
    respondOrNextCmd(0xFF);   receiveOrNextCmd(&_);
    ps1_mmce_prev_ch(false);
}

static void __time_critical_func(mc_mmce_next_channel)(void) {
    uint8_t _;
    respondOrNextCmd(0x00);   receiveOrNextCmd(&_);
    respondOrNextCmd(0x00);   receiveOrNextCmd(&_);
    respondOrNextCmd(0x20);   receiveOrNextCmd(&_);
    respondOrNextCmd(0xFF);   receiveOrNextCmd(&_);
    ps1_mmce_next_ch(false);
}

static void __time_critical_func(mc_mmce_prev_index)(void) {
    uint8_t _;
    respondOrNextCmd(0x00);   receiveOrNextCmd(&_);
    respondOrNextCmd(0x00);   receiveOrNextCmd(&_);
    respondOrNextCmd(0x20);   receiveOrNextCmd(&_);
    respondOrNextCmd(0xFF);   receiveOrNextCmd(&_);
    ps1_mmce_prev_idx(false);
}

static void __time_critical_func(mc_mmce_next_index)(void) {
    uint8_t _;
    respondOrNextCmd(0x00);   receiveOrNextCmd(&_);
    respondOrNextCmd(0x00);   receiveOrNextCmd(&_);
    respondOrNextCmd(0x20);   receiveOrNextCmd(&_);
    respondOrNextCmd(0xFF);   receiveOrNextCmd(&_);
    ps1_mmce_next_idx(false);
}

static void __time_critical_func(mc_mmce_reset)(void) {
    uint8_t _;
    respondOrNextCmd(0x00);   receiveOrNextCmd(&_);
    respondOrNextCmd(0x00);   receiveOrNextCmd(&_);
    respondOrNextCmd(0x20);   receiveOrNextCmd(&_);
    respondOrNextCmd(0xFF);   receiveOrNextCmd(&_);
    ps1_mmce_reset(false);
}

/**
  01h  Hi-Z  Controller address
  42h  idlo  Receive ID bit0..7 (variable) and Send Read Command (ASCII "B")
  TAP  idhi  Receive ID bit8..15 (usually/always 5Ah)
  MOT  swlo  Receive Digital Switches bit0..7
  MOT  swhi  Receive Digital Switches bit8..15
  --------

  Switch Bits:
  0   Select Button    (0=Pressed, 1=Released)
  1   L3/Joy-button    (0=Pressed, 1=Released/None/Disabled) ;analog mode only
  2   R3/Joy-button    (0=Pressed, 1=Released/None/Disabled) ;analog mode only
  3   Start Button     (0=Pressed, 1=Released)
  4   Joypad Up        (0=Pressed, 1=Released)
  5   Joypad Right     (0=Pressed, 1=Released)
  6   Joypad Down      (0=Pressed, 1=Released)
  7   Joypad Left      (0=Pressed, 1=Released)


  8   L2 Button        (0=Pressed, 1=Released) (Lower-left shoulder)
  9   R2 Button        (0=Pressed, 1=Released) (Lower-right shoulder)
  10  L1 Button        (0=Pressed, 1=Released) (Upper-left shoulder)
  11  R1 Button        (0=Pressed, 1=Released) (Upper-right shoulder)
  12  /\ Button        (0=Pressed, 1=Released) (Triangle, upper button)
  13  () Button        (0=Pressed, 1=Released) (Circle, right button)
  14  >< Button        (0=Pressed, 1=Released) (Cross, lower button)
  15  [] Button        (0=Pressed, 1=Released) (Square, left button)
 */
/* Modalita' di cambio card attiva. Scritta da core1 (dove gira l'emulazione) e
   letta da core0, che pilota i LED: da qui non si puo' toccare l'hardware dei
   LED direttamente, quindi si espone lo stato e ci pensa led_task(). */
static volatile bool switch_card_combo_enabled = false;

bool ps1_memory_card_combo_active(void) {
    return switch_card_combo_enabled;
}

/* Sola lettura della boot card: fissa a compile time, senza manopole. Un
   interruttore a runtime qui sarebbe un rischio e basta - basta dimenticarlo
   spento per rovinare l'immagine e invalidare in silenzio le prove seguenti.
   Nessun contatore: sono __time_critical_func, chiamate per ogni byte. */
/* La regola e' la STESSA nelle due build: solo la boot card e' in sola lettura,
   le card normali restano scrivibili. Cosi' la build di taratura misura il
   comportamento che spediamo davvero, invece di una variante piu' prudente che
   nasconderebbe proprio gli effetti delle scritture.

   Chi la governa sono i due flag a compile time PS1_BOOTCARD_SD_READONLY e
   PS1_BOOTCARD_RAM_READONLY (ps1_memory_card.h): mettendoli a 0 si toglie la
   protezione anche alla boot card, ed e' l'unico modo previsto per farlo.

   Per un periodo la taratura ha protetto TUTTE le card, dopo che una prova
   sulla gestione file del BIOS aveva scritto su una card normale. La protezione
   estesa e' stata tolta perche' rendeva le prove meno rappresentative; il
   rischio resta reale, e si gestisce sapendo che durante le prove su card
   normale la console PUO' modificarne il contenuto. mcget lo dice: i campi
   readonly_now_sd / readonly_now_ram riportano se la protezione morde sulla
   card montata in quel momento, non il valore del flag. */
bool __time_critical_func(ps1_mc_bootcard_sd_write_denied)(void) {
#if PS1_BOOTCARD_SD_READONLY
    return ps1_cardman_get_state() == PS1_CM_STATE_BOOT;
#else
    return false;
#endif
}

bool __time_critical_func(ps1_mc_bootcard_ram_write_denied)(void) {
#if PS1_BOOTCARD_RAM_READONLY
    return ps1_cardman_get_state() == PS1_CM_STATE_BOOT;
#else
    return false;
#endif
}

static void mc_read_controller(void) {
    static uint8_t prevCommand = 0;
    uint8_t controller_in[2];
    uint8_t _ = 0x00;

    receiveOrNextCmd(&_);
    if (_ == (uint8_t)'B') {    // Only reactive to "read buttons" command
        receiveOrNextCntrl(&_); // Hi-Z
        receiveOrNextCntrl(&_);
        if ((_== 0x00) || (_ == 0xFF))
            return;
        receiveOrNextCntrl(&_);
        if (_ != 0x5A)
            return;
        for (uint8_t i = 0; i < 2; i++) {
            receiveOrNextCntrl(&controller_in[i]);
        }
        /* Combo di cambio card.
         *
         * SELECT + L1 + L2 premuti insieme attivano la modalita' di switch.
         * La modalita' resta attiva finche' almeno uno dei tre e' ancora
         * premuto: si esce solo quando vengono rilasciati tutti e tre. Cosi'
         * si possono fare piu' cambi di seguito tenendo giu' i soli dorsali,
         * senza dover ripremere SELECT ogni volta.
         *
         * Dentro la modalita' le direzioni del dpad cambiano card e canale, e
         * il comando parte al *rilascio* della direzione. */

        /* controller_in[0] */
        #define BTN_SEL 0b00000001
        #define BTN_STA 0b00001000
        #define BTN_UP  0b00010000
        #define BTN_RGT 0b00100000
        #define BTN_DWN 0b01000000
        #define BTN_LFT 0b10000000
        #define BTN_DPAD (BTN_UP | BTN_RGT | BTN_DWN | BTN_LFT)
        /* Tasti che generano un comando: il comando parte quando sono tutti
           rilasciati, START compreso. */
        #define BTN_TRIGGERS (BTN_DPAD | BTN_STA)

        /* controller_in[1] */
        #define BTN_L2  0b00000001
        #define BTN_L1  0b00000100

        /* I tasti sono attivi bassi: 0 = premuto. */
        #define IS_PRESSED(x,y)   (((x) & (y)) == 0)
        #define NONE_PRESSED(x,y) (((x) & (y)) == (y))

        const bool sel = IS_PRESSED(controller_in[0], BTN_SEL);
        const bool l1  = IS_PRESSED(controller_in[1], BTN_L1);
        const bool l2  = IS_PRESSED(controller_in[1], BTN_L2);

        if (!switch_card_combo_enabled) {
            if (sel && l1 && l2)
                switch_card_combo_enabled = true;
        } else if (!sel && !l1 && !l2) {
            switch_card_combo_enabled = false;
            prevCommand = 0;
        }

        if (switch_card_combo_enabled) {
            if (prevCommand == 0) {
                if (IS_PRESSED(controller_in[0], BTN_UP)) {
                    prevCommand = MMCE_PS1_NXT_CARD;
                } else if (IS_PRESSED(controller_in[0], BTN_DWN)) {
                    prevCommand = MMCE_PS1_PRV_CARD;
                } else if (IS_PRESSED(controller_in[0], BTN_LFT)) {
                    prevCommand = MMCE_PS1_PRV_CH;
                } else if (IS_PRESSED(controller_in[0], BTN_RGT)) {
                    prevCommand = MMCE_PS1_NXT_CH;
                } else if (IS_PRESSED(controller_in[0], BTN_STA)) {
                    prevCommand = MMCE_PS1_SWITCH_BOOTCARD;
                }
            } else if (NONE_PRESSED(controller_in[0], BTN_TRIGGERS)) {
                switch (prevCommand) {
                    case MMCE_PS1_NXT_CARD:
                        ps1_mmce_next_idx(false);
                        break;
                    case MMCE_PS1_PRV_CARD:
                        ps1_mmce_prev_idx(false);
                        break;
                    case MMCE_PS1_NXT_CH:
                        ps1_mmce_next_ch(false);
                        break;
                    case MMCE_PS1_PRV_CH:
                        ps1_mmce_prev_ch(false);
                        break;
                    case MMCE_PS1_SWITCH_BOOTCARD:
                        ps1_mmce_switch_bootcard(false);
                        break;
                }
                prevCommand = 0;
            }
        }
    }

}

static void __time_critical_func(mc_main_loop)(void) {
    flag = 8;

    while (1) {
        uint8_t ch = 0x00;

        while (!reset && !reset && !reset && !reset && !reset) {
            if (mc_exit_request) {
                mc_exit_response = 1;
                return;
            }
        }
        reset = 0;
        uint8_t received = recv_mc(&ch);

        if (received != RECEIVE_OK) {
            if (received == RECEIVE_EXIT) {
                mc_exit_response = 1;
                break;
            }
            /* If this ch belongs to the next command sequence */
            if (received == RECEIVE_RESET) {
                continue;
            }
        }

        if (0x81 == ch) { /* Command is for MC - process! */
            /* Istante dell'ultimo comando ricevuto dalla card. Lo usa il timeout
               della boot card, che e' di INATTIVITA' e non assoluto: mentre
               l'exploit legge non deve poter scadere. Una sola store a 32 bit,
               niente time_us_64() qui dentro. */
            mc_last_activity_us = timer_hw->timerawl;
            ps1_mc_respond(flag);

            if (recv_mc(&ch) == RECEIVE_RESET)
                continue;

#if PS1_MMCE_TRACE
            if ((ch >= 0x20) && (ch <= 0x27))
                ps1_mc_mmcelog_put(ch, 0, 0);
#endif
            /* Alzato prima e abbassato dopo: i gestori possono uscire a meta'
               con un return dai macro, ma il controllo torna sempre qui. */
            mmce_cmd_in_progress = ((ch >= 0x20) && (ch <= 0x27));
            /* Azzerato a OGNI comando, non solo in fondo a mc_cmd_read():
               le macro respondOrNextCmd/receiveOrNextCmd escono con un
               return se SEL risale, e una lettura lunga interrotta a meta'
               lascerebbe il flag alzato. Da li' in poi le letture del BIOS
               salterebbero il ritardo IN SILENZIO, che e' esattamente il
               tipo di guasto che costa una giornata. */
            long_read_in_progress = false;
#if PS1_MC_TUNING
            mc_last_cmd = ch;
            /* Il primo comando MMCE e' l'annuncio di UniROM: da li' in poi le
               letture sono del browser, non piu' dell'exploit. */
            if (mmce_cmd_in_progress) mc_phase = PH_POST;
            mcring_put(EV_CMD, ch, (uint8_t)mc_phase, 0);
#endif

            switch (ch) {
                case 0x20: mc_mmce_ping(); break;
                case 0x21: mc_mmce_set_game_id(); break;
                case 0x22: mc_mmce_prev_channel(); break;
                case 0x23: mc_mmce_next_channel(); break;
                case 0x24: mc_mmce_prev_index(); break;
                case 0x25: mc_mmce_next_index(); break;
                case 0x27: mc_mmce_reset(); break;
                case 'B': mc_cmd_read(true); break;
                case 'R': mc_cmd_read(false); break;
                case 'S': mc_cmd_get_card_id(); break;
                case 'W': mc_cmd_write(); break;
                default: DPRINTF("Unknown command: 0x%.02x\n", ch); break;
            }
            mmce_cmd_in_progress = false;
        } else if ((0x01 == ch) && (settings_get_ps1_controllercombo())) {
            mc_read_controller();
        } else if ((0x21 == ch) && !ps2_multitap) {
            ps1_mc_respond(0x00);

            if (RECEIVE_RESET == recv_mc(&ch))
                continue;

            if (0x53 == ch) {
                ps1_mc_respond(0x0F);
            } else if (ch == 0x21) {      // PS2 multitap is also sending 0x21 as configuration command
                ps2_multitap = true;
            }
        } else {
        }

    }

}

static void __no_inline_not_in_flash_func(mc_main)(void) {
    while (1) {
        while (!mc_enter_request)
        {}
        mc_enter_response = 1;

        mc_main_loop();
    }
}

static gpio_irq_callback_t callbacks[NUM_CORES];

static void __time_critical_func(RAM_gpio_acknowledge_irq)(uint gpio, uint32_t events) {
    check_gpio_param(gpio);
    iobank0_hw->intr[gpio / 8] = events << (4 * (gpio % 8));
}

static void __time_critical_func(RAM_gpio_default_irq_handler)(void) {
    uint core = get_core_num();
    gpio_irq_callback_t callback = callbacks[core];
    io_bank0_irq_ctrl_hw_t *irq_ctrl_base = core ? &iobank0_hw->proc1_irq_ctrl : &iobank0_hw->proc0_irq_ctrl;
    for (uint gpio = 0; gpio < NUM_BANK0_GPIOS; gpio+=8) {
        uint32_t events8 = irq_ctrl_base->ints[gpio >> 3u];
        // note we assume events8 is 0 for non-existent GPIO
        for(uint i=gpio;events8 && i<gpio+8;i++) {
            uint32_t events = events8 & 0xfu;
            if (events) {
                RAM_gpio_acknowledge_irq(i, events);
                if (callback) {
                    callback(i, events);
                }
            }
            events8 >>= 4;
        }
    }
}

static void my_gpio_set_irq_callback(gpio_irq_callback_t callback) {
    uint core = get_core_num();
    if (callbacks[core]) {
        if (!callback) {
            irq_remove_handler(IO_IRQ_BANK0, RAM_gpio_default_irq_handler);
        }
        callbacks[core] = callback;
    } else if (callback) {
        callbacks[core] = callback;
        irq_add_shared_handler(IO_IRQ_BANK0, RAM_gpio_default_irq_handler, GPIO_IRQ_CALLBACK_ORDER_PRIORITY);
    }
}

static void my_gpio_set_irq_enabled_with_callback(uint gpio, uint32_t events, bool enabled, gpio_irq_callback_t callback) {
    gpio_set_irq_enabled(gpio, events, enabled);
    my_gpio_set_irq_callback(callback);
    if (enabled) irq_set_enabled(IO_IRQ_BANK0, true);
}

#if PS1_MC_TUNING
/* --- Taratura: leggere e scrivere i parametri da seriale -------------------
 * Su core0, fuori da qualunque percorso critico. */
void ps1_mc_tune_set(const char *what, uint32_t value) {
    if (!strcmp(what, "ackwidth"))      ack_cycles = (uint16_t)value;
    else if (!strcmp(what, "ackdelay")) { ack_delay_us = value; return; }
    else if (!strcmp(what, "datpio"))   div_dat = (uint16_t)value;
    else if (!strcmp(what, "rxpio"))    div_cmd = (uint16_t)value;
    else if (!strcmp(what, "ctrlpio"))  div_ctrl = (uint16_t)value;
    else if (!strcmp(what, "ackwait"))  ack_wait_on = (uint8_t)(value ? 1u : 0u);
    /* 255 e' il valore sentinella "segui il profilo", e va lasciato passare:
       troncandolo a PS1_ACK_HEAD_MAX come un valore qualunque si perdeva il
       modo di TORNARE al comportamento normale dopo averlo forzato, e ogni
       rilettura di mctest.py avrebbe visto 31 al posto dei 255 chiesti. */
    else if (!strcmp(what, "ackhead"))  ack_head = (uint8_t)((value == 255u) ? 255u
                                                  : ((value > PS1_ACK_HEAD_MAX) ? PS1_ACK_HEAD_MAX : value));
    else if (!strcmp(what, "txdepth"))  { tx_depth = (uint8_t)((value > 8u) ? 8u : value); return; }
    else if (!strcmp(what, "tracestop")) { trace_stop = (uint8_t)(value ? 1u : 0u); return; }
    else if (!strcmp(what, "mmcenodelay")) { mmce_nodelay = (uint8_t)value; return; }
    else if (!strcmp(what, "longnodelay")) { longnodelay = (uint8_t)(value ? 1u : 0u); return; }
    else if (!strcmp(what, "delayall")) { delay_all = (uint8_t)(value ? 1u : 0u); return; }
    else if (!strcmp(what, "boottimeout")) { boottimeout_force = (uint8_t)value; return; }
    else if (!strcmp(what, "profile")) {
        profile_force = (uint8_t)value;
        if (timing_applied)
            ps1_mc_apply_timing_profile(ps1_mc_wants_boot_timing());
        return;
    }
    else return;
    if (timing_applied)
        ps1_mc_apply_timing_profile(timing_boot);
}

/* --- Uscita per un programma: chiave=valore, una per riga, chiusa da "end" ---
 *
 * mcstat e status restano per la lettura umana. Questa serve a mctest.py, che
 * dopo ogni impostazione rilegge e verifica invece di fidarsi dell'eco: e' il
 * modo per non armare piu' una prova credendo che la card sia dove non e'. */
static void mcget_hist(const char *name, const volatile uint32_t *h, uint n) {
    printf("%s=", name);
    for (uint i = 0; i < n; i++)
        printf("%u%s", (unsigned)h[i], (i + 1u < n) ? "," : "\n");
}

/* L'anello esisteva ma non c'era modo di leggerlo, e senza di esso le prove
   sui fronti restano a meta': i contatori dicono CHE la trasmissione si rompe,
   non QUANDO arrivano i byte rispetto al nostro impulso.

   Formato pensato per essere letto da mctest.py: una riga per evento, tempo in
   microsecondi ASSOLUTO piu' il delta dal precedente, che e' quello che serve
   davvero. Si stampa dal piu' vecchio al piu' recente. */
void ps1_mc_ring_print(void) {
    static const char *const names[] = {
        "RX", "PUSH", "SEL_LOW", "SEL_HIGH", "CMD", "PAGE", "ABORT"
    };
    const uint32_t tot = mcring_written;
    const uint32_t n   = (tot < MCRING_SLOTS) ? tot : MCRING_SLOTS;
    const uint32_t first = tot - n;

    printf("ring_total=%u\n", (unsigned)tot);
    printf("ring_shown=%u\n", (unsigned)n);
    printf("ring_frozen=%u\n", (unsigned)mcring_frozen);
    printf("ring_ackwidth_ns=%u\n", (unsigned)ack_width_ns);
    printf("ring_begin\n");
    uint32_t prev = 0;
    for (uint32_t k = 0; k < n; k++) {
        const mcring_entry_t *e = &mcring[(first + k) % MCRING_SLOTS];
        const uint32_t d = k ? (e->t_us - prev) : 0u;
        printf("%u %u %s %u %u %u\n",
               (unsigned)(first + k), (unsigned)e->t_us,
               (e->type < 7u) ? names[e->type] : "?",
               (unsigned)e->a, (unsigned)e->b, (unsigned)e->c);
        (void)d;
        prev = e->t_us;
    }
    printf("ring_end\n");
}

void ps1_mc_get_print(void) {
    const uint clkdiv = timing_boot ? PS1_CLKDIV_BOOT : PS1_CLKDIV;

    /* --- posizione: va verificata PRIMA di misurare qualunque cosa --- */
    printf("mode=%s\n",    (settings_get_mode(true) == MODE_PS2) ? "PS2" : "PS1");
    printf("running=%s\n", (main_get_running_mode() == MODE_PS2) ? "PS2" : "PS1");
#if WITH_MSC
    printf("usb=%d\n", (int)msc_mode_state());
#else
    printf("usb=0\n");
#endif
    printf("card_state=%d\n", (int)ps1_cardman_get_state());
    printf("card_idx=%d\n",   ps1_cardman_get_idx());
    printf("card_ch=%d\n",    ps1_cardman_get_channel());

    /* --- knob richiesti --- */
    printf("req_ackwidth=%u\n", (unsigned)ack_cycles);
    printf("req_ackdelay=%u\n", (unsigned)ack_delay_us);
    printf("req_ackwait=%u\n",  (unsigned)ack_wait_on);
    printf("req_txdepth=%u\n",  (unsigned)tx_depth);
    printf("req_ackhead=%u\n",  (unsigned)ack_head);
    printf("eff_ackhead=%u\n",  (unsigned)head_applied);
    printf("eff_ackhead_ns=%u\n", (unsigned)head_ns_applied);
    printf("req_delayall=%u\n", (unsigned)delay_all);
    printf("req_mmcenodelay=%u\n", (unsigned)mmce_nodelay);
    printf("req_longnodelay=%u\n", (unsigned)longnodelay);
    printf("req_tracestop=%u\n",   (unsigned)trace_stop);
    printf("req_profile=%u\n",     (unsigned)profile_force);
    printf("req_datpio=%u\n", (unsigned)div_dat);
    printf("req_rxpio=%u\n",  (unsigned)div_cmd);
    printf("req_ctrlpio=%u\n",(unsigned)div_ctrl);

    /* --- stato effettivo del PIO --- */
    printf("eff_applied=%u\n",  timing_applied ? 1u : 0u);
    printf("eff_boot=%u\n",     timing_boot ? 1u : 0u);
    printf("eff_ackwidth=%u\n", (unsigned)ack_width_cycles);
    printf("eff_ackwidth_ns=%u\n", (unsigned)ack_width_ns);
    printf("eff_ackmin_ns=%u\n",   (unsigned)(PS1_ACK_MIN_US * 1000u));
    /* Serve a mctest.py per convertire una durata in cicli: senza, le prove
       dovrebbero parlare in cicli, e la stessa cifra varrebbe durate diverse
       a divisori diversi. E' la trappola che ha gia' fatto danni. */
    printf("clk_hz=%u\n", (unsigned)clock_get_hz(clk_sys));
    printf("eff_datpio=%u\n", (unsigned)(div_dat  ? div_dat  : clkdiv));
    printf("eff_rxpio=%u\n",  (unsigned)(div_cmd  ? div_cmd  : clkdiv));
    printf("eff_ctrlpio=%u\n",(unsigned)(div_ctrl ? div_ctrl : clkdiv));
    printf("eff_delay_active=%u\n",
           ((delay_all || timing_boot) && ack_delay_us) ? 1u : 0u);
    printf("readonly_sd=%u\n",  PS1_BOOTCARD_SD_READONLY  ? 1u : 0u);
    printf("readonly_ram=%u\n", PS1_BOOTCARD_RAM_READONLY ? 1u : 0u);
    /* I due sopra sono i FLAG DI COMPILAZIONE. Questi due dicono se la
       protezione morde sulla card montata ADESSO: con una card normale i primi
       due dicevano "protetta" mentre le scritture passavano, ed e' costato una
       rassicurazione sbagliata durante una sessione. */
    printf("readonly_now_sd=%u\n",  ps1_mc_bootcard_sd_write_denied()  ? 1u : 0u);
    printf("readonly_now_ram=%u\n", ps1_mc_bootcard_ram_write_denied() ? 1u : 0u);

    /* --- dove si e' fermato --- */
    printf("phase=%u\n",       (unsigned)mc_phase);
    printf("last_cmd=%u\n",    (unsigned)mc_last_cmd);
    printf("last_page=%u\n",   (unsigned)mc_last_page);
    printf("max_page=%u\n",    (unsigned)mc_max_page);
    printf("last_offset=%u\n", (unsigned)mc_last_offset);
    printf("frames=%u\n",      (unsigned)mc_frames);
    printf("exit_site=%u\n",   (unsigned)mc_exit_site);
    printf("idle_us=%u\n",     (unsigned)(timer_hw->timerawl - mc_last_activity_us));

    /* --- contatori --- */
    printf("pushes=%u\n",   (unsigned)mc_pushes);
    printf("rx=%u\n",       (unsigned)mc_rx);
    printf("timeouts=%u\n", (unsigned)mc_timeouts);
    printf("writes=%u\n",   (unsigned)mcwrites_written);
    printf("readback=%u\n", (unsigned)mcwrites_readback);
    printf("ring=%u\n",     (unsigned)mcring_written);
    printf("ring_frozen=%u\n", (unsigned)mcring_frozen);

    /* --- istogrammi --- */
    mcget_hist("h1_scan",   h_ack_phase[PH_SCAN],   MC_BUCKETS);
    mcget_hist("h1_stage3", h_ack_phase[PH_STAGE3], MC_BUCKETS);
    mcget_hist("h1_post",   h_ack_phase[PH_POST],   MC_BUCKETS);
    mcget_hist("h2_scan",   h_con_resp[PH_SCAN],    MC_BUCKETS);
    mcget_hist("h2_stage3", h_con_resp[PH_STAGE3],  MC_BUCKETS);
    mcget_hist("h2_post",   h_con_resp[PH_POST],    MC_BUCKETS);
    mcget_hist("h3_txlevel", h_tx_level, 9u);

#if PS1_MMCE_TRACE
    printf("gid_age_us=%u\n",
           (unsigned)(gid_ts ? (timer_hw->timerawl - gid_ts) : 0u));
    printf("gid_len=%u\n", (unsigned)gid_len_expected);
    printf("gid_got=%u\n", (unsigned)gid_n);
    printf("gid_hex=");
    for (unsigned i = 0; i < gid_n; i++) printf("%02X", (unsigned)gid_buf[i]);
    printf("\n");
    printf("mmce_events=%u\n", (unsigned)mmcelog_written);
#endif
    printf("end\n");
}

/* Azzera tutto cio' che si accumula, cosi' la prova successiva parte pulita. */
void ps1_mc_counters_clear(void) {
    for (uint f = 0; f < 3; f++)
        for (uint i = 0; i < MC_BUCKETS; i++) {
            h_ack_phase[f][i] = 0;
            h_con_resp[f][i] = 0;
        }
    for (uint i = 0; i < 9; i++) h_tx_level[i] = 0;
    mc_pushes = mc_rx = mc_timeouts = 0;
    mc_frames = 0;
    mc_last_page = mc_max_page = 0;
    mc_last_offset = 0; mc_last_cmd = 0; mc_exit_site = 0;
    mc_phase = PH_SCAN;
    mc_last_rx_us = mc_last_push_us = 0;
    mcring_written = 0;
    mcring_cycle_start = 0;
    mcring_skip = 0; mcring_frozen = 0;
    printf("cleared\n");
}

/* Lo stato EFFETTIVO, non quello che si crede di aver impostato. Da leggere
   prima di ogni prova: e' il controllo che nella build precedente mancava. */
void ps1_mc_tune_print(void) {
    /* Modalita' e stato USB li stampa "status", che c'e' in tutte le build.
       Qui resta solo se il profilo PIO e' stato applicato, perche' senza quello
       tutti i numeri sotto sono zero e sembrerebbero un guasto: in PS2, o prima
       che l'emulazione parta, init_pio() non viene mai chiamata. */
    if (!timing_applied) {
        printf("profilo PIO: NON ANCORA APPLICATO"
               " (vedi 'status': serve PS1 con l'emulazione avviata)\n");
    }

    /* La larghezza EFFETTIVA, non quella chiesta: il valore che conta e' quello
       che il PIO sta eseguendo, e a divisore diverso lo stesso numero di cicli
       vale una durata diversa. */
    printf("ackwidth=%u cicli (%u ns, minimo di specifica %u000 ns)%s\n",
           (unsigned)ack_width_cycles, (unsigned)ack_width_ns, PS1_ACK_MIN_US,
           (!timing_applied) ? ""
           : (ack_width_ns < PS1_ACK_MIN_US * 1000u) ? "  <-- FUORI SPECIFICA" : "");
    printf("ackwidth_force=%s ackwait=%s\n",
           ack_cycles ? "si" : "no (calcolata dal divisore)",
           ack_wait_on ? "on" : "OFF <-- ACK prima della fine del byte");
    /* Il ritardo effettivo che verrebbe applicato adesso, non il knob: fra i due
       ci sono il profilo e delayall, ed e' la differenza che ha gia' falsato
       una serie di misure. */
    printf("ackdelay=%u us (ora attivo: %s)  delayall=%s\n",
           (unsigned)ack_delay_us,
           ((delay_all || timing_boot) && ack_delay_us) ? "si" : "NO",
           delay_all ? "on" : "off");
    printf("datpio=%u rxpio=%u ctrlpio=%u boot=%u\n",
           (unsigned)(div_dat  ? div_dat  : (timing_boot ? PS1_CLKDIV_BOOT : PS1_CLKDIV)),
           (unsigned)(div_cmd  ? div_cmd  : (timing_boot ? PS1_CLKDIV_BOOT : PS1_CLKDIV)),
           (unsigned)(div_ctrl ? div_ctrl : (timing_boot ? PS1_CLKDIV_BOOT : PS1_CLKDIV)),
           timing_boot ? 1u : 0u);
    printf("mmcenodelay=%s\n", mmce_nodelay ? "on" : "OFF");
    printf("boottimeout=%s\n",
           (boottimeout_force == 255u) ? "da ini" : "forzato");
    printf("profile=%s\n", (profile_force == 1u) ? "FORZATO standard"
                           : (profile_force == 2u) ? "FORZATO boot"
                           : "automatico");
    /* Il livello di protezione della boot card: e' fisso a compile time, ma va
       stampato lo stesso, perche' e' quello che decide se un tentativo fallito
       sporca le prove successive. */
    printf("readonly=%s (sd=%s ram=%s)\n",
#if   PS1_BOOTCARD_SD_READONLY && PS1_BOOTCARD_RAM_READONLY
           "sd+ram",
#elif PS1_BOOTCARD_SD_READONLY
           "solo-sd",
#else
           "NESSUNO",
#endif
           PS1_BOOTCARD_SD_READONLY  ? "on" : "OFF",
           PS1_BOOTCARD_RAM_READONLY ? "on" : "OFF");
}
#endif /* PS1_MC_TUNING */

#if PS1_MMCE_TRACE
/* Su core0. Stampa e azzera: si legge DOPO la prova, non durante. */
void ps1_mc_mmcelog_print(void) {
    const uint32_t total = mmcelog_written;
    const uint32_t shown = (total > MMCELOG_SLOTS) ? MMCELOG_SLOTS : total;
    /* Nell'ordine dell'enum in ps1_cardman.h: NAMED, BOOT, GAMEID, NORMAL.
       Scriverli a intuito e' costato una conclusione sbagliata il 02/09:
       GAMEID e NORMAL risultavano invertiti. */
    static const char *st[] = { "NAMED", "BOOT", "GAMEID", "NORMAL" };

    printf("mmcelog: %u eventi%s\n", (unsigned)total,
           (total > MMCELOG_SLOTS) ? " (i piu' vecchi persi)" : "");

    for (uint32_t k = 0; k < shown; k++) {
        const mmcelog_entry_t *e = &mmcelog[(total - shown + k) % MMCELOG_SLOTS];
        const char *name;
        switch (e->cmd) {
            case 0x20: name = "ping (ricevuto)";     break;
            case 0x21: name = "game id (ricevuto)";  break;
            case 0x22: name = "canale prec";   break;
            case 0x23: name = "canale succ";   break;
            case 0x24: name = "card prec";     break;
            case 0x25: name = "card succ";     break;
            case 0x26: name = "switch default (non gestito)"; break;
            case 0x27: name = "reset";         break;
            case MMCELOG_CMD_RUN:    name = "ESEGUITO";            break;
            case MMCELOG_SWAP_DONE:  name = "== card cambiata ==";  break;
            case MMCELOG_PING_DONE:  name = "  -> ping completato"; break;
            case MMCELOG_GID_DONE:   name = "  -> game id letto per intero"; break;
            case MMCELOG_GID_VALID:  name = "  -> id valido: card del gioco"; break;
            case MMCELOG_GID_SWITCH: name = "  -> id non valido: card predefinita"; break;
            case MMCELOG_GID_NONE:   name = "  -> NESSUN RAMO: non cambia niente"; break;
            default:   name = "?"; break;
        }
        const unsigned s_idx = (e->state < (sizeof(st)/sizeof(st[0]))) ? e->state : 0u;
        printf("  %10u us  0x%02X %-38s card=%s", (unsigned)e->t_us,
               e->cmd, name, st[s_idx]);
        if (e->cmd == MMCELOG_GID_DONE)
            printf("  len=%u primo=0x%02X", (unsigned)e->a, (unsigned)e->b);
        else if (e->cmd == MMCELOG_CMD_RUN) {
            static const char *cn[] = { "?", "game id", "canale succ", "canale prec",
                                        "card succ", "card prec", "switch bootcard",
                                        "switch default", "set card", "set canale",
                                        "reset" };
            printf("  %s", (e->a < (sizeof(cn)/sizeof(cn[0]))) ? cn[e->a] : "?");
        } else if (e->cmd == MMCELOG_SWAP_DONE)
            printf("  idx=%u", (unsigned)e->b);
        printf("\n");
    }

    /* Vale anche - soprattutto - se la transazione si e' interrotta. */
    if (gid_len_expected || gid_n) {
        printf("  ultimo game id: annunciati %u byte, ricevuti %u", 
               (unsigned)gid_len_expected, (unsigned)gid_n);
        if (gid_n) {
            printf("  testo=\"");
            for (unsigned i = 0; i < gid_n; i++) {
                const uint8_t ch = gid_buf[i];
                printf("%c", ((ch >= 0x20) && (ch < 0x7F)) ? ch : '.');
            }
            printf("\"  hex=");
            for (unsigned i = 0; i < gid_n; i++)
                printf("%02X ", (unsigned)gid_buf[i]);
        }
        printf("\n");
    }

    mmcelog_written = 0;
}
#endif /* PS1_MMCE_TRACE */

void ps1_memory_card_main(void) {
    multicore_lockout_victim_init();

    init_pio();

    us_startup = time_us_64();
    DPRINTF("Secondary core!\n");

    my_gpio_set_irq_enabled_with_callback(PIN_PSX_SEL, GPIO_IRQ_EDGE_RISE, 1, card_deselected);
    my_gpio_set_irq_enabled_with_callback(PIN_PSX_SEL, GPIO_IRQ_EDGE_FALL, 1, card_deselected);

    /* Slew rate e drive del DAT li imposta ps1_mc_apply_timing_profile(): sono
       diversi fra profilo standard e profilo boot. Metterli anche qui li
       riscriverebbe dopo che core0 ha gia' scelto il profilo. */

    mc_main();
}

static int memcard_running;

void ps1_memory_card_exit(void) {
    if (!memcard_running)
        return;

    mc_exit_request = 1;
    while (!mc_exit_response)
    {}
    mc_exit_request = mc_exit_response = 0;
    memcard_running = 0;
}

void ps1_memory_card_enter(void) {
    if (memcard_running)
        return;

    /* Il profilo di timing dipende dalla card che stiamo per servire, quindi si
       decide qui: e' il punto per cui passano sia l'avvio sia ogni cambio card,
       e core1 e' fermo in attesa di mc_enter_request. */
    while (!pio_ready)
        {}
    const bool boot = ps1_mc_wants_boot_timing();
    if (!timing_applied || (boot != timing_boot))
        ps1_mc_apply_timing_profile(boot);

    mc_enter_request = 1;
    while (!mc_enter_response)
    {}
    mc_enter_request = mc_enter_response = 0;
    memcard_running = 1;
}

void ps1_memory_card_unload(void) {
    /* Un solo pio_remove_program per cmd_reader, non due.
     *
     * Dall'unificazione cntrl_reader gira sulla STESSA copia caricata
     * (cntrl_reader.offset = cmd_reader.offset in init_pio), quindi rimuoverla
     * due volte fa scattare l'assert dell'SDK sullo spazio gia' liberato. Le
     * due state machine restano distinte e vanno disimpegnate separatamente.
     * Prima dell'unificazione gli offset erano diversi e il doppio remove era
     * corretto. Il percorso e' ps1_deinit(), cioe' il passaggio a PS2. */
    pio_remove_program(pio0, &cmd_reader_program, cmd_reader.offset);
    pio_sm_unclaim(pio0, cmd_reader.sm);
    pio_sm_unclaim(pio0, cntrl_reader.sm);
    pio_remove_program(pio0, &dat_writer_program, dat_writer.offset);
    pio_remove_program(pio0, &ack_pulser_program, ack_pulser.offset);
    pio_sm_unclaim(pio0, ack_pulser.sm);
    pio_sm_unclaim(pio0, dat_writer.sm);
    pio_ready = false;
    timing_applied = false;
    log(LOG_TRACE, "Unclaimed %u, %u, %u!\n", cmd_reader.sm, dat_writer.sm, cntrl_reader.sm);

}
