// SPDX-License-Identifier: GPL-2.0
/*
 * st7789fb.c - LKSS Lab 5 (Hackathon): ST7789 SPI display with fbdev support
 *
 * This is the Lab 3 st7789.c driver extended with a Linux framebuffer
 * (fbdev) interface. Userspace sees a standard /dev/fb0 device:
 *
 *   - a shadow framebuffer (240 x 240 x RGB565) lives in system RAM
 *   - applications write() or mmap() /dev/fb0 like regular memory
 *   - the "deferred I/O" framework batches the dirtied pages and calls
 *     our flush function up to 30 times per second, which pushes the
 *     whole shadow buffer to the panel over SPI
 *
 * This is what LVGL's Linux fbdev backend talks to.
 *
 * Same device tree node as Lab 3 (compatible "lkss,st7789"). Unload the
 * Lab 3 module before loading this one - they bind to the same device.
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/fb.h>
#include <linux/of.h>

/* ST7789 command opcodes */
#define ST7789_SLPOUT    0x11
#define ST7789_NORON     0x13
#define ST7789_INVON     0x21
#define ST7789_DISPOFF   0x28
#define ST7789_DISPON    0x29
#define ST7789_CASET     0x2A
#define ST7789_RASET     0x2B
#define ST7789_RAMWR     0x2C
#define ST7789_MADCTL    0x36
#define ST7789_COLMOD    0x3A
#define ST7789_PORCTRL   0xB2
#define ST7789_GCTRL     0xB7
#define ST7789_VCOMS     0xBB
#define ST7789_VDVVRHEN  0xC2
#define ST7789_VRHS      0xC3
#define ST7789_VDVS      0xC4
#define ST7789_VCMOFSET  0xC7
#define ST7789_PWCTRL1   0xD0
#define ST7789_PVGAMCTRL 0xE0
#define ST7789_NVGAMCTRL 0xE1

#define ST7789_COLMOD_RGB565  0x55
#define ST7789_MADCTL_NORMAL  0x00

#define ST7789_WIDTH   240
#define ST7789_HEIGHT  240
#define ST7789_BPP     16
#define ST7789_FB_SIZE (ST7789_WIDTH * ST7789_HEIGHT * ST7789_BPP / 8)

/* Panel refresh rate cap for deferred I/O (frames per second) */
#define ST7789_FPS     30

struct st7789_priv {
	struct spi_device *spi;
	struct gpio_desc  *dc;
	struct gpio_desc  *reset;
	u16                width;
	u16                height;

	/* fbdev */
	struct fb_info    *info;
	u8                *txbuf;	/* byte-swapped copy sent over SPI */
	struct mutex       io_lock;	/* serializes panel updates */
	u32                pseudo_palette[16];
};

/* ---------------- Low-level SPI primitives (from Lab 3) ---------------- */

static int st7789_write_cmd(struct st7789_priv *priv, u8 cmd)
{
	gpiod_set_value(priv->dc, 0);
	return spi_write(priv->spi, &cmd, 1);
}

static int st7789_write_data(struct st7789_priv *priv,
			     const u8 *buf, size_t len)
{
	gpiod_set_value(priv->dc, 1);
	return spi_write(priv->spi, buf, len);
}

static inline int st7789_write_data_byte(struct st7789_priv *priv, u8 byte)
{
	return st7789_write_data(priv, &byte, 1);
}

static void st7789_hw_reset(struct st7789_priv *priv)
{
	gpiod_set_value(priv->reset, 1);	/* assert RESX LOW (active-low) */
	msleep(20);				/* hold > 15 ms */
	gpiod_set_value(priv->reset, 0);	/* deassert RESX HIGH */
	msleep(150);				/* wait > 120 ms before cmds */
}

