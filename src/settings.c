#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "pico/multicore.h"
#include "sd.h"
#include "wear_leveling/wear_leveling.h"

#include "ini.h"

/* NOTE: for any change to the layout/size of this structure (that gets shipped to users),
   ensure to increase the version magic below -- this will trigger setting reset on next boot */
typedef struct {
    uint32_t version_magic;
    uint16_t ps1_card;
    uint16_t ps2_card;
    uint8_t ps1_channel;
    uint8_t ps2_channel;
    uint8_t ps1_boot_channel;
    uint8_t ps2_boot_channel;
    uint8_t ps1_flags; // TODO: single bit options: freepsxboot, pocketstation, freepsxboot slot
    // TODO: more ps1 settings: model for freepsxboot
    uint8_t ps2_flags; // TODO: single bit options: autoboot
    uint8_t sys_flags; // TODO: single bit options: whether ps1 or ps2 mode, etc
    uint8_t display_timeout; // display - auto off, in seconds, 0 - off
    uint8_t display_contrast; // display - contrast, 0-255
    uint8_t display_vcomh; // display - vcomh, valid values are 0x00, 0x20, 0x30 and 0x40
    uint8_t ps2_cardsize;
    // TODO: how do we store last used channel for cards that use autodetecting w/ gameid?
    uint8_t ps2_variant; // Variant for keys
    uint8_t ps1_maxcardidx;    //1-255, 0 = nessun limite (si scrive -1 nell'ini)
    uint8_t ps2_maxcardidx;    //1-255
    uint8_t ps1_bootcard_timeout; // secondi sulla BootCard prima di tornare alla card predefinita, 0 = mai
    uint8_t ps1_maxchannels;   // canali per card, default per tutte; il CardX.ini puo' sovrascriverlo. 255 = tutti (si scrive -1 nell'ini)
    uint8_t ps1_led_bootcard_timeout; // secondi di verde fisso in boot mode; FOLLOW = quanto dura la boot mode
} settings_t;

typedef struct {
    uint8_t ps2_flags;
    uint8_t ps1_flags;
    uint8_t sys_flags;
    uint8_t ps2_cardsize;
    uint8_t ps2_variant; // Variant for keys
    uint8_t ps1_maxcardidx;
    uint8_t ps2_maxcardidx;
    uint8_t ps1_bootcard_timeout;
    uint8_t ps1_maxchannels;
    uint8_t ps1_led_bootcard_timeout;
} serialized_settings_t;

#define SETTINGS_UPDATE_FIELD(field) settings_update_part(&settings.field, sizeof(settings.field))

/* Alzato per far entrare in vigore il nuovo default di Autoboot su schede che
   hanno gia' le impostazioni in flash, e perche' il byte di
   ps1_bootcard_timeout prima era padding e conterrebbe spazzatura: il bump
   azzera le impostazioni una volta.
   0xAACD000D: nuovo bit FAST_MODE (allora SUPERFAST_FIX), acceso di default.
   Senza il bump, sulle impostazioni gia' salvate il bit resterebbe a 0 e il
   default non entrerebbe mai in vigore.

   0xAACD000E: rimossa una chiave PS1 e il suo bit (0b0100000). Governava un
   ritardo prima dell'ACK, esposto come opzione: mascherava il difetto
   dell'issue 59 e ne creava un altro, il pad che si blocca nel visualizzatore
   di memory card. Il bump serve a due cose: liberare il bit da un valore che
   ora non significa piu' niente, e far riscrivere settings.ini senza la
   chiave, cosi' nessuno resta a credere di
   avere un'opzione che non c'e'. Vedi docs/ISSUE-59.md.

   Un cambio dei soli valori di default non richiede piu' un bump: da
   settings_load_sd() la mancanza di settings.ini vale come ritorno di
   fabbrica, quindi per farli entrare in vigore basta cancellare il file. */
#define SETTINGS_VERSION_MAGIC              (0xAACD000E)
#define SETTINGS_PS1_FLAGS_AUTOBOOT         (0b0000001)
#define SETTINGS_PS1_FLAGS_GAME_ID          (0b0000010)
#define SETTINGS_PS1_FLAGS_CTRL_COMBO       (0b0000100)
#define SETTINGS_PS1_FLAGS_REMEMBER_CARD    (0b0001000)
#define SETTINGS_PS1_FLAGS_FAST_MODE        (0b0010000)
/* 0b0100000 e' libero: apparteneva a un flag PS1 rimosso alla 0xAACD000E. */
#define SETTINGS_PS2_FLAGS_AUTOBOOT         (0b0000001)
#define SETTINGS_PS2_FLAGS_GAME_ID          (0b0000010)
#define SETTINGS_SYS_FLAGS_PS2_MODE         (0b0000001)
#define SETTINGS_SYS_FLAGS_FLIPPED_DISPLAY  (0b0000010)

