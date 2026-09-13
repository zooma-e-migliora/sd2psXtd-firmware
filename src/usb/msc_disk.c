/* Passthrough USB MSC della microSD.
 *
 * Espone i settori fisici della scheda, come farebbe un lettore di card:
 * nessun RAM disk, nessun filesystem duplicato, mappatura LBA -> settore diretta.
 * E' lo stesso approccio del firmware PicoMemcard attualmente in uso sulla board.
 *
 * L'unita' e' dichiarata pronta soltanto in modalita' MSC (vedi msc_disk_set_enabled):
 * fuori da quella modalita' il firmware sta usando la SD per l'emulazione carta e
 * l'host riceve "medium not present". Le due cose non convivono mai.
 */

#include "usb/msc_disk.h"

#include <stdbool.h>
#include <string.h>

#include "debug.h"
#include "sd.h"
#include "tusb.h"

#define MSC_VENDOR_ID  "sd2psXtd"
#define MSC_PRODUCT_ID "microSD"
#define MSC_REVISION   "1.0"

static bool msc_enabled = false;
static bool msc_write_seen = false;
static bool msc_read_seen = false;
static bool msc_host_ejected = false;

void msc_disk_set_enabled(bool enabled) {
    msc_enabled = enabled;
    if (!enabled) {
        msc_write_seen = false;
        msc_read_seen = false;
        msc_host_ejected = false;
    }
}

bool msc_disk_is_enabled(void) {
    return msc_enabled;
}

bool msc_disk_write_occurred(void) {
    bool seen = msc_write_seen;
    msc_write_seen = false;
    return seen;
}

bool msc_disk_read_occurred(void) {
    bool seen = msc_read_seen;
    msc_read_seen = false;
    return seen;
}

bool msc_disk_host_ejected(void) {
    return msc_host_ejected;
}

/* --- callback TinyUSB ---------------------------------------------------- */

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, MSC_VENDOR_ID, strnlen(MSC_VENDOR_ID, 8));
    memcpy(product_id, MSC_PRODUCT_ID, strnlen(MSC_PRODUCT_ID, 16));
    memcpy(product_rev, MSC_REVISION, strnlen(MSC_REVISION, 4));
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (!msc_enabled || msc_host_ejected || sd_sector_count() == 0) {
        /* 3A-00 = medium not present */
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = sd_sector_count();
    *block_size = sd_sector_size();
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return msc_enabled;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer,
                          uint32_t bufsize) {
    (void)lun;

    if (!msc_enabled)
        return -1;
    if (offset != 0)                        /* solo accessi allineati al settore */
        return -1;
    if (bufsize == 0 || (bufsize % sd_sector_size()) != 0)
        return -1;

    const uint32_t sectors = bufsize / sd_sector_size();
    if (lba + sectors > sd_sector_count())
        return -1;

    if (!sd_read_sectors(lba, (uint8_t *)buffer, sectors)) {
        /* 03-11 = unrecovered read error */
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);
        return -1;
    }

    msc_read_seen = true;

    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer,
                           uint32_t bufsize) {
    (void)lun;

    if (!msc_enabled)
        return -1;
    if (offset != 0)
        return -1;
    if (bufsize == 0 || (bufsize % sd_sector_size()) != 0)
        return -1;

    const uint32_t sectors = bufsize / sd_sector_size();
    if (lba + sectors > sd_sector_count())
        return -1;

    if (!sd_write_sectors(lba, buffer, sectors)) {
        /* 03-0C = write error */
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0C, 0x00);
        return -1;
    }

    msc_write_seen = true;
    return (int32_t)bufsize;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
                           bool load_eject) {
    (void)lun;
    (void)power_condition;

    if (load_eject && !start) {
        /* L'host ha espulso il volume: svuotiamo la cache della scheda e
           segnaliamo al main loop che puo' uscire dalla modalita' MSC. */
        sd_sync_device();
        msc_host_ejected = true;
        DPRINTF("MSC: volume espulso dall'host\n");
    }

    return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer,
                        uint16_t bufsize) {
    (void)buffer;
    (void)bufsize;

    switch (scsi_cmd[0]) {
        case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
            return 0;
        default:
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
            return -1;
    }
}
