#include "ps1_cardman.h"

#include <ctype.h>
#include <ps1/ps1_memory_card.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "card_config.h"

#include "ps1_mc_data_interface.h"
#include "sd.h"
#include "debug.h"
#include "settings.h"
#if WITH_PSRAM
#include <psram/psram.h>
#endif
#include "ps1_empty_card.h"
#include "util.h"

#include "game_db/game_db.h"

#include "hardware/timer.h"

#if LOG_LEVEL_PS1_CM == 0
    #define log(x...)
#else
    #define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_PS1_CM, level, fmt, ##x)
#endif


#define CARD_SIZE (128 * 1024)
#define BLOCK_SIZE 128
static uint8_t flushbuf[BLOCK_SIZE];
static int fd = -1;

#define IDX_MIN 1
#define CHAN_MIN 1


static int card_idx;
static int card_chan;
static char folder_name[MAX_FOLDER_NAME_LENGTH];
static ps1_cardman_state_t cardman_state;
static bool needs_update;

/* Percorso della boot card per un dato canale. Un nome solo: il ripiego storico
   su BOOT/BootCard.mcd senza numero, da prima che la boot card avesse i canali,
   e' stato tolto.
   Sta qui in un punto solo perche' lo usano sia try_set_boot_card(), per sapere
   se la card esiste, sia ps1_cardman_open(), per aprirla. */
static void boot_card_path(char *path, size_t len, int chan) {
    snprintf(path, len, "MemoryCards/PS1/BOOT/BootCard-%d.mcd", chan);
}

/* Seleziona la boot card, se c'e'. Non guarda Autoboot: quell'impostazione dice
   soltanto se partirci all'accensione, non se la boot card sia utilizzabile.
   Serve per il passaggio esplicito da pad (START nella combo), che deve
   funzionare anche con Autoboot spento.

   Se il file non c'e' si restituisce false senza toccare niente: senza questo
   controllo ps1_cardman_open() creerebbe una boot card vuota e la servirebbe
   alla console come se fosse una card qualsiasi. Una boot card vuota non serve
   a niente, ha senso solo con dentro un payload FreePSXBoot, quindi va copiata
   a mano: il dispositivo non la crea.

   La verifica e' sicura: sd_init() gira prima di ps1_cardman_init(). */
static bool set_boot_card(void) {
    int chan = settings_get_ps1_remember_card() ? settings_get_ps1_boot_channel()
                                                : CHAN_MIN;
    char path[96];

    /* Limitato al MaxChannels di BOOT/BootCard.ini, che di suo vale 1: senza
       questo un canale ricordato piu' alto cercherebbe un file che non c'e' e
       la boot card sparirebbe del tutto. Lo stesso fa set_default_card(). */
    const uint8_t max_chan = card_config_get_max_channels("BOOT", "BootCard");
    if (chan > max_chan)
        chan = max_chan;

    boot_card_path(path, sizeof(path), chan);
    if (!sd_exists(path))
        return false;

    card_idx = PS1_CARD_IDX_SPECIAL;
    card_chan = chan;
    cardman_state = PS1_CM_STATE_BOOT;
    snprintf(folder_name, sizeof(folder_name), "BOOT");
    return true;
}

/* Come sopra, ma solo se l'autoboot e' attivo: usata all'accensione e nella
   rotazione delle card, dove la boot card compare solo se l'utente la vuole. */
static bool try_set_boot_card() {
    if (!settings_get_ps1_autoboot())
        return false;

    return set_boot_card();
}

static void set_default_card() {
    if (settings_get_ps1_remember_card()) {
        card_idx = settings_get_ps1_card();
        card_chan = settings_get_ps1_channel();
    } else {
        /* Stato pulito a ogni accensione: sempre la prima card, primo canale. */
        card_idx = IDX_MIN;
        card_chan = CHAN_MIN;
    }
    cardman_state = PS1_CM_STATE_NORMAL;
    snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
    uint8_t max_chan = card_config_get_max_channels(folder_name, folder_name);
    if (card_chan > max_chan)
        card_chan = max_chan;
}

