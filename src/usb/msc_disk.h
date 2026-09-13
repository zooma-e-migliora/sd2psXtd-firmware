#pragma once

#include <stdbool.h>

/* Abilita/disabilita l'unita' MSC. Quando e' disabilitata l'host vede
   "medium not present": serve a garantire che il passthrough e l'emulazione
   carta non tocchino mai la SD nello stesso momento. */
void msc_disk_set_enabled(bool enabled);
bool msc_disk_is_enabled(void);

/* True (e azzera il flag) se c'e' stata almeno una scrittura dall'ultima
   chiamata: usato per far lampeggiare il LED. */
bool msc_disk_write_occurred(void);
bool msc_disk_read_occurred(void);

/* True dopo che l'host ha espulso il volume. */
bool msc_disk_host_ejected(void);
