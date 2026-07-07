/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * hackpad.h - userspace API for the LKSS hackathon button/LED driver
 *
 * The driver exposes a misc character device (/dev/hackpad):
 *
 *  - read()  returns one or more struct hackpad_event (button presses
 *            and releases). Blocking by default, O_NONBLOCK supported,
 *            poll()/select() supported.
 *  - ioctl() controls the LEDs and reads the instantaneous button state.
 */
#ifndef _UAPI_LKSS_HACKPAD_H
#define _UAPI_LKSS_HACKPAD_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Button indexes (bit positions in the state bitmask) */
#define HACKPAD_BTN_SW1   0
#define HACKPAD_BTN_SW2   1
#define HACKPAD_BTN_SW3   2
#define HACKPAD_BTN_SW4   3
#define HACKPAD_NUM_BTNS  4

/* LED indexes (bit positions in the LED bitmask) */
#define HACKPAD_LED_RED   0
#define HACKPAD_LED_GREEN 1
#define HACKPAD_LED_BLUE  2
#define HACKPAD_NUM_LEDS  3

/* One button event, as returned by read() */
struct hackpad_event {
	__u8  button;       /* HACKPAD_BTN_* */
	__u8  pressed;      /* 1 = pressed, 0 = released */
	__u16 reserved;
	__u32 timestamp_ms; /* milliseconds since boot */
};

/* ioctl argument for HACKPAD_IOC_SET_LED */
struct hackpad_led {
	__u8 led;           /* HACKPAD_LED_* */
	__u8 on;            /* 1 = on, 0 = off */
};

#define HACKPAD_IOC_MAGIC     'H'
/* Set one LED */
#define HACKPAD_IOC_SET_LED   _IOW(HACKPAD_IOC_MAGIC, 1, struct hackpad_led)
/* Set all LEDs at once from a bitmask (bit0=red, bit1=green, bit2=blue) */
#define HACKPAD_IOC_SET_LEDS  _IOW(HACKPAD_IOC_MAGIC, 2, __u32)
/* Get the instantaneous button state bitmask (bit set = pressed) */
#define HACKPAD_IOC_GET_BTNS  _IOR(HACKPAD_IOC_MAGIC, 3, __u32)

#endif /* _UAPI_LKSS_HACKPAD_H */