static int st7789_init_display(struct st7789_priv *priv)
{
	static const u8 porctrl[]   = { 0x05, 0x05, 0x00, 0x33, 0x33 };
	static const u8 vdvvrhen[]  = { 0x01, 0xFF };
	static const u8 pwctrl1[]   = { 0xA4, 0xA1 };
	static const u8 pvgamctrl[] = { 0xD0, 0x05, 0x0A, 0x09, 0x08, 0x05, 0x2E,
					0x44, 0x45, 0x0F, 0x17, 0x16, 0x2B, 0x33 };
	static const u8 nvgamctrl[] = { 0xD0, 0x05, 0x0A, 0x09, 0x08, 0x05, 0x2E,
					0x43, 0x45, 0x0F, 0x16, 0x16, 0x2B, 0x33 };
	int ret;

	ret = st7789_write_cmd(priv, ST7789_SLPOUT);
	if (ret) return ret;
	msleep(600);

	ret = st7789_write_cmd(priv, ST7789_COLMOD);
	if (ret) return ret;
	ret = st7789_write_data_byte(priv, ST7789_COLMOD_RGB565);
	if (ret) return ret;

	ret = st7789_write_cmd(priv, ST7789_PORCTRL);
	if (ret) return ret;
	ret = st7789_write_data(priv, porctrl, sizeof(porctrl));
	if (ret) return ret;

	ret = st7789_write_cmd(priv, ST7789_GCTRL);
	if (ret) return ret;
	ret = st7789_write_data_byte(priv, 0x75);
	if (ret) return ret;

	ret = st7789_write_cmd(priv, ST7789_VDVVRHEN);
	if (ret) return ret;
	ret = st7789_write_data(priv, vdvvrhen, sizeof(vdvvrhen));
	if (ret) return ret;

	ret = st7789_write_cmd(priv, ST7789_VRHS);
	if (ret) return ret;
	ret = st7789_write_data_byte(priv, 0x13);
	if (ret) return ret;

	ret = st7789_write_cmd(priv, ST7789_VDVS);
	if (ret) return ret;
	ret = st7789_write_data_byte(priv, 0x20);
	if (ret) return ret;

	ret = st7789_write_cmd(priv, ST7789_VCOMS);
	if (ret) return ret;
	ret = st7789_write_data_byte(priv, 0x22);
	if (ret) return ret;

	ret = st7789_write_cmd(priv, ST7789_VCMOFSET);
	if (ret) return ret;
	ret = st7789_write_data_byte(priv, 0x20);
	if (ret) return ret;

	ret = st7789_write_cmd(priv, ST7789_PWCTRL1);
	if (ret) return ret;
	ret = st7789_write_data(priv, pwctrl1, sizeof(pwctrl1));
	if (ret) return ret;

	ret = st7789_write_cmd(priv, ST7789_DISPON);
	if (ret) return ret;
	msleep(150);

	ret = st7789_write_cmd(priv, ST7789_INVON);
	if (ret) return ret;

	ret = st7789_write_cmd(priv, ST7789_MADCTL);
	if (ret) return ret;
	ret = st7789_write_data_byte(priv, ST7789_MADCTL_NORMAL);
	if (ret) return ret;

	ret = st7789_write_cmd(priv, ST7789_PVGAMCTRL);
	if (ret) return ret;
	ret = st7789_write_data(priv, pvgamctrl, sizeof(pvgamctrl));
	if (ret) return ret;

	ret = st7789_write_cmd(priv, ST7789_NVGAMCTRL);
	if (ret) return ret;
	ret = st7789_write_data(priv, nvgamctrl, sizeof(nvgamctrl));
	if (ret) return ret;

	return 0;
}

static int st7789_set_addr_win(struct st7789_priv *priv,
			       u16 x0, u16 y0, u16 x1, u16 y1)
{
	u8 col[4] = { x0 >> 8, x0 & 0xff, x1 >> 8, x1 & 0xff };
	u8 row[4] = { y0 >> 8, y0 & 0xff, y1 >> 8, y1 & 0xff };
	int ret;

	ret = st7789_write_cmd(priv, ST7789_CASET);
	if (ret) return ret;
	ret = st7789_write_data(priv, col, 4);
	if (ret) return ret;

	ret = st7789_write_cmd(priv, ST7789_RASET);
	if (ret) return ret;
	ret = st7789_write_data(priv, row, 4);
	if (ret) return ret;

	return st7789_write_cmd(priv, ST7789_RAMWR);
}

/* ------------------------- fbdev support ------------------------------- */

/*
 * Push the whole shadow framebuffer to the panel.
 *
 * The shadow buffer holds native little-endian RGB565 (what userspace
 * and LVGL produce). The ST7789 wants each pixel high-byte first, so we
 * byte-swap into txbuf and send everything in one SPI transfer.
 */
static void st7789fb_update_display(struct st7789_priv *priv)
{
	const u16 *fb = (const u16 *)priv->info->screen_buffer;
	u16 *tx = (u16 *)priv->txbuf;
	int i, ret;

	mutex_lock(&priv->io_lock);

	for (i = 0; i < priv->width * priv->height; i++)
		tx[i] = swab16(fb[i]);

	ret = st7789_set_addr_win(priv, 0, 0,
				  priv->width - 1, priv->height - 1);
	if (!ret)
		ret = st7789_write_data(priv, priv->txbuf, ST7789_FB_SIZE);
	if (ret)
		dev_err_ratelimited(&priv->spi->dev,
				    "panel update failed: %d\n", ret);

	mutex_unlock(&priv->io_lock);
}