static bool try_set_game_id_card() {
    if (!settings_get_ps1_game_id())
        return false;

    char parent_id[MAX_GAME_ID_LENGTH] = {};

    (void)game_db_get_current_parent(parent_id);

    if (!parent_id[0])
        return false;

    card_idx = PS1_CARD_IDX_SPECIAL;
    card_chan = CHAN_MIN;
    cardman_state = PS1_CM_STATE_GAMEID;
    memset(folder_name, 0, sizeof(folder_name));
    card_config_get_card_folder(parent_id, folder_name, sizeof(folder_name));

    if (!folder_name[0])
        snprintf(folder_name, sizeof(folder_name), "%s", parent_id);


    return true;
}

static bool try_set_next_named_card() {
    bool ret = false;
    if (cardman_state != PS1_CM_STATE_NAMED) {
        ret = try_set_named_card_folder("MemoryCards/PS1", 0, folder_name, sizeof(folder_name));
        if (ret)
            card_idx = 1;
    } else {
        ret = try_set_named_card_folder("MemoryCards/PS1", card_idx, folder_name, sizeof(folder_name));
        if (ret)
            card_idx++;
    }

    if (ret) {
        card_chan = CHAN_MIN;
        cardman_state = PS1_CM_STATE_NAMED;
    }

    return ret;
}

static bool try_set_prev_named_card() {
    bool ret = false;
    if (card_idx > 1) {
        ret = try_set_named_card_folder("MemoryCards/PS1", card_idx - 2, folder_name, sizeof(folder_name));
        if (ret) {
            card_idx--;
            card_chan = CHAN_MIN;
            cardman_state = PS1_CM_STATE_NAMED;
        }
    }
    return ret;
}

void ps1_cardman_init(void) {
    if (!try_set_game_id_card() && !try_set_boot_card())
        set_default_card();
}

int ps1_cardman_read_sector(int sector, void *buf128) {
    if (fd < 0)
        return -1;

    if (sd_seek(fd, sector * BLOCK_SIZE, SEEK_SET) != 0)
        return -2;

    if (sd_read(fd, buf128, BLOCK_SIZE) != BLOCK_SIZE)
        return -3;

    return 0;
}

int ps1_cardman_write_sector(int sector, void *buf512) {
    if (fd < 0)
        return -1;

    if (sd_seek(fd, sector * BLOCK_SIZE, SEEK_SET) != 0)
        return -1;

    if (sd_write(fd, buf512, BLOCK_SIZE) != BLOCK_SIZE)
        return -1;

    return 0;
}

void ps1_cardman_flush(void) {
    if (fd >= 0)
        sd_flush(fd);
}

/* Testo del promemoria dentro BOOT/. Da quando il firmware non crea piu' la
   boot card da solo, una cartella vuota non direbbe a nessuno cosa farci. */
static const char boot_readme[] =
    "BOOT - la card di avvio\r\n"
    "\r\n"
    "Qui va l'immagine di memory card che contiene il payload FreePSXBoot,\r\n"
    "quella con cui la console avvia UniROM invece del gioco.\r\n"
    "\r\n"
    "=== COSA COPIARE QUI ==================================================\r\n"
    "\r\n"
    "  BootCard-1.mcd    l'immagine dell'exploit. Il nome deve essere\r\n"
    "                    esattamente questo. Il firmware non la crea da\r\n"
    "                    solo: una boot card vuota non servirebbe a niente.\r\n"
    "  BootCard-2.mcd    una seconda immagine, se ti serve (vedi sotto)\r\n"
    "\r\n"
    "=== COSA CI TROVI GIA' ================================================\r\n"
    "\r\n"
    "  BootCard.ini      lo scrive il firmware, con MaxChannels=1: di boot\r\n"
    "                    card ne ha senso una sola. Vale solo per questa\r\n"
    "                    cartella e ha la precedenza sul MaxChannels globale\r\n"
    "                    di .sd2psx/settings.ini. Mettilo a 2 se vuoi tenere\r\n"
    "                    anche BootCard-2.mcd.\r\n"
    "  LEGGIMI.txt       questo file\r\n"
    "\r\n"
    "=== COME SI USA =======================================================\r\n"
    "\r\n"
    "Con Autoboot=ON, che e' il default, la scheda parte da qui. Il LED sta\r\n"
    "verde fisso per tutto il tempo in cui la boot card e' montata; quando il\r\n"
    "payload ha finito e passa a una card normale il verde si spegne, ed e'\r\n"
    "quello il segnale che l'exploit e' partito.\r\n"
    "\r\n"
    "Da pad: SELECT+L1+L2 entra in modalita' cambio card (LED magenta), poi\r\n"
    "START torna alla boot card e SU/GIU' ne esce.\r\n"
    "\r\n"
    "=== E' IN SOLA LETTURA ================================================\r\n"
    "\r\n"
    "Le scritture della console sulla boot card vengono respinte, compresa la\r\n"
    "copia tenuta in memoria. Non e' una limitazione ma una protezione: il\r\n"
    "BIOS e UniROM ci scrivono davvero, e senza questo l'immagine\r\n"
    "dell'exploit si rovinerebbe da sola dopo pochi avvii - poi smetterebbe\r\n"
    "di funzionare senza che si capisca perche'.\r\n"
    "\r\n"
    "=== SE QUI NON METTI NIENTE ===========================================\r\n"
    "\r\n"
    "La scheda parte normalmente sulla card 1 e questa cartella viene\r\n"
    "ignorata. La card normale e' un altro file, in un'altra cartella:\r\n"
    "\r\n"
    "  MemoryCards/PS1/Card1/Card1-1.mcd\r\n";

