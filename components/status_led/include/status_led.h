#pragma once

/*
 * Status LED (Kconfig "Status LED"): addressable RGB LED (WS2812, e.g. GPIO48 on most ESP32-S3
 * N16R8 boards), a plain single-colour LED on a GPIO, or none.
 *
 * Indications, highest priority first:
 *   short events   paired: green 2 s / error: 3 fast red blinks (plain LED: same timing)
 *   PAIRING        Matter commissioning window open: blinking BLUE (0.4 s on / 0.4 s off)
 *   PORTAL         configuration SoftAP open: steady green (plain LED: steady on)
 *   nothing        off
 * All functions are callable from any task (not from an ISR) and never block for long.
 */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_PORTAL = 1u << 0,
    STATUS_LED_PAIRING = 1u << 1,
} status_led_flag_t;

typedef enum {
    STATUS_LED_EVENT_PAIRED = 0, /* the thermostat was added to a home ecosystem */
    STATUS_LED_EVENT_ERROR,      /* an action could not be performed (e.g. no Wi-Fi for pairing) */
} status_led_event_t;

esp_err_t status_led_init(void);
void status_led_set(status_led_flag_t flag, bool on);
void status_led_event(status_led_event_t ev);

#ifdef __cplusplus
}
#endif
