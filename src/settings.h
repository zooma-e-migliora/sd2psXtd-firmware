#pragma once

#include <stdbool.h>
#include <stdint.h>

void settings_load_sd(void);
void settings_init(void);

int settings_get_ps1_card(void);
int settings_get_ps1_channel(void);
int settings_get_ps1_boot_channel(void);
void settings_set_ps1_card(int x);
void settings_set_ps1_channel(int x);
void settings_set_ps1_boot_channel(int x);

/* Secondi da restare sulla BootCard prima di tornare da soli alla card
   predefinita.
     SETTINGS_BOOTCARD_TIMEOUT_NEVER  non si esce mai da soli (default, -1
                                      nell'ini): ci pensa solo il payload
     0                                si esce subito
     N                                si esce dopo N secondi */
#define SETTINGS_BOOTCARD_TIMEOUT_NEVER 255

uint8_t settings_get_ps1_bootcard_timeout(void);
void settings_set_ps1_bootcard_timeout(uint8_t seconds);

/* Per quanto tenere il verde fisso a indicare la boot mode.
     SETTINGS_LED_BOOTCARD_FOLLOW  quanto dura la boot mode stessa (default,
                                   -1 nell'ini)
     0                             mai: la modalita' non viene segnalata dal
                                   colore di fondo (i lampi verdi del cambio
                                   card restano)
     N                             N secondi
   Il getter limita il valore a BootCardTimeout, tranne quando quello e'
   SETTINGS_BOOTCARD_TIMEOUT_NEVER e quindi non fa da tetto. */
#define SETTINGS_LED_BOOTCARD_FOLLOW 255

uint8_t settings_get_ps1_led_bootcard_timeout(void);
void settings_set_ps1_led_bootcard_timeout(uint8_t seconds);

/* Se false (il default) card e canale correnti non vengono ne' salvati ne'
   ripristinati: si riparte sempre da uno stato pulito. */
bool settings_get_ps1_remember_card(void);
void settings_set_ps1_remember_card(bool remember);

/* Canali per card, valido per tutte. Il CardX.ini di una singola card puo'
   sovrascriverlo. Default 3, 255 = tutti quelli possibili (-1 nell'ini). */
uint8_t settings_get_ps1_maxchannels(void);
void settings_set_ps1_maxchannels(uint8_t maxchannels);
bool settings_get_ps1_controllercombo(void);
void settings_set_ps1_controllercombo(bool controllercombo);

/* Timing PIO pre-1.2.1 mentre e' montata la boot card: e' l'unico su cui parte
   il FreePSXBoot "superfast". Acceso di default; sulle card normali non ha
   effetto in nessun caso.
   Il nome dice "abilita", ma il clock raddoppiato lo decide il payload da solo:
   quello che si abilita qui e' il nostro stare al passo. Spento, il superfast
   non e' disattivato, semplicemente fallisce.

   Agisce in UN SOLO punto - ps1_mc_wants_boot_timing() - e da li' governa
   quattro cose, non una di piu':

     1. il divisore PIO di tutte e quattro le SM:  32  invece di 96;
     2. la DURATA dell'impulso di ACK: 1066 ns invece di 3200. La larghezza e'
        la stessa costante di 8 cicli in entrambi i casi - cambia la durata
        perche' si conta in cicli PIO e quindi segue il divisore. E' proprio il
        meccanismo che ha causato l'issue 59;
     3. il ritardo di testa dell'ACK: 0 cicli invece di 2 (800 ns);
     4. i 24 us sulle letture singole del BIOS.

   NON governa niente altro. Spegnerlo NON rende la boot card uguale a una card
   normale: restano legati allo stato BOOT, e indipendenti da qui, la protezione
   da scrittura, la cartella BootCard sulla SD, il LED verde, BootCardTimeout,
   la navigazione col dpad, l'etichetta a schermo e l'uscita pilotata dal Game
   ID. Spento, la boot card e' una card normale sul bus e resta una boot card
   per tutto il resto. */
bool settings_get_ps1_fast_mode(void);
void settings_set_ps1_fast_mode(bool fast_mode);

/* Quante memcard si possono scorrere. Default 10; 0 = nessun limite, e allora
   card_idx sale fino a UINT16_MAX (si scrive -1 nell'ini). */
uint8_t settings_get_ps1_maxcardidx(void);
void settings_set_ps1_maxcardidx(uint8_t x);

int settings_get_ps2_card(void);
int settings_get_ps2_channel(void);
int settings_get_ps2_boot_channel(void);
uint8_t settings_get_ps2_cardsize(void);
int settings_get_ps2_variant(void);
void settings_set_ps2_card(int x);
void settings_set_ps2_channel(int x);
void settings_set_ps2_boot_channel(int x);
void settings_set_ps2_cardsize(uint8_t size);
void settings_set_ps2_variant(int x);
uint8_t settings_get_ps2_maxcardidx(void);
void settings_set_ps2_maxcardidx(uint8_t x);

enum {
    MODE_PS1 = 0,
    MODE_PS2 = 1,
    MODE_TEMP_PS1 = 2
};

enum {
    PS2_VARIANT_RETAIL  = 0, // Retail
    PS2_VARIANT_COH     = 1, // Arcade. Port 1
    PS2_VARIANT_PROTO   = 2, // Prototype
    PS2_VARIANT_SC2 = 3,    // Arcade. port 2
};

int settings_get_mode(bool current);
void settings_set_mode(int mode);

/* La modalita' EFFETTIVAMENTE in esecuzione (definita in main.c, non qui: sta
   accanto al getter delle impostazioni perche' e' li' che uno la cerca).
 *
 * Non coincide sempre con settings_get_mode(): settings_set_mode() cambia solo
 * l'impostazione, e il passaggio avviene quando il lato in esecuzione accetta di
 * uscire - ps2_task() lo fa solo se ps2_cardman_is_idle(). Se le due divergono,
 * un cambio di modalita' e' rimasto appeso, ed e' quello che "status" mostra. */
int main_get_running_mode(void);
bool settings_get_ps1_autoboot(void);
void settings_set_ps1_autoboot(bool autoboot);
bool settings_get_ps1_game_id(void);
void settings_set_ps1_game_id(bool enabled);
bool settings_get_ps2_autoboot(void);
void settings_set_ps2_autoboot(bool autoboot);
bool settings_get_ps2_game_id(void);
void settings_set_ps2_game_id(bool enabled);

#define IDX_MIN 1
#define IDX_BOOT 0
#define CHAN_MIN 1

uint8_t settings_get_display_timeout(void);
uint8_t settings_get_display_contrast(void);
uint8_t settings_get_display_vcomh(void);
bool    settings_get_display_flipped(void);
void settings_set_display_timeout(uint8_t display_timeout);
void settings_set_display_contrast(uint8_t display_contrast);
void settings_set_display_vcomh(uint8_t display_vcomh);
void settings_set_display_flipped(bool flipped);