/* Di boot card ne ha senso una sola: il .ini della cartella ha la precedenza
   sul MaxChannels globale (card_config_get_max_channels), quindi bastano due
   righe per togliere i canali alla sola BOOT.
   L'ultima riga deve finire con a capo: ini_reader_sd() scarta la riga finale
   se non termina con \n. */
static const char bootcard_ini[] =
    "; Configurazione della boot card. Vale solo per BOOT/.\r\n"
    "[Settings]\r\n"
    "MaxChannels=1\r\n"
    "\r\n";

/* Guida rapida nella radice della SD: e' il primo posto che uno guarda
   collegandola al PC, e la board non ha display ne' altro modo di spiegarsi.
   Senza accenti di proposito, cosi' si legge uguale con qualunque codifica.
   Costa ~2 KB di flash, su circa 1 MB liberi. */
static const char root_readme[] =
    "sd2psXtd - guida rapida\r\n"
    "\r\n"
    "=== CARTELLE ==========================================================\r\n"
    "\r\n"
    "  /.sd2psx/settings.ini          impostazioni globali\r\n"
    "  /.sd2psx/LEGGIMI.txt           questa guida\r\n"
    "  /MemoryCards/PS1/\r\n"
    "      BOOT/BootCard-1.mcd        card di avvio FreePSXBoot (copiala tu)\r\n"
    "      BOOT/BootCard.ini          MaxChannels=1, lo scrive il firmware\r\n"
    "      Card1/Card1-1.mcd          card 1, canale 1\r\n"
    "      Card1/Card1-2.mcd          card 1, canale 2\r\n"
    "      Card1/Card1.ini            opzionale, lo scrivi tu\r\n"
    "      SLUS-00594/...             card del Game ID, se GameID=ON\r\n"
    "      Import/Import-1.mcd        card 'con nome', si raggiunge col pad\r\n"
    "\r\n"
    "Il file si chiama come la cartella piu' -<canale>: Card3/Card3-1.mcd.\r\n"
    "Un nome diverso non viene visto. I canali partono da 1.\r\n"
    "Unica eccezione: la cartella BOOT, dove i file sono BootCard-N.mcd.\r\n"
    "Qualunque cartella che non sia BOOT ne' CardN, con nome sotto i 16\r\n"
    "caratteri, entra nella lista navigabile col pad.\r\n"
    "\r\n"
    "Il .ini di una cartella contiene MaxChannels e vale solo per quella card,\r\n"
    "con la precedenza sul valore globale. Il firmware ne scrive uno solo,\r\n"
    "BOOT/BootCard.ini: gli altri li aggiungi tu se ti servono.\r\n"
    "\r\n"
    "=== LED ===============================================================\r\n"
    "\r\n"
    "  spento              riposo\r\n"
    "  rosso-verde-blu     accensione, sweep di ~0,4 s\r\n"
    "  bianco, 1 lampo     microSD montata, si entra in emulazione\r\n"
    "  ciano, lento        lettura dalla memory card\r\n"
    "  blu, veloce         scrittura sulla memory card\r\n"
    "  verde fisso         boot card montata; sparisce = exploit partito\r\n"
    "  magenta fisso       modalita' combo attiva\r\n"
    "  giallo, N lampi     cambio card o canale, N = il numero\r\n"
    "  bianco fisso        modalita' USB MSC, collegata al PC\r\n"
    "  rosso               errore; il ritmo dice quale\r\n"
    "\r\n"
    "L'attivita' della card lampeggia sopra i colori fissi: in boot card una\r\n"
    "lettura si vede come lampi ciano su fondo verde.\r\n"
    "\r\n"
    "=== COMBO SUL PAD =====================================================\r\n"
    "\r\n"
    "Entri con SELECT + L1 + L2. Resti dentro finche' ne tieni premuto almeno\r\n"
    "uno, esci quando li rilasci tutti e tre. Il LED e' magenta fisso.\r\n"
    "\r\n"
    "  Su          card successiva\r\n"
    "  Giu'        card precedente\r\n"
    "  Destra      canale successivo\r\n"
    "  Sinistra    canale precedente\r\n"
    "  START       passa alla BootCard, se esiste\r\n"
    "\r\n"
    "Il comando parte al rilascio del tasto, quindi puoi incatenare piu'\r\n"
    "cambi di seguito senza uscire dalla modalita'.\r\n"
    "Servono EnableControllerCombo=ON. La BootCard non e' nella lista di\r\n"
    "Su/Giu': ci si entra solo con START o con Autoboot, ma se ne esce con\r\n"
    "Su/Giu'.\r\n"
    "\r\n"
    "=== OPZIONI (.sd2psx/settings.ini) ====================================\r\n"
    "\r\n"
    "Sono i valori di default, nell'ordine in cui il firmware li scrive.\r\n"
    "\r\n"
    "  [General]\r\n"
    "  Mode=PS1                  evita l'avvio in modalita' PS2\r\n"
    "  [PS1]\r\n"
    "  Autoboot=ON               parte dalla BootCard se c'e'\r\n"
    "  GameID=ON                 una card separata per gioco riconosciuto\r\n"
    "  EnableControllerCombo=ON  indispensabile: senza, il pad non comanda\r\n"
    "  RememberLastCard=0        0 = riparte sempre dallo stesso punto\r\n"
    "  FastMode_Enable=1         timing per il FreePSXBoot 'superfast'\r\n"
    "  MaxCardIdx=10             quante memcard puoi scorrere\r\n"
    "  MaxChannels=3             quanti canali ha ogni card\r\n"
    "  BootCardTimeout=-1        -1 = nessuna uscita a tempo dalla BootCard\r\n"
    "  LedBootCardTimeout=-1     -1 = verde per tutta la durata della boot\r\n"
    "\r\n"
    "Su MaxCardIdx e MaxChannels -1 vuol dire 'nessun limite': per le memcard\r\n"
    "si arriva a Card65535, per i canali al massimo possibile, 255. Vale da 1\r\n"
    "a 255; qualunque altra cosa viene corretta dal firmware, che riscrive il\r\n"
    "file con il valore che ha applicato davvero. Se hai scritto una\r\n"
    "sciocchezza, dopo un riavvio la vedi corretta qui.\r\n"
    "Sui due timeout invece -1 e 0 sono cose diverse: 0 vuol dire esci subito\r\n"
    "dalla BootCard e LED spento.\r\n"
    "\r\n"
    "Nel file c'e' anche una sezione [PS2], che su questa scheda non serve.\r\n"
    "Le modifiche hanno effetto al riavvio successivo.\r\n"
    "Lascia una riga vuota in fondo al file: il parser la richiede.\r\n"
    "\r\n"
    "Cancella questo file per tornare ai valori di fabbrica: il firmware lo\r\n"
    "ricrea al riavvio successivo con i default qui sopra, dimenticando\r\n"
    "qualunque impostazione avessi cambiato.\r\n";

