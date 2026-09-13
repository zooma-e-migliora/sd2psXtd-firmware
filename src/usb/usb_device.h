#pragma once

#include <stdbool.h>

/* Avvia lo stack USB (CDC + MSC) e collega il driver stdio alla CDC.
   Idempotente: se lo stack e' gia' partito riattiva solo la connessione. */
void usb_device_init(void);

/* Stacca il device dal bus e disabilita il driver stdio. */
void usb_device_deinit(void);

/* Da chiamare periodicamente dal main loop: fa avanzare TinyUSB. */
void usb_device_task(void);

bool usb_device_mounted(void);
bool usb_device_cdc_connected(void);