/* 25 byte di campi piu' 3 di allineamento finale (il primo campo e' un uint32).
   Era 24 prima dei campi aggiunti per BootCardTimeout, MaxChannels e
   LedBootCardTimeout. L'area wear-levelled ne offre 512, quindi c'e' margine.
   Come dice la nota sopra, ogni cambio di layout va accompagnato da un aumento
   di SETTINGS_VERSION_MAGIC. */
_Static_assert(sizeof(settings_t) == 28, "unexpected padding in the settings structure");

static settings_t settings;
static serialized_settings_t serialized_settings;
static int tempmode;
static const char settings_path[] = "/.sd2psx/settings.ini";

/* Alzato dal parser quando ha dovuto correggere un valore: fa riscrivere l'ini
   con quello che vale davvero, che e' l'unico modo per cui uno si accorge di
   aver scritto una sciocchezza. Senza display non c'e' altro canale. */
static bool settings_ini_dirty;

static void settings_update_part(void *settings_ptr, uint32_t sz);
static void settings_serialize(void);
static void settings_serialize_internal(bool force);

/* Il valore letto e' da correggere se non e' identico a come lo riscriveremmo:
   copre -1, 0, 300, "pippo", gli zeri iniziali e gli spazi. Prende un int e non
   un uint8_t perche' deve poter ricevere il -1 di "nessun limite". */
static void mark_if_not_canonical(const char *value, int canonical) {
    char buf[8];

    snprintf(buf, sizeof(buf), "%d", canonical);
    if (strcmp(value, buf) != 0)
        settings_ini_dirty = true;
}

