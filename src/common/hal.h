/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hal.h - LKSS hackathon hardware abstraction layer
 *
 * One call to hal_init() gives every demo:
 *   - an initialized LVGL bound to /dev/fb0 (the st7789fb driver)
 *   - button input from /dev/hackpad, available three ways:
 *       1. as an LVGL keypad indev (hal_keypad()) for widget navigation
 *          SW1 = LV_KEY_PREV, SW2 = LV_KEY_NEXT,
 *          SW3 = LV_KEY_ENTER, SW4 = LV_KEY_ESC
 *       2. as edge events via hal_set_button_cb()
 *       3. as level state via hal_buttons() (bitmask of held buttons)
 *   - LED control: hal_led() / hal_leds()
 *
 * Typical demo skeleton:
 *
 *   int main(void)
 *   {
 *       hal_init();
 *       create_my_ui();
 *       hal_run();          // never returns
 *   }
 */
#ifndef LKSS_HAL_H
#define LKSS_HAL_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "hackpad.h"

/* Called from the LVGL thread whenever a button changes state */
typedef void (*hal_btn_cb_t)(int button, bool pressed);

/* Initialize LVGL, the display and the hackpad. Exits on failure. */
void hal_init(void);

/* Run the LVGL loop forever */
void hal_run(void);

/* Instantaneous button state: bit N set = HACKPAD_BTN_N held down */
uint32_t hal_buttons(void);

/* True exactly once per press of the given button (edge, latched) */
bool hal_button_pressed(int button);

/* Register an event callback (press + release edges) */
void hal_set_button_cb(hal_btn_cb_t cb);

/* LEDs */
void hal_led(int led, bool on);
void hal_leds(uint32_t mask);

/* The LVGL keypad input device (already wired to the default group) */
lv_indev_t *hal_keypad(void);

/* Read the BMP280 via the Lab 4 sysfs attributes.
 * Returns 0 on success, -1 if the sensor is not available. */
int hal_bmp280_read(double *temp_c, double *press_hpa);

#endif /* LKSS_HAL_H */
