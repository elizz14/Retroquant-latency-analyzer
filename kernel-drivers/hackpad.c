// SPDX-License-Identifier: GPL-2.0
/*
 * hackpad.c - LKSS Lab 5 (Hackathon): buttons + LEDs framework driver
 *
 * Exposes the 4 push buttons and 3 user LEDs of the LKSS daughter board
 * through a single misc character device, /dev/hackpad:
 *
 *   - read()  blocks until a button is pressed or released and returns
 *             struct hackpad_event records (see hackpad.h)
 *   - poll()  wakes up when an event is pending
 *   - ioctl() HACKPAD_IOC_SET_LED / SET_LEDS / GET_BTNS
 *   - write() convenience text interface: "rgb" bitmask, e.g.
 *             echo 101 > /dev/hackpad  (red on, green off, blue on)
 *
 * Device tree binding (compatible "lkss,hackpad"):
 *
 *   hackpad {
 *       compatible  = "lkss,hackpad";
 *       button-gpios = <&gpio2 17 GPIO_ACTIVE_LOW>,   // SW1
 *                      <&gpio2 18 GPIO_ACTIVE_LOW>,   // SW2
 *                      <&gpio2 27 GPIO_ACTIVE_LOW>,   // SW3
 *                      <&gpio2 22 GPIO_ACTIVE_LOW>;   // SW4
 *       led-gpios    = <&gpio2  4 GPIO_ACTIVE_HIGH>,  // red
 *                      <&gpio2 14 GPIO_ACTIVE_HIGH>,  // green
 *                      <&gpio2 15 GPIO_ACTIVE_HIGH>;  // blue
 *   };
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/kfifo.h>
#include <linux/poll.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/of.h>

#include "hackpad.h"

#define HACKPAD_DEBOUNCE_MS	30
#define HACKPAD_FIFO_SIZE	64	/* events, must be power of 2 */

struct hackpad_button {
	struct hackpad_priv *priv;
	struct gpio_desc *gpiod;
	int irq;
	int index;
	unsigned long last_jiffies;	/* debounce timestamp */
	int last_state;
};

struct hackpad_priv {
	struct device *dev;
	struct miscdevice miscdev;
	struct hackpad_button buttons[HACKPAD_NUM_BTNS];
	struct gpio_desc *leds[HACKPAD_NUM_LEDS];
	DECLARE_KFIFO(events, struct hackpad_event, HACKPAD_FIFO_SIZE);
	spinlock_t fifo_lock;		/* protects the event fifo */
	wait_queue_head_t waitq;
};

static struct hackpad_priv *to_hackpad(struct file *file)
{
	struct miscdevice *misc = file->private_data;

	return container_of(misc, struct hackpad_priv, miscdev);
}

/*
 * Interrupt handler: one per button, fires on both edges. Debounce by
 * ignoring edges that arrive less than HACKPAD_DEBOUNCE_MS after the
 * previous accepted one, then queue an event and wake up readers.
 */
static irqreturn_t hackpad_irq_handler(int irq, void *data)
{
	struct hackpad_button *btn = data;
	struct hackpad_priv *priv = btn->priv;
	struct hackpad_event ev;
	unsigned long flags;
	int state;

	if (time_before(jiffies, btn->last_jiffies +
			msecs_to_jiffies(HACKPAD_DEBOUNCE_MS)))
		return IRQ_HANDLED;
	btn->last_jiffies = jiffies;

	state = gpiod_get_value(btn->gpiod);	/* 1 = pressed (active-low) */
	if (state == btn->last_state)
		return IRQ_HANDLED;
	btn->last_state = state;

	ev.button = btn->index;
	ev.pressed = state;
	ev.reserved = 0;
	ev.timestamp_ms = jiffies_to_msecs(jiffies);

	spin_lock_irqsave(&priv->fifo_lock, flags);
	/* If the fifo is full, drop the oldest event to make room */
	if (kfifo_is_full(&priv->events))
		kfifo_skip(&priv->events);
	kfifo_put(&priv->events, ev);
	spin_unlock_irqrestore(&priv->fifo_lock, flags);

	wake_up_interruptible(&priv->waitq);

	return IRQ_HANDLED;
}

static ssize_t hackpad_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct hackpad_priv *priv = to_hackpad(file);
	struct hackpad_event ev;
	unsigned long flags;
	size_t copied = 0;
	int ret;

	if (count < sizeof(ev))
		return -EINVAL;

	while (copied == 0) {
		if (kfifo_is_empty(&priv->events)) {
			if (file->f_flags & O_NONBLOCK)
				return -EAGAIN;
			ret = wait_event_interruptible(priv->waitq,
					!kfifo_is_empty(&priv->events));
			if (ret)
				return ret;
		}

		while (copied + sizeof(ev) <= count) {
			int got;

			spin_lock_irqsave(&priv->fifo_lock, flags);
			got = kfifo_get(&priv->events, &ev);
			spin_unlock_irqrestore(&priv->fifo_lock, flags);
			if (!got)
				break;

			if (copy_to_user(buf + copied, &ev, sizeof(ev)))
				return copied ? copied : -EFAULT;
			copied += sizeof(ev);
		}
	}

	return copied;
}

