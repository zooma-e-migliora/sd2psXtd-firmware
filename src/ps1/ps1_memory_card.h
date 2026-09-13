#pragma once


#include <stdbool.h>
#include <stdint.h>

#define MMCE_PS1_GAME_ID     (1U)
#define MMCE_PS1_NXT_CH      (2U)
#define MMCE_PS1_PRV_CH      (3U)
#define MMCE_PS1_NXT_CARD    (4U)
#define MMCE_PS1_PRV_CARD    (5U)
#define MMCE_PS1_SWITCH_BOOTCARD (6U)
#define MMCE_PS1_SWITCH_DEFAULT  (7U)
#define MMCE_PS1_SET_CARD    (8U)
#define MMCE_PS1_SET_CHANNEL (9U)
#define MMCE_PS1_RESET       (10U)


void ps1_memory_card_main(void);
void ps1_memory_card_enter(void);
void ps1_memory_card_exit(void);
void ps1_memory_card_unload(void);

/* --- Boot card in sola lettura: DUE livelli, indipendenti ------------------
 *
 * Sono due protezioni distinte, con conseguenze diverse, e vanno tenute
 * separate perche' si puo' volere la prima senza la seconda:
 *
 *   PS1_BOOTCARD_SD_READONLY   il file .mcd sulla microSD non viene mai
 *                              riscritto. Spegnendolo, un boot fallito rovina
 *                              l'immagine in modo PERMANENTE.
 *
 *   PS1_BOOTCARD_RAM_READONLY  non si scrive nemmeno nella copia in RAM, che e'
 *                              quella da cui la console legge davvero. Senza,
 *                              il file resta intatto ma l'immagine in memoria
 *                              si degrada fino al prossimo cambio card: le
 *                              prove successive girano su dati diversi e non
 *                              sono confrontabili.
 *
 * La console scrive sulla boot card sia aprendo il visualizzatore di memory
 * card sia a ogni tentativo di boot - 128 byte, misurati - e sfilare la card non
 * riavvia il firmware, quindi senza il livello RAM un'immagine rovinata resta in
 * giro per tutti i boot successivi.
 *
 * Con entrambi accesi, quello che la console legge e' sempre identico al file.
 *
 * Le combinazioni utili sono TRE, e "mcstat" stampa quella attiva per nome, cosi'
 * non va dedotta da due flag separati:
 *
 *   SD   RAM   livello    cosa succede a un tentativo di boot fallito
 *   ---  ---   --------   ---------------------------------------------------
 *    0    0    nessuno    rovina il file: DANNO PERMANENTE
 *    1    0    solo-sd    il file resta intatto, la copia in RAM si degrada
 *                         fino al prossimo cambio card: prove non ripetibili
 *    1    1    sd+ram     non cambia niente da nessuna parte   <-- default
 *    0    1    sd+ram     promossa: vedi sotto, e' esattamente il caso (1,1)
 *
 *
 * Per la PS1 la scrittura riesce lo stesso: il byte di conferma lo manda
 * mc_cmd_write() senza consultare questo strato. E' il comportamento di una card
 * protetta in scrittura. Diverso dal tentativo descritto in GUIDA sezione 8, che
 * rifiutava la scrittura con 0xFF e cambiava card: li' la boot mode diventava
 * inusabile.
 *
 * Valgono per ogni canale: guardano lo stato di cardman, non il nome del file. */
#ifndef PS1_BOOTCARD_SD_READONLY
#define PS1_BOOTCARD_SD_READONLY 1
#endif
#ifndef PS1_BOOTCARD_RAM_READONLY
#define PS1_BOOTCARD_RAM_READONLY 1
#endif

/* La sola lettura della RAM implica quella sulla microSD, e non e' pignoleria:
   senza questa riga (sd=0, ram=1) sarebbe soltanto SIMILE a (1,1), non uguale.
   Il contenuto scritto sarebbe identico - in RAM non e' cambiato niente - ma
   write_mc() marcherebbe lo stesso la pagina e ps1_dirty_task() la riscriverebbe
   davvero: I/O sulla microSD nel mezzo di un tentativo di boot, usura della
   scheda, e un errore di scrittura accenderebbe il LED d'errore per una pagina
   mai cambiata. Promuovendola qui, le due combinazioni sono lo stesso codice. */