/* Albero minimo e configurazione di default, creati al primo avvio su una
   microSD vuota. Chiamata da main() subito dopo sd_init(), quindi PRIMA del
   bivio fra passthrough USB ed emulazione: cosi' l'albero c'e' gia' al primo
   collegamento al PC, che e' il momento in cui uno va a guardare cosa c'e'
   sulla scheda.

   A differenza di ensuredirs() qui un fallimento non e' fatale: e' una
   comodita', e se MemoryCards/ non si puo' creare l'errore vero arrivera'
   comunque da ensuredirs() al primo uso della card. */
/* Scritti solo se mancano, cosi' chi ci annota qualcosa non se lo vede
   sovrascritto a ogni avvio. Cancellarli li fa rigenerare. */
static void write_file_if_missing(const char *path, const char *text, size_t len) {
    if (sd_exists(path))
        return;

    int rfd = sd_open(path, O_RDWR | O_CREAT | O_TRUNC);
    if (rfd < 0) {
        log(LOG_ERROR, "impossibile creare %s\n", path);
        return;
    }
    sd_write(rfd, (void *)text, len);
    sd_close(rfd);
}

void ps1_cardman_ensure_base_layout(void) {
    static const char *const dirs[] = {
        "MemoryCards",
        "MemoryCards/PS1",
        "MemoryCards/PS1/Card1",
        "MemoryCards/PS1/BOOT",
    };

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (sd_exists(dirs[i]))
            continue;

        sd_mkdir(dirs[i]);
        if (!sd_exists(dirs[i])) {
            log(LOG_ERROR, "impossibile creare %s\n", dirs[i]);
            return;     /* senza la cartella padre e' inutile insistere */
        }
        log(LOG_INFO, "creata %s\n", dirs[i]);
    }

    /* L'immagine Card1-1.mcd non viene pre-generata: nasce in
       ps1_cardman_open() al primo uso vero, dove i 128 KB da scrivere non
       fanno aspettare l'enumerazione USB. */

    write_file_if_missing("MemoryCards/PS1/BOOT/LEGGIMI.txt",
                          boot_readme, sizeof(boot_readme) - 1);
    write_file_if_missing("MemoryCards/PS1/BOOT/BootCard.ini",
                          bootcard_ini, sizeof(bootcard_ini) - 1);

    /* La guida generale sta accanto alle impostazioni che descrive, non nella
       radice: la radice della microSD e' dell'utente, e riempirla di file
       nostri e' un modo per farseli cancellare. La cartella la crea
       settings_write_sd(), ma qui non si puo' dare per scontato l'ordine. */
    if (!sd_exists("/.sd2psx/"))
        sd_mkdir("/.sd2psx/");
    write_file_if_missing("/.sd2psx/LEGGIMI.txt",
                          root_readme, sizeof(root_readme) - 1);
}