static int parse_card_configuration(void *user, const char *section, const char *name, const char *value) {
    serialized_settings_t* _s = user;

    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    #define DIFFERS(v, s) ((strcmp(v, "ON") == 0) != s)
    if (MATCH("PS1", "Autoboot")
        && DIFFERS(value, ((_s->ps1_flags & SETTINGS_PS1_FLAGS_AUTOBOOT) > 0))) {
        _s->ps1_flags ^= SETTINGS_PS1_FLAGS_AUTOBOOT;
    } else if (MATCH("PS1", "GameID")
        && DIFFERS(value, ((_s->ps1_flags & SETTINGS_PS1_FLAGS_GAME_ID) > 0))) {
        _s->ps1_flags ^= SETTINGS_PS1_FLAGS_GAME_ID;
    } else if (MATCH("PS1", "EnableControllerCombo")
        && DIFFERS(value, ((_s->ps1_flags & SETTINGS_PS1_FLAGS_CTRL_COMBO) > 0))) {
        _s->ps1_flags ^= SETTINGS_PS1_FLAGS_CTRL_COMBO;
    } else if (MATCH("PS1", "RememberLastCard")) {
        /* Scritta come 0/1, ma si accetta anche ON/OFF come le altre. */
        const bool on = (strcmp(value, "1") == 0) || (strcmp(value, "ON") == 0);
        if (on != ((_s->ps1_flags & SETTINGS_PS1_FLAGS_REMEMBER_CARD) > 0))
            _s->ps1_flags ^= SETTINGS_PS1_FLAGS_REMEMBER_CARD;
    } else if (MATCH("PS1", "FastMode_Enable")) {
        /* Timing PIO pre-1.2.1 mentre e' montata la boot card: serve al
           FreePSXBoot "superfast". Vedi ps1_mc_spi.pio. */
        const bool on = (strcmp(value, "1") == 0) || (strcmp(value, "ON") == 0);
        if (on != ((_s->ps1_flags & SETTINGS_PS1_FLAGS_FAST_MODE) > 0))
            _s->ps1_flags ^= SETTINGS_PS1_FLAGS_FAST_MODE;
    } else if (MATCH("PS1", "LedBootCardTimeout")) {
        /* -1 = segue BootCardTimeout, 0 = niente verde fisso, N = N secondi.
           Il tetto rispetto a BootCardTimeout lo mette il getter, cosi' vale
           anche se le due chiavi vengono modificate una alla volta. */
        int timeout = atoi(value);
        if (timeout < 0)
            _s->ps1_led_bootcard_timeout = SETTINGS_LED_BOOTCARD_FOLLOW;
        else if (timeout >= SETTINGS_LED_BOOTCARD_FOLLOW)
            _s->ps1_led_bootcard_timeout = SETTINGS_LED_BOOTCARD_FOLLOW - 1;
        else
            _s->ps1_led_bootcard_timeout = (uint8_t)timeout;
    } else if (MATCH("PS1", "MaxChannels")) {
        /* Canali per card, default per tutte. Il CardX.ini di una singola card
           puo' comunque sovrascriverlo.
           Da 1 a 254 e' quel numero di canali; -1, 0 e il testo non numerico
           vogliono dire "tutti", che qui coincide con il massimo del campo,
           quindi si riscrivono tutti come -1.
           strtol e non atoi: il testo puo' essere qualunque cosa e va letto
           come intero con segno, il byte e' solo dove finisce il risultato.
           Su un numero fuori dai limiti di long strtol satura, e il ramo del
           massimo lo cattura; atoi li' sarebbe comportamento indefinito. */
        const long maxchan = strtol(value, NULL, 10);
        _s->ps1_maxchannels = (maxchan > 0 && maxchan < 255) ? (uint8_t)maxchan : 255;
        mark_if_not_canonical(value, (_s->ps1_maxchannels == 255) ? -1 : (int)maxchan);
    } else if (MATCH("PS1", "MaxCardIdx")) {
        /* Da 1 a 255 e' il numero di memcard scorribili; -1, 0 e il testo non
           numerico vogliono dire "nessun limite", che internamente e' 0 e
           lascia salire card_idx fino a UINT16_MAX (ps1_cardman_next_idx).
           Qui il massimo del campo, 255, resta un limite vero e si scrive come
           numero: e' meno di "nessun limite". */
        const long maxcard = strtol(value, NULL, 10);
        if (maxcard > 255)
            _s->ps1_maxcardidx = 255;
        else if (maxcard > 0)
            _s->ps1_maxcardidx = (uint8_t)maxcard;
        else
            _s->ps1_maxcardidx = 0;
        mark_if_not_canonical(value, (_s->ps1_maxcardidx == 0) ? -1 : _s->ps1_maxcardidx);
    } else if (MATCH("PS1", "BootCardTimeout")) {
        /* -1 = non si esce mai da soli (default), 0 = subito, N = N secondi. */
        int timeout = atoi(value);
        if (timeout < 0)
            _s->ps1_bootcard_timeout = SETTINGS_BOOTCARD_TIMEOUT_NEVER;
        else if (timeout >= SETTINGS_BOOTCARD_TIMEOUT_NEVER)
            _s->ps1_bootcard_timeout = SETTINGS_BOOTCARD_TIMEOUT_NEVER - 1;
        else
            _s->ps1_bootcard_timeout = (uint8_t)timeout;
    } else if (MATCH("PS2", "MaxCardIdx")) {
         int maxcard = atoi(value);
         if (maxcard > 255)
             _s->ps2_maxcardidx = 255;
         else if (maxcard > 0)
            _s->ps2_maxcardidx = maxcard;
    } else if (MATCH("PS2", "Autoboot")
        && DIFFERS(value, ((_s->ps2_flags & SETTINGS_PS2_FLAGS_AUTOBOOT) > 0))) {
        _s->ps2_flags ^= SETTINGS_PS2_FLAGS_AUTOBOOT;
    } else if (MATCH("PS2", "GameID")
        && DIFFERS(value, ((_s->ps2_flags & SETTINGS_PS2_FLAGS_GAME_ID) > 0))) {
        _s->ps2_flags ^= SETTINGS_PS2_FLAGS_GAME_ID;
    } else if (MATCH("PS2", "CardSize")) {
        int size = atoi(value);
        switch (size) {
            case 1:
            case 2:
            case 4:
            case 8:
            case 16:
            case 32:
            case 64:
            case 128:
                _s->ps2_cardsize = size;
                break;
            default:
                break;
        }
    }else if (MATCH("PS2", "Variant")) {
        _s->ps2_variant = PS2_VARIANT_RETAIL;
        if (strcmp(value, "PROTO") == 0) {
            _s->ps2_variant = PS2_VARIANT_PROTO;
        } else if (strcmp(value, "ARCADE") == 0) {
            _s->ps2_variant = PS2_VARIANT_COH;
        } else if (strcmp(value, "CONQUEST") == 0) {
            _s->ps2_variant = PS2_VARIANT_SC2;
        }
    } else if (MATCH("General", "Mode")
        && (strcmp(value, "PS2") == 0) != ((_s->sys_flags & SETTINGS_SYS_FLAGS_PS2_MODE) > 0)) {
        _s->sys_flags ^= SETTINGS_SYS_FLAGS_PS2_MODE;
    } else if (MATCH("General", "FlippedScreen")
        && DIFFERS(value, ((_s->sys_flags & SETTINGS_SYS_FLAGS_FLIPPED_DISPLAY) > 0))) {
        _s->sys_flags ^= SETTINGS_SYS_FLAGS_FLIPPED_DISPLAY;
    }
    #undef MATCH
    return 1;
}