#if PS1_BOOTCARD_RAM_READONLY && !PS1_BOOTCARD_SD_READONLY
#undef  PS1_BOOTCARD_SD_READONLY
#define PS1_BOOTCARD_SD_READONLY 1
#endif

/* true se la scrittura va scartata a questo livello. */
/* Istante (timer_hw->timerawl, 32 bit, avvolge ogni ~71 minuti) dell'ultimo
   comando ricevuto dalla memory card. Serve al timeout della boot card, che
   misura l'INATTIVITA': durante l'exploit i comandi si susseguono senza sosta,
   quindi non puo' scadere mentre la console sta leggendo. */
uint32_t ps1_mc_last_activity_us(void);

bool ps1_mc_bootcard_sd_write_denied(void);
bool ps1_mc_bootcard_ram_write_denied(void);

/* --- Taratura da seriale ---------------------------------------------------
 *
 * Con PS1_MC_TUNING spento sparisce tutto: i parametri tornano costanti, le due
 * funzioni non esistono e nel binario non resta nessuna stringa dei comandi.
 * Acceso serve per misurare senza ricompilare a ogni valore.
 *
 * "mcstat" stampa lo stato EFFETTIVO di tutti i parametri. Va letto prima di
 * ogni prova: nella build precedente mancava, e tre prove sono state annotate
 * con un ritardo dell'ACK diverso da quello che avevano davvero. */
#ifndef PS1_MC_TUNING
#define PS1_MC_TUNING 0   /* build di taratura: mettere a 1 per misurare */
#endif
#if PS1_MC_TUNING
void ps1_mc_tune_set(const char *what, uint32_t value);
void ps1_mc_ring_print(void);
void ps1_mc_tune_print(void);
/* Scritture della console sulla boot card e riletture di pagine gia' scritte.
   Il contatore delle riletture dice se la sola lettura falsa le prove o no. */
void ps1_mc_writes_print(void);
/* Stato completo in chiave=valore per mctest.py, e azzeramento dei contatori.
   mcstat/status restano per la lettura umana. */
void ps1_mc_get_print(void);
void ps1_mc_counters_clear(void);
/* Override di BootCardTimeout senza passare dall'ini. 255 = usa l'ini. */
uint8_t ps1_mc_tune_boottimeout(void);
#endif

/* --- Traccia dei comandi MMCE ---------------------------------------------
 *
 * Interruttore SEPARATO da PS1_MC_TUNING, di proposito: sono due strumenti per
 * due domande diverse - la taratura serve ai tempi del bus, questa a capire
 * quali comandi manda la console e quando. Si accendono in modo indipendente, o
 * entrambi.
 *
 * Registra da core1 nel percorso critico senza formattare niente (un indice e
 * cinque byte); stampa da core0 con il comando seriale "mmcelog". */
#ifndef PS1_MMCE_TRACE
#define PS1_MMCE_TRACE 0
#endif
#if PS1_MMCE_TRACE
void ps1_mc_mmcelog_put(uint8_t cmd, uint8_t a, uint8_t b);
void ps1_mc_mmcelog_print(void);
#define MMCELOG_GID_VALID   0xF0u
#define MMCELOG_GID_SWITCH  0xF1u
#define MMCELOG_GID_NONE    0xF2u
/* "ricevuto" e "portato a termine" sono due cose diverse: i gestori escono con
   un return se la console deseleziona la card a meta'. Senza questi due codici
   un comando abortito e' indistinguibile da uno completato. */
#define MMCELOG_GID_DONE    0xF3u
#define MMCELOG_PING_DONE   0xF4u
/* Comando ESEGUITO da ps1_mmce_task (a = codice MMCE_PS1_*), e cambio card
   portato a termine (a = stato risultante, b = indice card). Servono perche' la
   combo del dispositivo non passa dal dispatcher del filo: un comando eseguito
   senza il corrispondente evento "ricevuto" viene da li'. */
#define MMCELOG_CMD_RUN     0xE0u
#define MMCELOG_SWAP_DONE   0xE1u
#endif

uint8_t ps1_memory_card_get_ode_command(void);
void ps1_memory_card_reset_ode_command(void);
const char* ps1_memory_card_get_game_id(void);
/* true finche' la modalita' di cambio card (SELECT+L1+L2) e' attiva. Scritta da
   core1, letta da core0 per la segnalazione sui LED. */
bool ps1_memory_card_combo_active(void);