static void ensuredirs(void) {
    char cardpath[64];

    snprintf(cardpath, sizeof(cardpath), "MemoryCards/PS1/%s", folder_name);

    sd_mkdir("MemoryCards");
    sd_mkdir("MemoryCards/PS1");
    sd_mkdir(cardpath);

    if (!sd_exists("MemoryCards") || !sd_exists("MemoryCards/PS1") || !sd_exists(cardpath))
        fatal(ERR_CARDMAN, "error creating directories");
}

static void genblock(size_t pos, void *buf) {
    memset(buf, 0xFF, BLOCK_SIZE);

    if (pos < 0x2000)
        memcpy(buf, &ps1_empty_card[pos], BLOCK_SIZE);
}

void ps1_cardman_open(void) {
    char path[96];
    ensuredirs();
    needs_update = false;

    switch (cardman_state) {
        case PS1_CM_STATE_BOOT:
            boot_card_path(path, sizeof(path), card_chan);

            /* Rete di sicurezza: qui sotto un file mancante verrebbe creato
               vuoto, ed e' esattamente il comportamento che non vogliamo per la
               boot card. set_boot_card() ha gia' verificato che ci sia, quindi
               non dovrebbe succedere: se succede si ripiega sulla card normale
               invece di fabbricare una boot card fasulla. */
            if (!sd_exists(path)) {
                log(LOG_ERROR, "boot card sparita, ripiego sulla card normale\n");
                set_default_card();
                snprintf(path, sizeof(path), "MemoryCards/PS1/%s/%s-%d.mcd",
                         folder_name, folder_name, card_chan);
                ensuredirs();
                break;
            }

            if (settings_get_ps1_remember_card())
                settings_set_ps1_boot_channel(card_chan);
            break;
        case PS1_CM_STATE_NAMED:
        case PS1_CM_STATE_GAMEID:
            snprintf(path, sizeof(path), "MemoryCards/PS1/%s/%s-%d.mcd", folder_name, folder_name, card_chan);
            break;
        case PS1_CM_STATE_NORMAL:
            snprintf(path, sizeof(path), "MemoryCards/PS1/%s/%s-%d.mcd", folder_name, folder_name, card_chan);

            /* this is ok to do on every boot because it wouldn't update if the value is the same as currently stored */
            if (settings_get_ps1_remember_card()) {
                settings_set_ps1_card(card_idx);
                settings_set_ps1_channel(card_chan);
            }
            break;
    }

    log(LOG_INFO, "Switching to card path = %s\n", path);

    if (!sd_exists(path)) {
        fd = sd_open(path, O_RDWR | O_CREAT | O_TRUNC);

        if (fd < 0)
            fatal(ERR_CARDMAN, "cannot open for creating new card");

        log(LOG_INFO, "create new image at %s... ", path);
        uint64_t cardprog_start = time_us_64();

        for (size_t pos = 0; pos < CARD_SIZE; pos += BLOCK_SIZE) {
            genblock(pos, flushbuf);
#if WITH_PSRAM
            psram_write_dma(pos, flushbuf, BLOCK_SIZE, NULL);
#endif
            if (sd_write(fd, flushbuf, BLOCK_SIZE) != BLOCK_SIZE)
                fatal(ERR_CARDMAN, "cannot init memcard");
#if WITH_PSRAM
            psram_wait_for_dma();
#endif
        }
        sd_flush(fd);

        ps1_mc_data_interface_card_changed();

        uint64_t end = time_us_64();
        log(LOG_INFO, "OK!\n");


        log(LOG_INFO, "took = %.2f s; SD write speed = %.2f kB/s\n", (end - cardprog_start) / 1e6,
            1000000.0 * CARD_SIZE / (end - cardprog_start) / 1024);
    } else {
        fd = sd_open(path, O_RDWR);

        if (fd < 0)
            fatal(ERR_CARDMAN, "cannot open card");

        /* read 8 megs of card image */
        log(LOG_INFO, "reading card.... ");
        uint64_t cardprog_start = time_us_64();
#if WITH_PSRAM
        for (size_t pos = 0; pos < CARD_SIZE; pos += BLOCK_SIZE) {
            if (sd_read(fd, flushbuf, BLOCK_SIZE) != BLOCK_SIZE)
                fatal(ERR_CARDMAN, "cannot read memcard");

            psram_write_dma(pos, flushbuf, BLOCK_SIZE, NULL);
            psram_wait_for_dma();
        }
#endif
        ps1_mc_data_interface_card_changed();
        uint64_t end = time_us_64();
        log(LOG_INFO, "OK!\n");

        log(LOG_INFO, "took = %.2f s; SD read speed = %.2f kB/s\n", (end - cardprog_start) / 1e6,
            1000000.0 * CARD_SIZE / (end - cardprog_start) / 1024);
    }
}