static void settings_deserialize(void) {
    int fd = sd_open(settings_path, O_RDONLY);

    if (fd >= 0) {

        serialized_settings_t newSettings = {.ps2_flags = settings.ps2_flags,
                                             .sys_flags = settings.sys_flags,
                                             .ps2_cardsize = settings.ps2_cardsize,
                                             .ps2_variant = settings.ps2_variant,
                                             .ps1_flags = settings.ps1_flags,
                                             .ps1_maxcardidx = settings.ps1_maxcardidx,
                                             .ps2_maxcardidx = settings.ps2_maxcardidx,
                                             .ps1_bootcard_timeout = settings.ps1_bootcard_timeout,
                                             .ps1_maxchannels = settings.ps1_maxchannels,
                                             .ps1_led_bootcard_timeout = settings.ps1_led_bootcard_timeout};
        serialized_settings = newSettings;
        /* Azzerato qui e non a fine giro: settings_load_sd() torna a ogni
           cambio di modalita' in main(), e un file gia' corretto non deve
           trascinarsi dietro il flag della lettura precedente. */
        settings_ini_dirty = false;
        ini_parse_sd_file(fd, parse_card_configuration, &newSettings);
        sd_close(fd);
        if (memcmp(&newSettings, &serialized_settings, sizeof(serialized_settings))) {
            QPRINTF("Updating settings from ini\n");
            serialized_settings      = newSettings;
            settings.sys_flags       = newSettings.sys_flags;
            settings.ps2_flags       = newSettings.ps2_flags;
            settings.ps2_cardsize    = newSettings.ps2_cardsize;
            settings.ps2_variant     = newSettings.ps2_variant;
            settings.ps1_flags       = newSettings.ps1_flags;
            settings.ps1_maxcardidx  = newSettings.ps1_maxcardidx;
            settings.ps2_maxcardidx  = newSettings.ps2_maxcardidx;
            settings.ps1_bootcard_timeout = newSettings.ps1_bootcard_timeout;
            settings.ps1_maxchannels = newSettings.ps1_maxchannels;
            settings.ps1_led_bootcard_timeout = newSettings.ps1_led_bootcard_timeout;

            wear_leveling_write(0, &settings, sizeof(settings));
        }
    }
}

static void settings_serialize(void) {
    settings_serialize_internal(false);
}

/* force salta il controllo qui sotto: serve dopo una lettura in cui il parser
   ha corretto qualcosa, perche' li' le impostazioni in memoria coincidono gia'
   con quelle "serializzate" e il file non verrebbe mai riscritto. */
