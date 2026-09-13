# Quattro varianti, ognuna esplicita, e nessun ramo di default.
#
# Upstream ne ha sei e il default e' "SD2PSX", cioe' il suo pinout. Una variante
# scritta male - "tuning", o "psxmemcardmsc" minuscolo, perche' STREQUAL
# distingue le maiuscole - cadeva nel ramo else() finale, che assegnava quel
# pinout SENZA DIRLO. Il firmware si compilava, si flashava, e la scheda non
# funzionava: LED e microSD sui pin sbagliati, niente USB, nessun messaggio
# d'errore da nessuna parte. Il 03/09/2026 e' costato due recuperi col BOOTSEL.
#
# Qui il default e' PSXMemCardMSC - questa board - e il ramo finale e' un
# FATAL_ERROR: un nome sconosciuto ferma la build invece di ereditare un pinout
# in silenzio. E' una divergenza deliberata da upstream, non un ritardo da
# recuperare: su main il default e' ancora SD2PSX e l'else() e' ancora li'.
#
# SD2PSX, PMC+ e PMCZero servono a compilare per le board di upstream, con
# tutte le funzioni di questo fork sopra (passthrough MSC, BootCard, comandi
# seriali, albero automatico). Cambiano solo i pin e cosa la board ha addosso.
# NON sono state provate su hardware: non abbiamo nessuna di quelle tre schede.
# I pin vengono da misc/variants.cmake di upstream, verificato sia sulla copia
# locale al tag 1.4.0 (git show 4377a53:...) sia su sd2psXtd/firmware@main:
# identici.
#
# Le altre due varianti di upstream restano fuori:
#
#   PSXMemCard  ha ESATTAMENTE i pin di PSXMemCardMSC: e' la variante da cui la
#               nostra e' derivata. Su quella board si flasha psxmemcardmsc.uf2
#               e si hanno tutte le funzioni; una variante a parte darebbe lo
#               stesso hardware con meno dentro.
#   SD2PSXlite  upstream ha un PARENT_DIRECTORY penzolante dentro
#               add_compile_definitions(): da non ricopiare cosi' com'e'.
#
# Il file completo di upstream sta in git fino a 4377a53.

set(VARIANT "PSXMemCardMSC" CACHE STRING "Firmware variant to build")
set_property(CACHE VARIANT PROPERTY STRINGS
                    "PSXMemCardMSC"
                    "SD2PSX"
                    "PMC+"
                    "PMCZero")

message(STATUS "Building for ${VARIANT}")

if (VARIANT STREQUAL "PSXMemCardMSC")
    # Board custom RP2040 con firmware PicoMemcard modificato, pinout ricavato
    # per reverse engineering dal dump del firmware (vedi docs/PINOUT-board.md).
    # Coincide col profilo PICO di PicoMemcard (inc/config.h) e con la variante
    # PSXMemCard di upstream. Rispetto a quest'ultima:
    #   - passthrough microSD via USB MSC attivo
    #   - schema colori proprio sul LED RGB (LED_STYLE_ACTIVITY)
    #   - pulsanti spostati su GPIO liberi: la board non ne ha di fisici, e i
    #     default (23 e 21) si sovrappongono ai pin del LED RGB
    set(PIN_PSX_ACK 9)
    set(PIN_PSX_SEL 7)
    set(PIN_PSX_CLK 8)
    set(PIN_PSX_CMD 6)
    set(PIN_PSX_DAT 5)
    set(PIN_PSX_SPD_SEL 10)
    add_compile_definitions("UART_TX=0"
                            "UART_RX=1"
                            "UART_PERIPH=uart1"
                            "UART_BAUD=115200"
                            "SD_PERIPH=SPI"
                            "SD_MISO=16"
                            "SD_MOSI=19"
                            "SD_SCK=18"
                            "SD_CS=17"
                            "MMCE_PRODUCT_ID=0x3"
                            # LED RGB. Pin e polarita' verificati sull'hardware
                            # con test-firmware-3led: GPIO 25 = rosso,
                            # 24 = verde, 23 = blu, tutti attivi alti. Coincide
                            # con i default di upstream.
                            # Schema colori in GUIDA.md sezione 3.
                            "LED_STYLE_ACTIVITY=1"
                            "LED_R=25"
                            "LED_G=24"
                            "LED_B=23"
                            # Colori a pieno regime, cioe' acceso/spento: i 7
                            # colori puri bastano per i 7 stati. Per le
                            # sfumature (arancione, viola, luminosita' ridotta)
                            # mettere LED_PWM=1 e tarare LED_GAIN_R/G/B e
                            # LED_BRIGHTNESS.
                            "LED_PWM=0"
                            # L'uscita a tempo dalla BootCard non e' un define:
                            # si regola con BootCardTimeout in settings.ini,
                            # in secondi, -1 = mai (default).
                            "PIN_BTN_LEFT=14"
                            "PIN_BTN_RIGHT=15"
                            )
    set(SD2PSX_WITH_GUI FALSE)
    set(SD2PSX_WITH_PSRAM FALSE)
    set(SD2PSX_WITH_LED TRUE)
    set(SD2PSX_WITH_MSC TRUE)
    add_compile_definitions(PICO_FLASH_SIZE_BYTES=2097152)