bool ps1_cardman_needs_update(void) {
    return needs_update;
}

void ps1_cardman_close(void) {
    if (fd < 0)
        return;
    ps1_cardman_flush();
    sd_close(fd);
    fd = -1;
}

void ps1_cardman_next_channel(void) {
    uint8_t max_chan = card_config_get_max_channels(folder_name, (cardman_state == PS1_CM_STATE_BOOT) ? "BootCard" : folder_name);
    switch (cardman_state) {
        case PS1_CM_STATE_NAMED:
        case PS1_CM_STATE_BOOT:
        case PS1_CM_STATE_GAMEID:
        case PS1_CM_STATE_NORMAL:
            card_chan += 1;
            if (card_chan > max_chan)
#if WITH_GUI
                card_chan = CHAN_MIN;
#else
                card_chan = max_chan; //dont jump to CHAN_MIN. Otherwise without display, you cant see where you actually are.
#endif
            break;
    }

    needs_update = true;
}

void ps1_cardman_prev_channel(void) {
    uint8_t max_chan = card_config_get_max_channels(folder_name, (cardman_state == PS1_CM_STATE_BOOT) ? "BootCard" : folder_name);

    switch (cardman_state) {
        case PS1_CM_STATE_NAMED:
        case PS1_CM_STATE_BOOT:
        case PS1_CM_STATE_GAMEID:
        case PS1_CM_STATE_NORMAL:
            card_chan -= 1;
            if (card_chan < CHAN_MIN)
#if WITH_GUI
                card_chan = max_chan;
#else
                card_chan = CHAN_MIN; //dont jump to max_chan. Otherwise without display, you cant see where you actually are.
#endif
            break;
    }
    needs_update = true;
}