static void settings_serialize_internal(bool force) {
    int fd;
    // Only serialize if required
    if (!force &&
        serialized_settings.ps1_maxcardidx == settings.ps1_maxcardidx &&
        serialized_settings.ps2_maxcardidx == settings.ps2_maxcardidx &&
        serialized_settings.ps2_cardsize == settings.ps2_cardsize &&
        serialized_settings.ps2_flags == settings.ps2_flags &&
        serialized_settings.sys_flags == settings.sys_flags &&
        serialized_settings.ps2_variant == settings.ps2_variant &&
        serialized_settings.ps1_bootcard_timeout == settings.ps1_bootcard_timeout &&
        serialized_settings.ps1_maxchannels == settings.ps1_maxchannels &&
        serialized_settings.ps1_led_bootcard_timeout == settings.ps1_led_bootcard_timeout &&
        serialized_settings.ps1_flags == settings.ps1_flags) {
        return;
    }

    if (!sd_exists("/.sd2psx/")) {
        sd_mkdir("/.sd2psx/");
    }

    fd = sd_open(settings_path, O_RDWR | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        QPRINTF("Serializing Settings\n");
        char line_buffer[256] = { 0x0 };
        int written = snprintf(line_buffer, 256, "[General]\n");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "Mode=%s\n", ((settings.sys_flags & SETTINGS_SYS_FLAGS_PS2_MODE) > 0) ? "PS2" : "PS1");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "FlippedScreen=%s\n", ((settings.sys_flags & SETTINGS_SYS_FLAGS_FLIPPED_DISPLAY) > 0) ? "ON" : "OFF");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "[PS1]\n");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "Autoboot=%s\n", ((settings.ps1_flags & SETTINGS_PS1_FLAGS_AUTOBOOT) > 0) ? "ON" : "OFF");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "GameID=%s\n", ((settings.ps1_flags & SETTINGS_PS1_FLAGS_GAME_ID) > 0) ? "ON" : "OFF");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "EnableControllerCombo=%s\n", ((settings.ps1_flags & SETTINGS_PS1_FLAGS_CTRL_COMBO) > 0) ? "ON" : "OFF");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "RememberLastCard=%u\n", ((settings.ps1_flags & SETTINGS_PS1_FLAGS_REMEMBER_CARD) > 0) ? 1u : 0u);
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "FastMode_Enable=%u\n", ((settings.ps1_flags & SETTINGS_PS1_FLAGS_FAST_MODE) > 0) ? 1u : 0u);
        sd_write(fd, line_buffer, written);
        /* -1 e' la forma unica per "nessun limite", come per i due timeout qui
           sotto: cosi' l'ini, il LEGGIMI e la guida dicono tutti la stessa
           cosa. Per le memcard il valore interno e' 0, per i canali e' 255. */
        if (settings.ps1_maxcardidx == 0)
            written = snprintf(line_buffer, 256, "MaxCardIdx=-1\n");
        else
            written = snprintf(line_buffer, 256, "MaxCardIdx=%u\n", settings.ps1_maxcardidx);
        sd_write(fd, line_buffer, written);
        if (settings.ps1_maxchannels == 255)
            written = snprintf(line_buffer, 256, "MaxChannels=-1\n");
        else
            written = snprintf(line_buffer, 256, "MaxChannels=%u\n", settings.ps1_maxchannels);
        sd_write(fd, line_buffer, written);
        if (settings.ps1_bootcard_timeout == SETTINGS_BOOTCARD_TIMEOUT_NEVER)
            written = snprintf(line_buffer, 256, "BootCardTimeout=-1\n");
        else
            written = snprintf(line_buffer, 256, "BootCardTimeout=%u\n", settings.ps1_bootcard_timeout);
        sd_write(fd, line_buffer, written);
        if (settings.ps1_led_bootcard_timeout == SETTINGS_LED_BOOTCARD_FOLLOW)
            written = snprintf(line_buffer, 256, "LedBootCardTimeout=-1\n");
        else
            written = snprintf(line_buffer, 256, "LedBootCardTimeout=%u\n", settings.ps1_led_bootcard_timeout);
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "[PS2]\n");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "Autoboot=%s\n", ((settings.ps2_flags & SETTINGS_PS2_FLAGS_AUTOBOOT) > 0) ? "ON" : "OFF");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "GameID=%s\n", ((settings.ps2_flags & SETTINGS_PS2_FLAGS_GAME_ID) > 0) ? "ON" : "OFF");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "CardSize=%u\n", settings.ps2_cardsize);
        sd_write(fd, line_buffer, written);
        switch (settings.ps2_variant) {
            case PS2_VARIANT_PROTO:
                written = snprintf(line_buffer, 256, "Variant=PROTO\n" );
                break;
            case PS2_VARIANT_COH:
                written = snprintf(line_buffer, 256, "Variant=ARCADE\n" );
                break;
            case PS2_VARIANT_SC2:
                written = snprintf(line_buffer, 256, "Variant=CONQUEST\n" );
                break;
            case PS2_VARIANT_RETAIL:
            default:
                written = snprintf(line_buffer, 256, "Variant=RETAIL\n" );
                break;

        }
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "MaxCardIdx=%u\n", settings.ps2_maxcardidx);
        sd_write(fd, line_buffer, written);

        sd_close(fd);
    }
    serialized_settings.sys_flags       = settings.sys_flags;
    serialized_settings.ps2_flags       = settings.ps2_flags;
    serialized_settings.ps2_cardsize    = settings.ps2_cardsize;
    serialized_settings.ps2_variant     = settings.ps2_variant;
    serialized_settings.ps1_flags       = settings.ps1_flags;
    serialized_settings.ps1_maxcardidx  = settings.ps1_maxcardidx;
    serialized_settings.ps2_maxcardidx  = settings.ps2_maxcardidx;
    serialized_settings.ps1_bootcard_timeout = settings.ps1_bootcard_timeout;
    serialized_settings.ps1_maxchannels = settings.ps1_maxchannels;
    serialized_settings.ps1_led_bootcard_timeout = settings.ps1_led_bootcard_timeout;
}