static __poll_t hackpad_poll(struct file *file, poll_table *wait)
{
	struct hackpad_priv *priv = to_hackpad(file);
	__poll_t mask = 0;

	poll_wait(file, &priv->waitq, wait);
	if (!kfifo_is_empty(&priv->events))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static void hackpad_set_leds_mask(struct hackpad_priv *priv, u32 mask)
{
	int i;

	for (i = 0; i < HACKPAD_NUM_LEDS; i++)
		gpiod_set_value(priv->leds[i], !!(mask & BIT(i)));
}

static long hackpad_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct hackpad_priv *priv = to_hackpad(file);
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case HACKPAD_IOC_SET_LED: {
		struct hackpad_led led;

		if (copy_from_user(&led, argp, sizeof(led)))
			return -EFAULT;
		if (led.led >= HACKPAD_NUM_LEDS)
			return -EINVAL;
		gpiod_set_value(priv->leds[led.led], !!led.on);
		return 0;
	}
	case HACKPAD_IOC_SET_LEDS: {
		u32 mask;

		if (copy_from_user(&mask, argp, sizeof(mask)))
			return -EFAULT;
		hackpad_set_leds_mask(priv, mask);
		return 0;
	}
	case HACKPAD_IOC_GET_BTNS: {
		u32 state = 0;
		int i;

		for (i = 0; i < HACKPAD_NUM_BTNS; i++)
			if (gpiod_get_value(priv->buttons[i].gpiod))
				state |= BIT(i);
		if (copy_to_user(argp, &state, sizeof(state)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

/*
 * Text helper so the LEDs can be driven from the shell:
 *   echo 110 > /dev/hackpad   -> red on, green on, blue off
 * Exactly HACKPAD_NUM_LEDS '0'/'1' characters, red first.
 */
static ssize_t hackpad_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct hackpad_priv *priv = to_hackpad(file);
	char kbuf[HACKPAD_NUM_LEDS + 2];	/* digits + \n + NUL */
	u32 mask = 0;
	int i;

	if (count == 0 || count > sizeof(kbuf) - 1)
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	for (i = 0; i < HACKPAD_NUM_LEDS; i++) {
		if (kbuf[i] == '1')
			mask |= BIT(i);
		else if (kbuf[i] != '0')
			return -EINVAL;
	}
	hackpad_set_leds_mask(priv, mask);

	return count;
}

static const struct file_operations hackpad_fops = {
	.owner		= THIS_MODULE,
	.read		= hackpad_read,
	.write		= hackpad_write,
	.poll		= hackpad_poll,
	.unlocked_ioctl	= hackpad_ioctl,
};

static int hackpad_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hackpad_priv *priv;
	int i, ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	INIT_KFIFO(priv->events);
	spin_lock_init(&priv->fifo_lock);
	init_waitqueue_head(&priv->waitq);
	platform_set_drvdata(pdev, priv);

	for (i = 0; i < HACKPAD_NUM_LEDS; i++) {
		priv->leds[i] = devm_gpiod_get_index(dev, "led", i,
						     GPIOD_OUT_LOW);
		if (IS_ERR(priv->leds[i]))
			return dev_err_probe(dev, PTR_ERR(priv->leds[i]),
					     "failed to get LED %d\n", i);
	}

	for (i = 0; i < HACKPAD_NUM_BTNS; i++) {
		struct hackpad_button *btn = &priv->buttons[i];

		btn->priv = priv;
		btn->index = i;
		btn->gpiod = devm_gpiod_get_index(dev, "button", i, GPIOD_IN);
		if (IS_ERR(btn->gpiod))
			return dev_err_probe(dev, PTR_ERR(btn->gpiod),
					     "failed to get button %d\n", i);

		btn->irq = gpiod_to_irq(btn->gpiod);
		if (btn->irq < 0)
			return dev_err_probe(dev, btn->irq,
					     "no IRQ for button %d\n", i);

		btn->last_state = gpiod_get_value(btn->gpiod);

		ret = devm_request_irq(dev, btn->irq, hackpad_irq_handler,
				       IRQF_TRIGGER_RISING |
				       IRQF_TRIGGER_FALLING,
				       "hackpad-button", btn);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to request IRQ %d\n",
					     btn->irq);
	}

	priv->miscdev.minor = MISC_DYNAMIC_MINOR;
	priv->miscdev.name = "hackpad";
	priv->miscdev.fops = &hackpad_fops;
	priv->miscdev.parent = dev;

	ret = misc_register(&priv->miscdev);
	if (ret)
		return dev_err_probe(dev, ret, "misc_register failed\n");

	dev_info(dev, "hackpad ready: %d buttons, %d LEDs -> /dev/hackpad\n",
		 HACKPAD_NUM_BTNS, HACKPAD_NUM_LEDS);
	return 0;
}

static void hackpad_remove(struct platform_device *pdev)
{
	struct hackpad_priv *priv = platform_get_drvdata(pdev);

	hackpad_set_leds_mask(priv, 0);
	misc_deregister(&priv->miscdev);
	dev_info(&pdev->dev, "hackpad removed\n");
}

static const struct of_device_id hackpad_of_match[] = {
	{ .compatible = "lkss,hackpad" },
	{ }
};
MODULE_DEVICE_TABLE(of, hackpad_of_match);

static struct platform_driver hackpad_driver = {
	.probe	= hackpad_probe,
	.remove	= hackpad_remove,
	.driver	= {
		.name		= "lkss-hackpad",
		.of_match_table	= hackpad_of_match,
	},
};
module_platform_driver(hackpad_driver);

MODULE_AUTHOR("LKSS Lab Team");
MODULE_DESCRIPTION("LKSS Lab 5: hackathon buttons/LEDs driver");
MODULE_LICENSE("GPL v2");