/*
 * Deferred I/O: called by the fb core (in a workqueue, process context)
 * at most every 1/ST7789_FPS seconds after userspace touched the mmap'ed
 * framebuffer. We simply repaint the full screen - at 240x240 pixels a
 * full frame is only 115200 bytes, ~15 ms on the wire at 62.5 MHz.
 */
static void st7789fb_deferred_io(struct fb_info *info,
				 struct list_head *pagereflist)
{
	st7789fb_update_display(info->par);
}

/*
 * Damage callbacks: invoked by the generated fb_ops when userspace goes
 * through write()/fillrect/copyarea/imageblit instead of mmap.
 */
static void st7789fb_defio_damage_range(struct fb_info *info,
					off_t off, size_t len)
{
	st7789fb_update_display(info->par);
}

static void st7789fb_defio_damage_area(struct fb_info *info, u32 x, u32 y,
				       u32 width, u32 height)
{
	st7789fb_update_display(info->par);
}

/*
 * Generates st7789fb_defio_read/write/fillrect/copyarea/imageblit on top
 * of the generic system-memory helpers, each followed by a damage call.
 */
FB_GEN_DEFAULT_DEFERRED_SYSMEM_OPS(st7789fb,
				   st7789fb_defio_damage_range,
				   st7789fb_defio_damage_area)

/* Truecolor palette entry for fbcon (16 entries is enough) */
static int st7789fb_setcolreg(unsigned int regno, unsigned int red,
			      unsigned int green, unsigned int blue,
			      unsigned int transp, struct fb_info *info)
{
	u32 *pal = info->pseudo_palette;

	if (regno >= 16)
		return -EINVAL;

	pal[regno] = ((red   >> (16 - info->var.red.length))
			<< info->var.red.offset) |
		     ((green >> (16 - info->var.green.length))
			<< info->var.green.offset) |
		     ((blue  >> (16 - info->var.blue.length))
			<< info->var.blue.offset);
	return 0;
}

/*
 * FBIOBLANK ioctl: userspace (LVGL does this at startup) can turn the
 * panel on/off. Maps straight to the ST7789 DISPON/DISPOFF commands;
 * GRAM is retained, so unblanking restores the picture.
 */
static int st7789fb_blank(int blank_mode, struct fb_info *info)
{
	struct st7789_priv *priv = info->par;
	int ret;

	mutex_lock(&priv->io_lock);
	if (blank_mode == FB_BLANK_UNBLANK)
		ret = st7789_write_cmd(priv, ST7789_DISPON);
	else
		ret = st7789_write_cmd(priv, ST7789_DISPOFF);
	mutex_unlock(&priv->io_lock);

	return ret;
}

static const struct fb_ops st7789fb_ops = {
	.owner		= THIS_MODULE,
	FB_DEFAULT_DEFERRED_OPS(st7789fb),
	.fb_setcolreg	= st7789fb_setcolreg,
	.fb_blank	= st7789fb_blank,
};

static struct fb_deferred_io st7789fb_defio = {
	.delay		= HZ / ST7789_FPS,
	.deferred_io	= st7789fb_deferred_io,
};