static void settings_reset(void) {
    memset(&settings, 0, sizeof(settings));
    settings.version_magic = SETTINGS_VERSION_MAGIC;
    settings.display_timeout = 0; // off
    settings.display_contrast = 255; // 100%
    settings.display_vcomh = 0x30; // 0.83 x VCC
    /* Autoboot acceso di default: se la BootCard non c'e' sulla microSD,
       ps1_cardman ripiega da solo sulla card normale (try_set_boot_card). */
    /* FAST_MODE acceso di default: agisce solo mentre e' montata la boot card,
       quindi su una microSD senza payload non cambia nulla. */
    settings.ps1_flags = SETTINGS_PS1_FLAGS_AUTOBOOT
                       | SETTINGS_PS1_FLAGS_GAME_ID
                       | SETTINGS_PS1_FLAGS_CTRL_COMBO
                       | SETTINGS_PS1_FLAGS_FAST_MODE;
    settings.ps2_flags = SETTINGS_PS2_FLAGS_GAME_ID;
    settings.ps2_cardsize = 8;
    settings.ps2_variant = PS2_VARIANT_RETAIL;
    settings.ps1_maxcardidx = 10;
    settings.ps2_maxcardidx = 0; //unlimited UINT16_MAX
    settings.ps1_bootcard_timeout = SETTINGS_BOOTCARD_TIMEOUT_NEVER; // nessuna uscita a tempo
    settings.ps1_maxchannels = 3;
    settings.ps1_led_bootcard_timeout = SETTINGS_LED_BOOTCARD_FOLLOW; // verde per tutta la boot mode
    if (wear_leveling_write(0, &settings, sizeof(settings)) == WEAR_LEVELING_FAILED)
        fatal(ERR_SETTINGS, "failed to reset settings");
}

void settings_load_sd(void) {
    if (sd_exists(settings_path)) {
        settings_deserialize();
        if (settings_ini_dirty) {
            settings_ini_dirty = false;
            settings_serialize_internal(true);
        }
    } else {
        /* File mancante = ritorno ai valori di fabbrica. Senza il reset qui il
           file verrebbe riscritto a partire da quello che c'e' in flash, e
           cancellarlo non servirebbe a niente: e' quello che rendeva
           irraggiungibili i nuovi default su una scheda gia' usata.
           Cosi' cambiare un default non richiede piu' di alzare il magic di
           versione, basta cancellare il file.
           force perche' dopo il reset le impostazioni possono gia' coincidere
           con quelle serializzate, e allora il file non verrebbe ricreato. */
        settings_reset();
        settings_serialize_internal(true);
    }
}

void settings_init(void) {
    QPRINTF("Settings - init\n");
    if (wear_leveling_init() == WEAR_LEVELING_FAILED) {
        QPRINTF("failed to init wear leveling, reset settings\n");
        settings_reset();

        if (wear_leveling_init() == WEAR_LEVELING_FAILED)
            fatal(ERR_SETTINGS, "cannot init eeprom emu");
    }

    wear_leveling_read(0, &settings, sizeof(settings));

    if (settings.version_magic != SETTINGS_VERSION_MAGIC) {
        QPRINTF("version magic mismatch, reset settings\n");
        settings_reset();
    }


    tempmode = settings.sys_flags & SETTINGS_SYS_FLAGS_PS2_MODE;
}

static void settings_update_part(void *settings_ptr, uint32_t sz) {
    if (multicore_lockout_victim_is_initialized(1))
       multicore_lockout_start_blocking();
    wear_leveling_write((uint8_t*)settings_ptr - (uint8_t*)&settings, settings_ptr, sz);
    if (multicore_lockout_victim_is_initialized(1))
        multicore_lockout_end_blocking();
    settings_serialize();
}


int settings_get_ps2_card(void) {
    if (settings.ps2_card < IDX_MIN)
        return IDX_MIN;
    else if ((settings.ps2_card > settings.ps2_maxcardidx) && (settings.ps2_maxcardidx > 0))
        return settings.ps2_maxcardidx;
    return settings.ps2_card;
}

int settings_get_ps2_channel(void) {
    if (settings.ps2_channel < CHAN_MIN)
        return CHAN_MIN;
    return settings.ps2_channel;
}

int settings_get_ps2_boot_channel(void) {
    if (settings.ps2_boot_channel < CHAN_MIN)
        return CHAN_MIN;
    return settings.ps2_boot_channel;
}

uint8_t settings_get_ps2_cardsize(void) {
    return settings.ps2_cardsize;
}

int settings_get_ps2_variant(void) {
    return settings.ps2_variant;
}

uint8_t settings_get_ps1_maxcardidx(void) {
    return settings.ps1_maxcardidx;
}

uint8_t settings_get_ps2_maxcardidx(void) {
    return settings.ps2_maxcardidx;
}

void settings_set_ps2_card(int card) {
    if (card != settings.ps2_card) {
        settings.ps2_card = card;
        SETTINGS_UPDATE_FIELD(ps2_card);
    }
}

void settings_set_ps2_channel(int chan) {
    if (chan != settings.ps2_channel) {
        settings.ps2_channel = chan;
        SETTINGS_UPDATE_FIELD(ps2_channel);
    }
}

void settings_set_ps2_boot_channel(int chan) {
    if (chan != settings.ps2_boot_channel) {
        settings.ps2_boot_channel = chan;
        SETTINGS_UPDATE_FIELD(ps2_boot_channel);
    }
}

