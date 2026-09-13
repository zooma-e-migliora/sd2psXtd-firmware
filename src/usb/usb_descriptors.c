/* Descrittori USB per il dispositivo composito CDC + MSC.
 *
 * Layout interfacce (l'ordine conta: il driver stdio usa l'istanza CDC 0):
 *   itf 0 + 1 : CDC  (control + data)   -> interfaccia comandi seriale
 *   itf 2     : MSC  (bulk in/out)      -> passthrough microSD
 */

#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

/* VID/PID di default TinyUSB, come nel firmware PicoMemcard attualmente in uso
   sulla board (VID 0xCAFE, PID 0x4003 = CDC|MSC). */
#define USB_VID 0xCAFE
#define USB_BCD 0x0200

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_MSC,
    ITF_NUM_TOTAL,
};

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_MSC_OUT   0x03
#define EPNUM_MSC_IN    0x83

/* --- device descriptor -------------------------------------------------- */

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,

    /* Composito: la classe va dichiarata a livello di interfaccia tramite IAD. */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = USB_VID,
    /* PID costruito come negli esempi TinyUSB: bit 0 = CDC, bit 1 = MSC. */
    .idProduct = 0x4000 | 0x01 | 0x02,
    .bcdDevice = 0x0100,

    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,

    .bNumConfigurations = 0x01,
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

/* --- configuration descriptor ------------------------------------------- */

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),

    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

/* --- string descriptors -------------------------------------------------- */

static char serial_str[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

static const char *const string_desc_arr[] = {
    [0] = (const char[]){0x09, 0x04},  /* 0: langid, English (0x0409) */
    [1] = "sd2psXtd",                  /* 1: manufacturer */
    [2] = "sd2psXtd Memory Card",      /* 2: product */
    [3] = serial_str,                  /* 3: serial, dallo unique id del chip */
    [4] = "sd2psXtd CDC",              /* 4: CDC interface */
    [5] = "sd2psXtd microSD",          /* 5: MSC interface */
};

static uint16_t desc_str[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    size_t chr_count;

    if (index == 0) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= TU_ARRAY_SIZE(string_desc_arr))
            return NULL;

        if (index == 3 && serial_str[0] == '\0')
            pico_get_unique_board_id_string(serial_str, sizeof(serial_str));

        const char *str = string_desc_arr[index];

        chr_count = strlen(str);
        const size_t max_count = TU_ARRAY_SIZE(desc_str) - 1;
        if (chr_count > max_count)
            chr_count = max_count;

        for (size_t i = 0; i < chr_count; i++)
            desc_str[1 + i] = str[i];
    }

    /* primo byte: lunghezza totale, secondo: tipo descrittore */
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return desc_str;
}
