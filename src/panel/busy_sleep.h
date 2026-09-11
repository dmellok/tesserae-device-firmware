/*
 * busy_sleep.h -- light-sleep the SoC while a panel controller is busy.
 *
 * An e-paper refresh is 2-30 s during which the MCU has nothing to do but
 * poll BUSY. Spinning in vTaskDelay keeps the S3 at full clock (tens of mA)
 * for the whole paint, which per wake costs more charge than a day of deep
 * sleep. epd_busy_sleep() instead arms a GPIO level wake on the BUSY pin plus
 * a timer backstop and enters light sleep (low single-digit mA, RAM and
 * peripherals retained), exactly what the official TRMNL build does on the
 * same reTerminal glass.
 *
 * Light sleep is only legal while the radios are down: every esp_wifi_start /
 * esp_wifi_stop and BLE start / stop reports its state here. Drivers call
 * epd_busy_sleep() unconditionally; it degrades to vTaskDelay when not
 * allowed, when the USB-Serial-JTAG console has a host attached (sleep would
 * drop the link), or under -DEPD_NO_LIGHT_SLEEP.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"

/* Gate: light sleep is legal only while no radio is up. wifi_manager and
 * provisioning report the Wi-Fi state, ble_setup the BLE state; the default
 * (nothing reported) is "down", which is the truth at boot. */
void epd_light_sleep_set_wifi(bool up);
void epd_light_sleep_set_ble(bool up);
bool epd_light_sleep_allowed(void);

/* Sleep for up to `ms`, ending early when `pin` reads `ready_level`, and
 * return the REAL milliseconds that passed (esp_timer, which is compensated
 * across light sleep; the FreeRTOS tick is not). Drivers keep their own loop
 * + timeout around it and re-read the pin afterwards, and must add the
 * returned value to their elapsed budget rather than the nominal `ms`: a nap
 * can run to EPD_NAP_MIN_MS (200 ms) when the pin stays busy, so a counter
 * that assumes 10 ms per call would stretch a 60 s cap into 20 minutes. */
uint32_t epd_busy_sleep(gpio_num_t pin, int ready_level, uint32_t ms);

/* Count of light-sleep entries this boot, for the paint log line. */
uint32_t epd_light_sleep_count(void);