elseif (VARIANT STREQUAL "SD2PSX")
    # Board SD2PSX / PSxMemCard Gen2: display OLED, PSRAM, 16 MB di flash e due
    # pulsanti fisici. Pinout identico a upstream, con sopra il passthrough MSC.
    #
    # Niente PIN_BTN_*: i default di src/config.h sono gia' 23 e 21, che sono i
    # pulsanti di questa board. E niente PMC_BUTTONS, che oltre ad aggiungere un
    # terzo pulsante INVERTE la polarita' di lettura in input.c.
    #
    # LED spento perche' la board non ne ha uno utile: qui si guarda l'OLED. Di
    # conseguenza LED_STYLE_ACTIVITY non ha senso e non viene definito.
    set(PIN_PSX_ACK 16)
    set(PIN_PSX_SEL 17)
    set(PIN_PSX_CLK 18)
    set(PIN_PSX_CMD 19)
    set(PIN_PSX_DAT 20)
    set(PIN_PSX_SPD_SEL 10)
    add_compile_definitions("UART_TX=8"
                            "UART_RX=9"
                            "UART_PERIPH=uart1"
                            "UART_BAUD=3000000"
                            "SD_PERIPH=SPI1"
                            "SD_MISO=24"
                            "SD_MOSI=27"
                            "SD_SCK=26"
                            "SD_CS=29"
                            "MMCE_PRODUCT_ID=0x1"
                            )
    set(SD2PSX_WITH_GUI TRUE)
    set(SD2PSX_WITH_PSRAM TRUE)
    set(SD2PSX_WITH_LED FALSE)
    # Aggiunta di questo fork. Limite noto: i loop di src/usb/msc_mode.c non
    # chiamano gui_task(), quindi mentre il volume e' montato sul PC l'OLED
    # resta sull'ultimo fotogramma. E' voluto finche' non c'e' una schermata
    # apposta: gui_task() legge la lista card dalla microSD, che in passthrough
    # e' dell'host.
    set(SD2PSX_WITH_MSC TRUE)
    add_compile_definitions(PICO_FLASH_SIZE_BYTES=16777216)