static int st7789fb_register(struct st7789_priv *priv)
{
	struct device *dev = &priv->spi->dev;
	struct fb_info *info;
	void *vmem;
	int ret;

	/* The shadow framebuffer userspace reads/writes/mmaps */
	vmem = vzalloc(ST7789_FB_SIZE);
	if (!vmem)
		return -ENOMEM;

	/* The SPI transmit copy (byte-swapped) */
	priv->txbuf = devm_kmalloc(dev, ST7789_FB_SIZE, GFP_KERNEL);
	if (!priv->txbuf) {
		ret = -ENOMEM;
		goto err_free_vmem;
	}

	info = framebuffer_alloc(0, dev);
	if (!info) {
		ret = -ENOMEM;
		goto err_free_vmem;
	}

	info->par = priv;
	priv->info = info;

	info->fbops = &st7789fb_ops;
	info->screen_buffer = vmem;
	info->screen_size = ST7789_FB_SIZE;
	info->pseudo_palette = priv->pseudo_palette;
	info->flags |= FBINFO_VIRTFB;	/* buffer is vmalloc'ed, not MMIO */

	/* Fixed parameters: memory layout */
	strscpy(info->fix.id, "st7789fb", sizeof(info->fix.id));
	info->fix.type        = FB_TYPE_PACKED_PIXELS;
	info->fix.visual      = FB_VISUAL_TRUECOLOR;
	info->fix.line_length = ST7789_WIDTH * ST7789_BPP / 8;
	info->fix.smem_len    = ST7789_FB_SIZE;

	/* Variable parameters: resolution and RGB565 pixel format */
	info->var.xres           = ST7789_WIDTH;
	info->var.yres           = ST7789_HEIGHT;
	info->var.xres_virtual   = ST7789_WIDTH;
	info->var.yres_virtual   = ST7789_HEIGHT;
	info->var.bits_per_pixel = ST7789_BPP;
	info->var.red.offset     = 11;
	info->var.red.length     = 5;
	info->var.green.offset   = 5;
	info->var.green.length   = 6;
	info->var.blue.offset    = 0;
	info->var.blue.length    = 5;
	info->var.activate       = FB_ACTIVATE_NOW;
	info->var.vmode          = FB_VMODE_NONINTERLACED;

	info->fbdefio = &st7789fb_defio;
	fb_deferred_io_init(info);

	ret = register_framebuffer(info);
	if (ret) {
		dev_err(dev, "register_framebuffer failed: %d\n", ret);
		goto err_defio;
	}

	dev_info(dev, "fb%d: %ux%u RGB565 framebuffer on SPI\n",
		 info->node, ST7789_WIDTH, ST7789_HEIGHT);
	return 0;

err_defio:
	fb_deferred_io_cleanup(info);
	framebuffer_release(info);
	priv->info = NULL;
err_free_vmem:
	vfree(vmem);
	return ret;
}

static void st7789fb_unregister(struct st7789_priv *priv)
{
	struct fb_info *info = priv->info;
	void *vmem = info->screen_buffer;

	unregister_framebuffer(info);
	fb_deferred_io_cleanup(info);
	framebuffer_release(info);
	vfree(vmem);
}

/* --------------------------- SPI driver -------------------------------- */

static int st7789fb_probe(struct spi_device *spi)
{
	struct st7789_priv *priv;
	int ret;

	spi->mode = SPI_MODE_0;
	ret = spi_setup(spi);
	if (ret < 0) {
		dev_err(&spi->dev, "spi_setup failed: %d\n", ret);
		return ret;
	}

	priv = devm_kzalloc(&spi->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->spi    = spi;
	priv->width  = ST7789_WIDTH;
	priv->height = ST7789_HEIGHT;
	mutex_init(&priv->io_lock);
	spi_set_drvdata(spi, priv);

	priv->reset = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset))
		return dev_err_probe(&spi->dev, PTR_ERR(priv->reset),
				     "failed to get reset GPIO\n");

	priv->dc = devm_gpiod_get(&spi->dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(priv->dc))
		return dev_err_probe(&spi->dev, PTR_ERR(priv->dc),
				     "failed to get D/C GPIO\n");

	st7789_hw_reset(priv);

	ret = st7789_init_display(priv);
	if (ret) {
		dev_err(&spi->dev, "init failed: %d\n", ret);
		return ret;
	}

	ret = st7789fb_register(priv);
	if (ret)
		return ret;

	/* Push the initial (black) frame so the panel starts clean */
	st7789fb_update_display(priv);

	dev_info(&spi->dev, "ST7789 fbdev ready\n");
	return 0;
}

static void st7789fb_remove(struct spi_device *spi)
{
	struct st7789_priv *priv = spi_get_drvdata(spi);

	st7789fb_unregister(priv);
	st7789_write_cmd(priv, ST7789_DISPOFF);
	dev_info(&spi->dev, "ST7789 fbdev removed\n");
}

static const struct of_device_id st7789fb_of_match[] = {
	{ .compatible = "lkss,st7789" },
	{ }
};
MODULE_DEVICE_TABLE(of, st7789fb_of_match);

static const struct spi_device_id st7789fb_spi_ids[] = {
	{ "st7789", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, st7789fb_spi_ids);

static struct spi_driver st7789fb_driver = {
	.driver = {
		.name           = "st7789fb",
		.of_match_table = st7789fb_of_match,
	},
	.probe    = st7789fb_probe,
	.remove   = st7789fb_remove,
	.id_table = st7789fb_spi_ids,
};
module_spi_driver(st7789fb_driver);

MODULE_AUTHOR("LKSS Lab Team");
MODULE_DESCRIPTION("LKSS Lab 5: ST7789 SPI display driver with fbdev");
MODULE_LICENSE("GPL v2");
