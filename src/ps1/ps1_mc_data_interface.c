#include <ps2/card_emu/ps2_mc_data_interface.h>
#include <stdbool.h>
#include <string.h>


#include "pico/multicore.h"
#include "pico/time.h"
#include "ps1_mc_data_interface.h"
#include "bigmem.h"
#if WITH_PSRAM
#include "psram.h"
#endif
#include "ps1_cardman.h"
#include "ps1_memory_card.h"
#include "ps1_dirty.h"

#include "debug.h"


#define PAGE_CACHE_SIZE 40
#define MAX_READ_AHEAD 0
#define PS1_CARD_SIZE (128 * 1024)
#define PS1_CARD_PAGES (PS1_CARD_SIZE / PS1_PAGE_SIZE)

/* Secondo strato del clamp del settore. Il primo, quello che conta per il
   protocollo, sta in ps1_memory_card.c: maschera i due byte dell'indirizzo
   appena arrivano, cosi' l'eco dell'indirizzo confermato e' quello giusto.
   Questo qui non serve a rispondere bene, serve a garantire che NESSUN
   chiamante - oggi o domani - possa indicizzare fuori da "card". Per un
   indirizzo valido sono entrambi no-op. */
#define PS1_WRAP_PAGE(p)    ((p) & (PS1_CARD_PAGES - 1u))
#define PS1_WRAP_ADDRESS(a) ((a) & (PS1_CARD_SIZE - 1u))

static volatile bool dma_in_progress = false;
static volatile bool write_occured = false;
static volatile bool read_occured = false;


#define card cache


#if WITH_PSRAM
static void __time_critical_func(ps1_mc_data_interface_rx_done)() {
    dma_in_progress = false;

    ps1_dirty_unlock();
}

void __time_critical_func(ps1_mc_data_interface_start_dma)(uint32_t page) {
    ps1_dirty_lockout_renew();
    /* the spinlock will be unlocked by the DMA irq once all data is tx'd */
    ps1_dirty_lock();
    dma_in_progress = true;
    psram_read_dma(PS1_WRAP_PAGE(page) * PS1_PAGE_SIZE, card, PS1_PAGE_SIZE, ps1_mc_data_interface_rx_done);
}
#endif



void __time_critical_func(ps1_mc_data_interface_setup_read_page)(uint32_t page) {
#if WITH_PSRAM
        ps1_mc_data_interface_start_dma(page);
#endif
}

uint8_t* __time_critical_func(ps1_mc_data_interface_get_page)(uint32_t page) {
    uint8_t* ret = NULL;

    read_occured = true;

#ifdef WITH_PSRAM
    (void)page;
    ret = card;
#else
    ret = &card[PS1_WRAP_PAGE(page)*PS1_PAGE_SIZE];
#endif

    return ret;
}

void __time_critical_func(ps1_mc_data_interface_write_byte)(uint32_t address, uint8_t byte) {
    /* Sulla boot card non si scrive nemmeno in RAM: e' da li' che la console
       legge, ed e' l'unico modo perche' quello che legge sia sempre il file.
       Livello PS1_BOOTCARD_RAM_READONLY, spiegato per esteso nell'header. */
    if (ps1_mc_bootcard_ram_write_denied())
        return;

#if WITH_PSRAM
    card[address%PS1_PAGE_SIZE] = byte;
    ps1_dirty_lockout_renew();
    ps1_dirty_lock();
    psram_write_dma(PS1_WRAP_ADDRESS(address), &card[address%PS1_PAGE_SIZE], 1, NULL);
    psram_wait_for_dma();

    ps1_dirty_unlock();
#else
    card[PS1_WRAP_ADDRESS(address)] = byte;
#endif
    write_occured = true;
}

void __time_critical_func(ps1_mc_data_interface_write_mc)(uint32_t page) {
    /* Sulla boot card le scritture non vengono mai riportate sulla microSD: il
       file conterrebbe un'immagine dell'exploit progressivamente rovinata, e il
       payload smetterebbe di funzionare senza che si capisca perche'.
       Livello PS1_BOOTCARD_SD_READONLY, spiegato per esteso nell'header. */
    if (ps1_mc_bootcard_sd_write_denied())
        return;

    ps1_dirty_mark(page);
}

void __time_critical_func(ps1_mc_data_interface_wait_for_byte)(uint32_t offset) {
#if WITH_PSRAM
    while (dma_in_progress && psram_read_dma_remaining() >= (PS1_PAGE_SIZE - offset)) {};
#endif
}

// Core 0

void ps1_mc_data_interface_card_changed(void) {
#if WITH_PSRAM != 1
    QPRINTF("Card changed\n");

    for (int i = 0; i < 1024; i++) {
        if (ps1_cardman_read_sector(i, &card[i*PS1_PAGE_SIZE]) < 0)
            fatal(ERR_MC_DATA, "Sector %i not read!!!\n", i);
    }
#endif
}

bool ps1_mc_data_interface_write_occured(void) {
    return write_occured;
}

bool ps1_mc_data_interface_read_occured(void) {
    return read_occured;
}

void ps1_mc_data_interface_task(void) {
    write_occured = false;
    read_occured = false;

    ps1_dirty_task();
}

void ps1_mc_data_interface_flush(void) {
    while ( ps1_dirty_activity > 0
    ) {
        ps1_mc_data_interface_task();
    }
}