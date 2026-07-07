// SPDX-License-Identifier: GPL-2.0
/*
 * hal.c - LKSS hackathon hardware abstraction layer
 *
 * Display : LVGL Linux fbdev backend on /dev/fb0 (st7789fb.ko)
 * Input   : /dev/hackpad button events (hackpad.ko)
 * LEDs    : /dev/hackpad ioctls
 * Sensor  : BMP280 sysfs attributes from the Lab 4 driver
 */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "hal.h"

#define FBDEV_PATH   "/dev/fb0"
#define HACKPAD_PATH "/dev/hackpad"

/* Ring buffer of keypad events handed to the LVGL indev */
#define KEYQ_SIZE 32
struct key_ev {
	uint32_t key;
	bool pressed;
};

static int hackpad_fd = -1;
static lv_indev_t *keypad_indev;
static hal_btn_cb_t user_btn_cb;

static uint32_t btn_state;	/* level: currently held buttons */
static uint32_t btn_edges;	/* latched press edges */

static struct key_ev keyq[KEYQ_SIZE];
static int keyq_head, keyq_tail;

/* SW1..SW4 -> LVGL navigation keys */
static const uint32_t btn2key[HACKPAD_NUM_BTNS] = {
	LV_KEY_PREV,	/* SW1 */
	LV_KEY_NEXT,	/* SW2 */
	LV_KEY_ENTER,	/* SW3 */
	LV_KEY_ESC,	/* SW4 */
};

static uint32_t hal_tick_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Drain all pending events from /dev/hackpad (non-blocking) */
static void hal_drain_events(void)
{
	struct hackpad_event ev;

	if (hackpad_fd < 0)
		return;

	while (read(hackpad_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (ev.button >= HACKPAD_NUM_BTNS)
			continue;

		if (ev.pressed) {
			btn_state |= 1u << ev.button;
			btn_edges |= 1u << ev.button;
		} else {
			btn_state &= ~(1u << ev.button);
		}

		/* Queue for the LVGL keypad indev */
		int next = (keyq_head + 1) % KEYQ_SIZE;
		if (next != keyq_tail) {
			keyq[keyq_head].key = btn2key[ev.button];
			keyq[keyq_head].pressed = ev.pressed;
			keyq_head = next;
		}

		if (user_btn_cb)
			user_btn_cb(ev.button, ev.pressed);
	}
}

static void hal_keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
	LV_UNUSED(indev);

	hal_drain_events();

	if (keyq_tail != keyq_head) {
		data->key = keyq[keyq_tail].key;
		data->state = keyq[keyq_tail].pressed ?
			LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
		keyq_tail = (keyq_tail + 1) % KEYQ_SIZE;
		data->continue_reading = (keyq_tail != keyq_head);
	} else {
		data->state = LV_INDEV_STATE_RELEASED;
	}
}

void hal_init(void)
{
	lv_display_t *disp;
	lv_group_t *group;

	lv_init();
	lv_tick_set_cb(hal_tick_ms);

	disp = lv_linux_fbdev_create();
	if (!disp) {
		fprintf(stderr, "hal: cannot create fbdev display\n");
		exit(1);
	}
	lv_linux_fbdev_set_file(disp, FBDEV_PATH);

	/* Buttons/LEDs are optional: display-only apps still work without
	 * the hackpad module - the helpers just become no-ops. */
	hackpad_fd = open(HACKPAD_PATH, O_RDWR | O_NONBLOCK);
	if (hackpad_fd < 0)
		fprintf(stderr,
			"hal: %s not available (%s) - no buttons/LEDs; did you 'modprobe hackpad'?\n",
			HACKPAD_PATH, strerror(errno));

	keypad_indev = lv_indev_create();
	lv_indev_set_type(keypad_indev, LV_INDEV_TYPE_KEYPAD);
	lv_indev_set_read_cb(keypad_indev, hal_keypad_read);

	group = lv_group_create();
	lv_group_set_default(group);
	lv_indev_set_group(keypad_indev, group);

	hal_leds(0);
}

void hal_run(void)
{
	while (1) {
		uint32_t sleep_ms = lv_timer_handler();

		if (sleep_ms == LV_NO_TIMER_READY || sleep_ms > 20)
			sleep_ms = 20;
		usleep(sleep_ms * 1000);
	}
}

uint32_t hal_buttons(void)
{
	uint32_t state = 0;

	if (hackpad_fd < 0 ||
	    ioctl(hackpad_fd, HACKPAD_IOC_GET_BTNS, &state) < 0)
		return btn_state;	/* fall back to event-tracked state */
	return state;
}

bool hal_button_pressed(int button)
{
	uint32_t bit = 1u << button;

	hal_drain_events();
	if (btn_edges & bit) {
		btn_edges &= ~bit;
		return true;
	}
	return false;
}

void hal_set_button_cb(hal_btn_cb_t cb)
{
	user_btn_cb = cb;
}

void hal_led(int led, bool on)
{
	struct hackpad_led arg = { .led = led, .on = on };

	if (hackpad_fd >= 0)
		ioctl(hackpad_fd, HACKPAD_IOC_SET_LED, &arg);
}

void hal_leds(uint32_t mask)
{
	if (hackpad_fd >= 0)
		ioctl(hackpad_fd, HACKPAD_IOC_SET_LEDS, &mask);
}

lv_indev_t *hal_keypad(void)
{
	return keypad_indev;
}

/* ---------------------------- BMP280 ----------------------------------- */

static int read_sysfs_double(const char *pattern, double *out)
{
	glob_t g;
	FILE *f;
	int ret = -1;

	if (glob(pattern, 0, NULL, &g) != 0 || g.gl_pathc == 0) {
		globfree(&g);
		return -1;
	}

	f = fopen(g.gl_pathv[0], "r");
	if (f) {
		if (fscanf(f, "%lf", out) == 1)
			ret = 0;
		fclose(f);
	}
	globfree(&g);
	return ret;
}

int hal_bmp280_read(double *temp_c, double *press_hpa)
{
	if (temp_c &&
	    read_sysfs_double("/sys/bus/i2c/devices/*-0076/temperature",
			      temp_c) < 0)
		return -1;
	if (press_hpa &&
	    read_sysfs_double("/sys/bus/i2c/devices/*-0076/pressure",
			      press_hpa) < 0)
		return -1;
	return 0;
}