void settings_set_ps2_cardsize(uint8_t size) {
    if (size != settings.ps2_cardsize) {
        settings.ps2_cardsize = size;
        SETTINGS_UPDATE_FIELD(ps2_cardsize);
    }
}

void settings_set_ps2_variant(int x) {
    if (settings.ps2_variant != x) {
        settings.ps2_variant = x;
        SETTINGS_UPDATE_FIELD(ps2_variant);
    }
}

void settings_set_ps1_maxcardidx(uint8_t x) {
    if (settings.ps1_maxcardidx != x) {
        settings.ps1_maxcardidx = x;
        SETTINGS_UPDATE_FIELD(ps1_maxcardidx);
    }
}

void settings_set_ps2_maxcardidx(uint8_t x) {
    if (settings.ps2_maxcardidx != x) {
        settings.ps2_maxcardidx = x;
        SETTINGS_UPDATE_FIELD(ps2_maxcardidx);
    }
}

int settings_get_ps1_card(void) {
    if (settings.ps1_card < IDX_MIN)
        return IDX_MIN;
    else if ((settings.ps1_card > settings.ps1_maxcardidx) && (settings.ps1_maxcardidx > 0))
        return settings.ps1_maxcardidx;
    return settings.ps1_card;
}

int settings_get_ps1_channel(void) {
    if (settings.ps1_channel < CHAN_MIN)
        return CHAN_MIN;
    return settings.ps1_channel;
}

int settings_get_ps1_boot_channel(void) {
    if (settings.ps1_boot_channel < CHAN_MIN)
        return CHAN_MIN;
    return settings.ps1_boot_channel;
}

void settings_set_ps1_card(int card) {
    if (card != settings.ps1_card) {
        settings.ps1_card = card;
        SETTINGS_UPDATE_FIELD(ps1_card);
    }
}

void settings_set_ps1_channel(int chan) {
    if (chan != settings.ps1_channel) {
        settings.ps1_channel = chan;
        SETTINGS_UPDATE_FIELD(ps1_channel);
    }
}

void settings_set_ps1_boot_channel(int chan) {
    if (chan != settings.ps1_boot_channel) {
        settings.ps1_boot_channel = chan;
        SETTINGS_UPDATE_FIELD(ps1_boot_channel);
    }
}

uint8_t settings_get_ps1_bootcard_timeout(void) {
    return settings.ps1_bootcard_timeout;
}

void settings_set_ps1_bootcard_timeout(uint8_t seconds) {
    if (seconds != settings.ps1_bootcard_timeout) {
        settings.ps1_bootcard_timeout = seconds;
        SETTINGS_UPDATE_FIELD(ps1_bootcard_timeout);
    }
}

int settings_get_mode(bool current) {
    if (current && tempmode == MODE_TEMP_PS1)
        return MODE_PS1;
    else if (!(settings.sys_flags & SETTINGS_SYS_FLAGS_PS2_MODE))
        return MODE_PS1;
    else
        return MODE_PS2;
}

void settings_set_mode(int mode) {
    if (mode == MODE_TEMP_PS1) {
        tempmode = MODE_TEMP_PS1;
        return;
    } else if (mode != MODE_PS1 && mode != MODE_PS2)
        return;

    if (mode != settings_get_mode(false)) {
        /* clear old mode, then set what was passed in */
        settings.sys_flags &= ~SETTINGS_SYS_FLAGS_PS2_MODE;
        settings.sys_flags |= mode;
        SETTINGS_UPDATE_FIELD(sys_flags);
        tempmode = settings.sys_flags & SETTINGS_SYS_FLAGS_PS2_MODE;
    }
}

bool settings_get_ps1_autoboot(void) {
    return (settings.ps1_flags & SETTINGS_PS1_FLAGS_AUTOBOOT);
}

void settings_set_ps1_autoboot(bool autoboot) {
    if (autoboot != settings_get_ps1_autoboot())
        settings.ps1_flags ^= SETTINGS_PS1_FLAGS_AUTOBOOT;
    SETTINGS_UPDATE_FIELD(ps1_flags);
}

bool settings_get_ps1_game_id(void) {
    return (settings.ps1_flags & SETTINGS_PS1_FLAGS_GAME_ID);
}

void settings_set_ps1_game_id(bool enabled) {
    if (enabled != settings_get_ps1_game_id())
        settings.ps1_flags ^= SETTINGS_PS1_FLAGS_GAME_ID;
    SETTINGS_UPDATE_FIELD(ps1_flags);
}

