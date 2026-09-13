/* Configurazione TinyUSB per il dispositivo composito CDC + MSC di sd2psXtd.
 *
 * Sostituisce quella di pico_stdio_usb (che prevede solo CDC): quando
 * SD2PSX_WITH_MSC e' attivo il target NON linka pico_stdio_usb, ma
 * tinyusb_device piu' i sorgenti in src/usb/.
 */
#pragma once

#include "pico/types.h"

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

/* I buffer USB devono stare in una sezione a cui il controller possa accedere. */
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#define CFG_TUD_ENDPOINT0_SIZE 64

/* --- classi abilitate --- */
#define CFG_TUD_CDC 1
#define CFG_TUD_MSC 1
#define CFG_TUD_MSC_EP_BUFSIZE 512

#define CFG_TUD_MIDI 0
#define CFG_TUD_HID 0
#define CFG_TUD_VENDOR 0

/* Stesse dimensioni usate da pico_stdio_usb, per non cambiare il
   comportamento dell'interfaccia seriale esistente. */
#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 64
#define CFG_TUD_CDC_EP_BUFSIZE 64