/* La boot card non compare nel giro su/giu' del dpad: e' una card speciale, con
   dentro un exploit, e finirci per sbaglio scorrendo le card non ha senso. Ci si
   entra solo di proposito, con START dentro la combo (ps1_cardman_switch_bootcard),
   oppure all'accensione con Autoboot (ps1_cardman_init). Uscirne col dpad si
   puo' invece eccome, ed e' il caso PS1_CM_STATE_BOOT qui sotto. */
void ps1_cardman_next_idx(void) {
    switch (cardman_state) {
        case PS1_CM_STATE_NAMED:
            if (!try_set_prev_named_card() && !try_set_game_id_card())
                set_default_card();
            break;
        case PS1_CM_STATE_BOOT:
            if (!try_set_game_id_card())
                set_default_card();
            break;
        case PS1_CM_STATE_GAMEID:
            set_default_card();
            break;
        case PS1_CM_STATE_NORMAL:
            card_idx += 1;
            card_chan = CHAN_MIN;
            if (card_idx > UINT16_MAX)
                card_idx = UINT16_MAX;
            uint8_t maxcards = settings_get_ps1_maxcardidx();
            if (maxcards != 0) //0 = unlimited cards UINT16_MAX
            {
                if (card_idx > maxcards)
                    card_idx = maxcards;
            }
            snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
            break;
    }
    needs_update = true;
}

void ps1_cardman_prev_idx(void) {
    switch (cardman_state) {
        case PS1_CM_STATE_NAMED:
        case PS1_CM_STATE_BOOT:
        case PS1_CM_STATE_GAMEID:
            if (!try_set_next_named_card())
                set_default_card();
            break;
        case PS1_CM_STATE_NORMAL:
            card_idx -= 1;
            card_chan = CHAN_MIN;
            if (card_idx <= PS1_CARD_IDX_SPECIAL) {
                if (!try_set_game_id_card() && !try_set_next_named_card())
                    set_default_card();
            } else {
                snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
            }
            break;
    }
    needs_update = true;
}

int ps1_cardman_get_idx(void) {
    return card_idx;
}

int ps1_cardman_get_channel(void) {
    return card_chan;
}

void ps1_cardman_set_game_id(const char* card_game_id) {
    if (!settings_get_ps1_game_id())
        return;

    char new_folder_name[MAX_FOLDER_NAME_LENGTH] = {};
    if (card_game_id[0]) {
        card_config_get_card_folder(card_game_id, new_folder_name, sizeof(new_folder_name));
        if (new_folder_name[0] == 0x00)
            snprintf(new_folder_name, sizeof(new_folder_name), "%s", card_game_id);
        if ((strcmp(new_folder_name, folder_name) != 0) || (PS1_CM_STATE_GAMEID != cardman_state)) {
            card_idx = PS1_CARD_IDX_SPECIAL;
            cardman_state = PS1_CM_STATE_GAMEID;
            card_chan = CHAN_MIN;
            memcpy(folder_name, new_folder_name, sizeof(folder_name));
            needs_update = true;
        }
    }
}
//TEMP
void ps1_cardman_switch_bootcard(void) {
    /* Richiesta esplicita dell'utente: basta che la boot card esista, non serve
       che Autoboot sia acceso. */
    if (set_boot_card())
        needs_update = true;
}

void ps1_cardman_switch_default(void) {
    if (PS1_CARD_IDX_SPECIAL == card_idx) {
        set_default_card();
        needs_update = true;
    }
}

void ps1_cardman_set_idx(int idx) {
    if (idx < PS1_CARD_IDX_SPECIAL)
        idx = PS1_CARD_IDX_SPECIAL;

    if (idx != card_idx) {
        card_idx = idx;
        card_chan = CHAN_MIN;
        cardman_state = PS1_CM_STATE_NORMAL;
        snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
        needs_update = true;
    }
}

void ps1_cardman_set_channel(int chn) {
    if (chn < CHAN_MIN)
        chn = CHAN_MIN;

    if (chn != card_chan) {
        card_chan = chn;
        needs_update = true;
    }
}


const char* ps1_cardman_get_folder_name(void) {
    return folder_name;
}

ps1_cardman_state_t ps1_cardman_get_state(void) {
    return cardman_state;
}