elseif (VARIANT STREQUAL "PMC+")
    # PicoMemcard+ : stessi pin PSX e microSD di PSXMemCardMSC - e' lo stesso
    # profilo PICO di PicoMemcard - ma con tre pulsanti FISICI su 26/27/28 e
    # senza LED RGB.
    #
    # E' l'unico motivo per cui esiste come variante separata: su un PMC+
    # psxmemcardmsc.uf2 gira, ma cerca i pulsanti su 14/15 (liberi sulla nostra
    # board apposta) e quindi li' non fanno niente.
    #
    # PMC_BUTTONS=1 non aggiunge solo il terzo pulsante: INVERTE anche la
    # polarita' di lettura in input.c. Va messo o tolto insieme ai pin.
    #
    # Niente LED_STYLE_ACTIVITY: senza LED non avrebbe nulla da pilotare.
    set(PIN_PSX_ACK 9)
    set(PIN_PSX_SEL 7)
    set(PIN_PSX_CLK 8)
    set(PIN_PSX_CMD 6)
    set(PIN_PSX_DAT 5)
    set(PIN_PSX_SPD_SEL 10)
    add_compile_definitions("UART_TX=0"
                            "UART_RX=1"
                            "UART_PERIPH=uart1"
                            "UART_BAUD=115200"
                            "SD_PERIPH=SPI"
                            "SD_MISO=16"
                            "SD_MOSI=19"
                            "SD_SCK=18"
                            "SD_CS=17"
                            "MMCE_PRODUCT_ID=0x3"
                            "PMC_BUTTONS=1"
                            "PIN_BTN_LEFT=27"
                            "PIN_BTN_RIGHT=28"
                            "PIN_BTN_BOOT=26"
                            )
    set(SD2PSX_WITH_GUI FALSE)
    set(SD2PSX_WITH_PSRAM FALSE)
    set(SD2PSX_WITH_LED FALSE)
    set(SD2PSX_WITH_MSC TRUE)
    add_compile_definitions(PICO_FLASH_SIZE_BYTES=2097152)

elseif (VARIANT STREQUAL "PMCZero")
    # PicoMemcard su RP2040-Zero: LED WS2812 indirizzabile su GPIO 16 (il pin e'
    # fisso in src/led/led.c), tre pulsanti, 2 MB di flash. Il pinout coincide
    # col profilo RP2040ZERO di PicoMemcard (inc/config.h).
    #
    # LED_STYLE_ACTIVITY vale anche qui: passa tutto da led_set_color(), che ha
    # gia' il ramo WS2812, e sul WS2812 i colori sono a 8 bit per canale invece
    # che acceso/spento. LED_PWM non c'entra - quel ramo e' per i LED a GPIO.
    set(PIN_PSX_ACK 13)
    set(PIN_PSX_SEL 11)
    set(PIN_PSX_CLK 12)
    set(PIN_PSX_CMD 10)
    set(PIN_PSX_DAT 9)
    set(PIN_PSX_SPD_SEL 11)
    add_compile_definitions("UART_TX=7"
                            "UART_RX=8"
                            "UART_PERIPH=uart1"
                            "UART_BAUD=115200"
                            "SD_PERIPH=SPI"
                            "SD_MISO=0"
                            "SD_MOSI=3"
                            "SD_SCK=2"
                            "SD_CS=1"
                            "MMCE_PRODUCT_ID=0x4"
                            "PMC_BUTTONS=1"
                            "PIN_BTN_LEFT=27"
                            "PIN_BTN_RIGHT=28"
                            "PIN_BTN_BOOT=26"
                            "WS2812=1"
                            # Aggiunta di questo fork: schema colori in
                            # GUIDA.md sezione 3, in inglese in
                            # docs/WHATS-NEW.md.
                            "LED_STYLE_ACTIVITY=1"
                            )
    set(SD2PSX_WITH_GUI FALSE)
    set(SD2PSX_WITH_PSRAM FALSE)
    set(SD2PSX_WITH_LED TRUE)
    set(SD2PSX_WITH_MSC TRUE)
    add_compile_definitions(PICO_FLASH_SIZE_BYTES=2097152)

else()
    # Il ramo che upstream usa per assegnare il pinout SD2PSX in silenzio. Qui
    # ferma la build: e' la protezione che mancava il 03/09/2026.
    message(FATAL_ERROR
        "VARIANT=\"${VARIANT}\" non esiste, e le maiuscole contano: "
        "\"psxmemcardmsc\" NON e' \"PSXMemCardMSC\". "
        "Le varianti sono PSXMemCardMSC (questa board), SD2PSX, PMC+, PMCZero. "
        "Prima un nome sbagliato dava il pinout di upstream in silenzio, "
        "e la scheda non partiva.")
endif()