uint8_t settings_get_ps1_led_bootcard_timeout(void) {
    const uint8_t led = settings.ps1_led_bootcard_timeout;

    if (led == SETTINGS_LED_BOOTCARD_FOLLOW)
        return led;

    /* Non ha senso tenere il verde acceso piu' a lungo di quanto duri la boot
       mode: si finirebbe col segnalare una modalita' gia' finita. Il tetto e'
       messo qui e non in scrittura, cosi' vale anche quando le due chiavi
       dell'ini vengono modificate una alla volta.
       Se la boot mode non scade mai non c'e' nessun tetto da applicare. */
    const uint8_t card = settings.ps1_bootcard_timeout;
    if (card != SETTINGS_BOOTCARD_TIMEOUT_NEVER && led > card)
        return card;

    return led;
}

void settings_set_ps1_led_bootcard_timeout(uint8_t seconds) {
    if (seconds != settings.ps1_led_bootcard_timeout) {
        settings.ps1_led_bootcard_timeout = seconds;
        SETTINGS_UPDATE_FIELD(ps1_led_bootcard_timeout);
    }
}

uint8_t settings_get_ps1_maxchannels(void) {
    return settings.ps1_maxchannels;
}

void settings_set_ps1_maxchannels(uint8_t maxchannels) {
    if (maxchannels != settings.ps1_maxchannels) {
        settings.ps1_maxchannels = maxchannels;
        SETTINGS_UPDATE_FIELD(ps1_maxchannels);
    }
}

bool settings_get_ps1_remember_card(void) {
    return (settings.ps1_flags & SETTINGS_PS1_FLAGS_REMEMBER_CARD);
}

void settings_set_ps1_remember_card(bool remember) {
    if (remember != settings_get_ps1_remember_card())
        settings.ps1_flags ^= SETTINGS_PS1_FLAGS_REMEMBER_CARD;
    SETTINGS_UPDATE_FIELD(ps1_flags);
}

bool settings_get_ps1_fast_mode(void) {
    return (settings.ps1_flags & SETTINGS_PS1_FLAGS_FAST_MODE);
}

void settings_set_ps1_fast_mode(bool fast_mode) {
    if (fast_mode != settings_get_ps1_fast_mode())
        settings.ps1_flags ^= SETTINGS_PS1_FLAGS_FAST_MODE;
    SETTINGS_UPDATE_FIELD(ps1_flags);
}


bool settings_get_ps1_controllercombo(void) {
    return (settings.ps1_flags & SETTINGS_PS1_FLAGS_CTRL_COMBO);
}

void settings_set_ps1_controllercombo(bool controllercombo) {
    if (controllercombo != settings_get_ps1_controllercombo())
        settings.ps1_flags ^= SETTINGS_PS1_FLAGS_CTRL_COMBO;
    SETTINGS_UPDATE_FIELD(ps1_flags);
}

bool settings_get_ps2_autoboot(void) {
    return (settings.ps2_flags & SETTINGS_PS2_FLAGS_AUTOBOOT);
}

void settings_set_ps2_autoboot(bool autoboot) {
    if (autoboot != settings_get_ps2_autoboot())
        settings.ps2_flags ^= SETTINGS_PS2_FLAGS_AUTOBOOT;
    SETTINGS_UPDATE_FIELD(ps2_flags);
}

bool settings_get_ps2_game_id(void) {
    return (settings.ps2_flags & SETTINGS_PS2_FLAGS_GAME_ID);
}

void settings_set_ps2_game_id(bool enabled) {
    if (enabled != settings_get_ps2_game_id())
        settings.ps2_flags ^= SETTINGS_PS2_FLAGS_GAME_ID;
    SETTINGS_UPDATE_FIELD(ps2_flags);
}

uint8_t settings_get_display_timeout() {
    return settings.display_timeout;
}

uint8_t settings_get_display_contrast() {
    return settings.display_contrast;
}

uint8_t settings_get_display_vcomh() {
    return settings.display_vcomh;
}

bool settings_get_display_flipped() {
    return (settings.sys_flags & SETTINGS_SYS_FLAGS_FLIPPED_DISPLAY);
}

void settings_set_display_timeout(uint8_t display_timeout) {
    settings.display_timeout = display_timeout;
    SETTINGS_UPDATE_FIELD(display_timeout);
}

void settings_set_display_contrast(uint8_t display_contrast) {
    settings.display_contrast = display_contrast;
    SETTINGS_UPDATE_FIELD(display_contrast);
}

void settings_set_display_vcomh(uint8_t display_vcomh) {
    settings.display_vcomh = display_vcomh;
    SETTINGS_UPDATE_FIELD(display_vcomh);
}

void settings_set_display_flipped(bool flipped) {
    if (flipped != settings_get_display_flipped())
        settings.sys_flags ^= SETTINGS_SYS_FLAGS_FLIPPED_DISPLAY;
    SETTINGS_UPDATE_FIELD(sys_flags);
}