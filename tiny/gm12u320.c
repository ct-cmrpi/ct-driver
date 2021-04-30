// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2019 Hans de Goede <hdegoede@redhat.com>
 */

#include <linux/dma-buf.h>
#include <linux/module.h>
#include <linux/usb.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_file.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

static bool eco_mode;
module_param(eco_mode, bool, 0644);
MODULE_PARM_DESC(eco_mode, "Turn on Eco mode (less bright, more silent)");

#define DRIVER_NAME		"gm12u320"
#define DRIVER_DESC		"Grain Media GM12U320 USB projector display"
#define DRIVER_DATE		"2019"
#define DRIVER_MAJOR		1
#define DRIVER_MINOR		0

/*
 * The DLP has an actual width of 854 pixels, but that is not a multiple
 * of 8, breaking things left and right, so we export a width of 848.
 */
#define GM12U320_USER_WIDTH		848
#define GM12U320_REAL_WIDTH		854
#define GM12U320_HEIGHT			480

#define GM12U320_BLOCK_COUNT		20

#define GM12U320_ERR(fmt, ...) \
	DRM_DEV_ERROR(&gm12u320->udev->dev, fmt, ##__VA_ARGS__)

#define MISC_RCV_EPT			1
#define DATA_RCV_EPT			2
#define DATA_SND_EPT			3
#define MISC_SND_EPT			4

#define DATA_BLOCK_HEADER_SIZE		84
#define DATA_BLOCK_CONTENT_SIZE		64512
#define DATA_BLOCK_FOOTER_SIZE		20
#define DATA_BLOCK_SIZE			(DATA_BLOCK_HEADER_SIZE + \
					 DATA_BLOCK_CONTENT_SIZE + \
					 DATA_BLOCK_FOOTER_SIZE)
#define DATA_LAST_BLOCK_CONTENT_SIZE	4032
#define DATA_LAST_BLOCK_SIZE		(DATA_BLOCK_HEADER_SIZE + \
					 DATA_LAST_BLOCK_CONTENT_SIZE + \
					 DATA_BLOCK_FOOTER_SIZE)

#define CMD_SIZE			31
#define READ_STATUS_SIZE		13
#define MISC_VALUE_SIZE			4

#define CMD_TIMEOUT			msecs_to_jiffies(200)
#define DATA_TIMEOUT			msecs_to_jiffies(1000)
#define IDLE_TIMEOUT			msecs_to_jiffies(2000)
#define FIRST_FRAME_TIMEOUT		msecs_to_jiffies(2000)

#define MISC_REQ_GET_SET_ECO_A		0xff
#define MISC_REQ_GET_SET_ECO_B		0x35
/* Windows driver does once every second, with arg d = 1, other args 0 */
#define MISC_REQ_UNKNOWN1_A		0xff
#define MISC_REQ_UNKNOWN1_B		0x38
/* Windows driver does this on init, with arg a, b = 0, c = 0xa0, d = 4 */
#define MISC_REQ_UNKNOWN2_A		0xa5
#define MISC_REQ_UNKNOWN2_B		0x00

struct gm12u320_device {
	struct drm_device	         dev;
	struct drm_simple_display_pipe   pipe;
	struct drm_connector	         conn;
	struct usb_device               *udev;
	unsigned char                   *cmd_buf;
	unsigned char                   *data_buf[GM12U320_BLOCK_COUNT];
	bool                             pipe_enabled;
	struct {
		bool                     run;
		struct workqueue_struct *workq;
		struct work_struct       work;
		wait_queue_head_t        waitq;
		struct mutex             lock;
		struct drm_framebuffer  *fb;
		struct drm_rect          rect;
	} fb_update;
};

static const char cmd_data[CMD_SIZE] = {
	0x55, 0x53, 0x42, 0x43, 0x00, 0x00, 0x00, 0x00,
	0x68, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x10, 0xff,
	0x00, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x80, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const char cmd_draw[CMD_SIZE] = {
	0x55, 0x53, 0x42, 0x43, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0xfe,
	0x00, 0x00, 0x00, 0xc0, 0xd1, 0x05, 0x00, 0x40,
	0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const char cmd_misc[CMD_SIZE] = {
	0x55, 0x53, 0x42, 0x43, 0x00, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x80, 0x01, 0x10, 0xfd,
	0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const char data_block_header[DATA_BLOCK_HEADER_SIZE] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xfb, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x04, 0x15, 0x00, 0x00, 0xfc, 0x00, 0x00,
	0x01, 0x00, 0x00, 0xdb
};

static const char data_last_block_header[DATA_BLOCK_HEADER_SIZE] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xfb, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x2a, 0x00, 0x20, 0x00, 0xc0, 0x0f, 0x00, 0x00,
	0x01, 0x00, 0x00, 0xd7
};

static const char data_block_footer[DATA_BLOCK_FOOTER_SIZE] = {
	0xfb, 0x14, 0x02, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x80, 0x00, 0x00, 0x4f
};

static int gm12u320_usb_alloc(struct gm12u320_device *gm12u320)
{
	int i, block_size;
	const char *hdr;

	gm12u320->cmd_buf = kmalloc(CMD_SIZE, GFP_KERNEL);
	if (!gm12u320->cmd_buf)
		return -ENOMEM;

	for (i = 0; i < GM12U320_BLOCK_COUNT; i++) {
		if (i == GM12U320_BLOCK_COUNT - 1) {
			block_size = DATA_LAST_BLOCK_SIZE;
			hdr = data_last_block_header;
		} else {
			block_size = DATA_BLOCK_SIZE;
			hdr = data_block_header;
		}

		gm12u320->data_buf[i] = kzalloc(block_size, GFP_KERNEL);
		if (!gm12u320->data_buf[i])
			return -ENOMEM;

		memcpy(gm12u320->data_buf[i], hdr, DATA_BLOCK_HEADER_SIZE);
		memcpy(gm12u320->data_buf[i] +
				(block_size - DATA_BLOCK_FOOTER_SIZE),
		       data_block_footer, DATA_BLOCK_FOOTER_SIZE);
	}

	gm12u320->fb_update.workq = create_singlethread_workqueue(DRIVER_NAME);
	if (!gm12u320->fb_update.workq)
		return -ENOMEM;

	return 0;
}

static void gm12u320_usb_free(struct gm12u320_device *gm12u320)
{
	int i;

	if (gm12u320->fb_update.workq)
		destroy_workqueue(gm12u320->fb_update.workq);

	for (i = 0; i < GM12U320_BLOCK_COUNT; i++)
		kfree(gm12u320->data_buf[i]);

	kfree(gm12u320->cmd_buf);
}

static int gm12u320_misc_request(struct gm12u320_device *gm12u320,
				 u8 req_a, u8 req_b,
				 u8 arg_a, u8 arg_b, u8 arg_c, u8 arg_d)
{
	int ret, len;

	memcpy(gm12u320->cmd_buf, &cmd_misc, CMD_SIZE);
	gm12u320->cmd_buf[20] = req_a;
	gm12u320->cmd_buf[21] = req_b;
	gm12u320->cmd_buf[22] = arg_a;
	gm12u320->cmd_buf[23] = arg_b;
	gm12u320->cmd_buf[24] = arg_c;
	gm12u320->cmd_buf[25] = arg_d;

	/* Send request */
	ret = usb_bulk_msg(gm12u320->udev,
			   usb_sndbulkpipe(gm12u320->udev, MISC_SND_EPT),
			   gm12u320->cmd_buf, CMD_SIZE, &len, CMD_TIMEOUT);
	if (ret || len != CMD_SIZE) {
		GM12U320_ERR("Misc. req. error %d\n", ret);
		return -EIO;
	}

	/* Read value */
	ret = usb_bulk_msg(gm12u320->udev,
			   usb_rcvbulkpipe(gm12u320->udev, MISC_RCV_EPT),
			   gm12u320->cmd_buf, MISC_VALUE_SIZE, &len,
			   DATA_TIMEOUT);
	if (ret || len != MISC_VALUE_SIZE) {
		GM12U320_ERR("Misc. value error %d\n", ret);
		return -EIO;
	}
	/* cmd_buf[0] now contains the read value, which we don't use */

	/* Read status */
	ret = usb_bulk_msg(gm12u320->udev,
			   usb_rcvbulkpipe(gm12u320->udev, MISC_RCV_EPT),
			   gm12u320->cmd_buf, READ_STATUS_SIZE, &len,
			   CMD_TIMEOUT);
	if (ret || len != READ_STATUS_SIZE) {
		GM12U320_ERR("Misc. status error %d\n", ret);
		return -EIO;
	}

	return 0;
}

static void gm12u320_32bpp_to_24bpp_packed(u8 *dst, u8 *src, int len)
{
	while (len--) {
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		src++;
	}
}

static void gm12u320_copy_fb_to_blocks(struct gm12u320_device *gm12u320)
{
	int block, dst_offset, len, remain, ret, x1, x2, y1, y2;
	struct drm_framebuffer *fb;
	void *vaddr;
	u8 *src;

	mutex_lock(&gm12u320->fb_update.lock);

	if (!gm12u320->fb_update.fb)
		goto unlock;

	fb = gm12u320->fb_update.fb;
	x1 = gm12u320->fb_update.rect.x1;
	x2 = gm12u320->fb_update.rect.x2;
	y1 = gm12u320->fb_update.rect.y1;
	y2 = gm12u320->fb_update.rect.y2;

	vaddr = drm_gem_shmem_vmap(fb->obj[0]);
	if (IS_ERR(vaddr)) {
		GM12U320_ERR("failed to vmap fb: %ld\n", PTR_ERR(vaddr));
		goto put_fb;
	}

	if (fb->obj[0]->import_attach) {
		ret = dma_buf_begin_cpu_access(
			fb->obj[0]->import_attach->dmabuf, DMA_FROM_DEVICE);
		if (ret) {
			GM12U320_ERR("dma_buf_begin_cpu_access err: %d\n", ret);
			goto vunmap;
		}
	}

	src = vaddr + y1 * fb->pitches[0] + x1 * 4;

	x1 += (GM12U320_REAL_WIDTH - GM12U320_USER_WIDTH) / 2;
	x2 += (GM12U320_REAL_WIDTH - GM12U320_USER_WIDTH) / 2;

	for (; y1 < y2; y1++) {
		remain = 0;
		len = (x2 - x1) * 3;
		dst_offset = (y1 * GM12U320_REAL_WIDTH + x1) * 3;
		block = dst_offset / DATA_BLOCK_CONTENT_SIZE;
		dst_offset %= DATA_BLOCK_CONTENT_SIZE;

		if ((dst_offset + len) > DATA_BLOCK_CONTENT_SIZE) {
			remain = dst_offset + len - DATA_BLOCK_CONTENT_SIZE;
			len = DATA_BLOCK_CONTENT_SIZE - dst_offset;
		}

		dst_offset += DATA_BLOCK_HEADER_SIZE;
		len /= 3;

		gm12u320_32bpp_to_24bpp_packed(
			gm12u320->data_buf[block] + dst_offset,
			src, len);

		if (remain) {
			block++;
			dst_offset = DATA_BLOCK_HEADER_SIZE;
			gm12u320_32bpp_to_24bpp_packed(
				gm12u320->data_buf[block] + dst_offset,
				src + len * 4, remain / 3);
		}
		src += fb->pitches[0];
	}

	if (fb->obj[0]->import_attach) {
		ret = dma_buf_end_cpu_access(fb->obj[0]->import_attach->dmabuf,
					     DMA_FROM_DEVICE);
		if (ret)
			GM12U320_ERR("dma_buf_end_cpu_access err: %d\n", ret);
	}
vunmap:
	drm_gem_shmem_vunmap(fb->obj[0], vaddr);
put_fb:
	drm_framebuffer_put(fb);
	gm12u320->fb_update.fb = NULL;
unlock:
	mutex_unlock(&gm12u320->fb_update.lock);
}

static void gm12u320_fb_update_work(struct work_struct *work)
{
	struct gm12u320_device *gm12u320 =
		container_of(work, struct gm12u320_device, fb_update.work);
	int draw_status_timeout = FIRST_FRAME_TIMEOUT;
	int block, block_size, len;
	int frame = 0;
	int ret = 0;

	while (gm12u320->fb_update.run) {
		gm12u320_copy_fb_to_blocks(gm12u320);

		for (block = 0; block < GM12U320_BLOCK_COUNT; block++) {
			if (block == GM12U320_BLOCK_COUNT - 1)
				block_size = DATA_LAST_BLOCK_SIZE;
			else
				block_size = DATA_BLOCK_SIZE;

			/* Send data command to device */
			memcpy(gm12u320->cmd_buf, cmd_data, CMD_SIZE);
			gm12u320->cmd_buf[8] = block_size & 0xff;
			gm12u320->cmd_buf[9] = block_size >> 8;
			gm12u320->cmd_buf[20] = 0xfc - block * 4;
			gm12u320->cmd_buf[21] = block | (frame << 7);

			ret = usb_bulk_msg(gm12u320->udev,
				usb_sndbulkpipe(gm12u320->udev, DATA_SND_EPT),
				gm12u320->cmd_buf, CMD_SIZE, &len,
				CMD_TIMEOUT);
			if (ret || len != CMD_SIZE)
				goto err;

			/* Send data block to device */
			ret = usb_bulk_msg(gm12u320->udev,
				usb_sndbulkpipe(gm12u320->udev, DATA_SND_EPT),
				gm12u320->data_buf[block], block_size,
				&len, DATA_TIMEOUT);
			if (ret || len != block_size)
				goto err;

			/* Read status */
			ret = usb_bulk_msg(gm12u320->udev,
				usb_rcvbulkpipe(gm12u320->udev, DATA_RCV_EPT),
				gm12u320->cmd_buf, READ_STATUS_SIZE, &len,
				CMD_TIMEOUT);
			if (ret || len != READ_STATUS_SIZE)
				goto err;
		}

		/* Send draw command to device */
		memcpy(gm12u320->cmd_buf, cmd_draw, CMD_SIZE);
		ret = usb_bulk_msg(gm12u320->udev,
			usb_sndbulkpipe(gm12u320->udev, DATA_SND_EPT),
			gm12u320->cmd_buf, CMD_SIZE, &len, CMD_TIMEOUT);
		if (ret || len != CMD_SIZE)
			goto err;

		/* Read status */
		ret = usb_bulk_msg(gm12u320->udev,
			usb_rcvbulkpipe(gm12u320->udev, DATA_RCV_EPT),
			gm12u320->cmd_buf, READ_STATUS_SIZE, &len,
			draw_status_timeout);
		if (ret || len != READ_STATUS_SIZE)
			goto err;

		draw_status_timeout = CMD_TIMEOUT;
		frame = !frame;

		/*
		 * We must draw a frame every 2s otherwise the projector
		 * switches back to showing its logo.
		 */
		wait_event_timeout(gm12u320->fb_update.waitq,
				   !gm12u320->fb_update.run ||
					gm12u320->fb_update.fb != NULL,
				   IDLE_TIMEOUT);
	}
	return;
err:
	/* Do not log errors caused by module unload or device unplug */
	if (ret != -ENODEV && ret != -ECONNRESET && ret != -ESHUTDOWN)
		GM12U320_ERR("Frame update error: %d\n", ret);
}

static void gm12u320_fb_mark_dirty(struct drm_framebuffer *fb,
				   struct drm_rect *dirty)
{
	struct gm12u320_device *gm12u320 = fb->dev->dev_private;
	struct drm_framebuffer *old_fb = NULL;
	bool wakeup = false;

	mutex_lock(&gm12u320->fb_update.lock);

	if (gm12u320->fb_update.fb != fb) {
		old_fb = gm12u320->fb_update.fb;
		drm_framebuffer_get(fb);
		gm12u320->fb_update.fb = fb;
		gm12u320->fb_update.rect = *dirty;
		wakeup = true;
	} else {
		struct drm_rect *rect = &gm12u320->fb_update.rect;

		rect->x1 = min(rect->x1, dirty->x1);
		rect->y1 = min(rect->y1, dirty->y1);
		rect->x2 = max(rect->x2, dirty->x2);
		rect->y2 = max(rect->y2, dirty->y2);
	}

	mutex_unlock(&gm12u320->fb_update.lock);

	if (wakeup)
		wake_up(&gm12u320->fb_update.waitq);

	if (old_fb)
		drm_framebuffer_put(old_fb);
}

static void gm12u320_start_fb_update(struct gm12u320_device *gm12u320)
{
	mutex_lock(&gm12u320->fb_update.lock);
	gm12u320->fb_update.run = true;
	mutex_unlock(&gm12u320->fb_update.lock);

	queue_work(gm12u320->fb_update.workq, &gm12u320->fb_update.work);
}

static void gm12u320_stop_fb_update(struct gm12u320_device *gm12u320)
{
	mutex_lock(&gm12u320->fb_update.lock);
	gm12u320->fb_update.run = false;
	mutex_unlock(&gm12u320->fb_update.lock);

	wake_up(&gm12u320->fb_update.waitq);
	cancel_work_sync(&gm12u320->fb_update.work);

	mutex_lock(&gm12u320->fb_update.lock);
	if (gm12u320->fb_update.fb) {
		drm_framebuffer_put(gm12u320->fb_update.fb);
		gm12u320->fb_update.fb = NULL;
	}
	mutex_unlock(&gm12u320->fb_update.lock);
}

static int gm12u320_set_ecomode(struct gm12u320_device *gm12u320)
{
	return gm12u320_misc_request(gm12u320, MISC_REQ_GET_SET_ECO_A,
				     MISC_REQ_GET_SET_ECO_B, 0x01 /* set */,
				     eco_mode ? 0x01 : 0x00, 0x00, 0x01);
}

/* ------------------------------------------------------------------ */
/* gm12u320 connector						      */

/*
 * We use fake EDID info so that userspace know that it is dealing with
 * an Acer projector, rather then listing this as an "unknown" monitor.
 * Note this assumes this driver is only ever used with the Acer C120, if we
 * add support for other devices the vendor and model should be parameterized.
 */
static struct edid gm12u320_edid = {
	.header		= { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 },
	.mfg_id		= { 0x04, 0x72 },	/* "ACR" */
	.prod_code	= { 0x20, 0xc1 },	/* C120h */
	.serial		= 0xaa55aa55,
	.mfg_week	= 1,
	.mfg_year	= 16,
	.version	= 1,			/* EDID 1.3 */
	.revision	= 3,			/* EDID 1.3 */
	.input		= 0x08,			/* Analog input */
	.features	= 0x0a,			/* Pref timing in DTD 1 */
	.standard_timings = { { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 },
			      { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 } },
	.detailed_timings = { {
		.pixel_clock = 3383,
		/* hactive = 848, hblank = 256 */
		.data.pixel_data.hactive_lo = 0x50,
		.data.pixel_data.hblank_lo = 0x00,
		.data.pixel_data.hactive_hblank_hi = 0x31,
		/* vactive = 480, vblank = 28 */
		.data.pixel_data.vactive_lo = 0xe0,
		.data.pixel_data.vblank_lo = 0x1c,
		.data.pixel_data.vactive_vblank_hi = 0x10,
		/* hsync offset 40 pw 128, vsync offset 1 pw 4 */
		.data.pixel_data.hsync_offset_lo = 0x28,
		.data.pixel_data.hsync_pulse_width_lo = 0x80,
		.data.pixel_data.vsync_offset_pulse_width_lo = 0x14,
		.data.pixel_data.hsync_vsync_offset_pulse_width_hi = 0x00,
		/* Digital separate syncs, hsync+, vsync+ */
		.data.pixel_data.misc = 0x1e,
	}, {
		.pixel_clock = 0,
		.data.other_data.type = 0xfd, /* Monitor ranges */
		.data.other_data.data.range.min_vfreq = 59,
		.data.other_data.data.range.max_vfreq = 61,
		.data.other_data.data.range.min_hfreq_khz = 29,
		.data.other_data.data.range.max_hfreq_khz = 32,
		.data.other_data.data.range.pixel_clock_mhz = 4, /* 40 MHz */
		.data.other_data.data.range.flags = 0,
		.data.other_data.data.range.formula.cvt = {
			0xa0, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 },
	}, {
		.pixel_clock = 0,
		.data.other_data.type = 0xfc, /* Model string */
		.data.other_data.data.str.str = {
			'P', 'r', 'o', 'j', 'e', 'c', 't', 'o', 'r', '\n',
			' ', ' ',  ' ' },
	}, {
		.pixel_clock = 0,
		.data.other_data.type = 0xfe, /* Unspecified text / padding */
		.data.other_data.data.str.str = {
			'\n', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
			' ', ' ',  ' ' },
	} },
	.checksum = 0x13,
};

static int gm12u320_conn_get_modes(struct drm_connector *connector)
{
	drm_connector_update_edid_property(connector, &gm12u320_edid);
	return drm_add_edid_modes(connector, &gm12u320_edid);
}

static const struct drm_connector_helper_funcs gm12u320_conn_helper_funcs = {
	.get_modes = gm12u320_conn_get_modes,
};

static const struct drm_connector_funcs gm12u320_conn_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int gm12u320_conn_init(struct gm12u320_device *gm12u320)
{
	drm_connector_helper_add(&gm12u320->conn, &gm12u320_conn_helper_funcs);
	return drm_connector_init(&gm12u320->dev, &gm12u320->conn,
				  &gm12u320_conn_funcs, DRM_MODE_CONNECTOR_VGA);
}

/* ------------------------------------------------------------------ */
/* gm12u320 (simple) display pipe				      */

static void gm12u320_pipe_enable(struct drm_simple_display_pipe *pipe,
				 struct drm_crtc_state *crtc_state,
				 struct drm_plane_state *plane_state)
{
	struct gm12u320_device *gm12u320 = pipe->crtc.dev->dev_private;
	struct drm_rect rect = { 0, 0, GM12U320_USER_WIDTH, GM12U320_HEIGHT };

	gm12u320_fb_mark_dirty(plane_state->fb, &rect);
	gm12u320_start_fb_update(gm12u320);
	gm12u320->pipe_enabled = true;
}

static void gm12u320_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct gm12u320_device *gm12u320 = pipe->crtc.dev->dev_private;

	gm12u320_stop_fb_update(gm12u320);
	gm12u320->pipe_enabled = false;
}

static void gm12u320_pipe_update(struct drm_simple_display_pipe *pipe,
				 struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_rect rect;

	if (drm_atomic_helper_damage_merged(old_state, state, &rect))
		gm12u320_fb_mark_dirty(pipe->plane.state->fb, &rect);

	if (crtc->state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static const struct drm_simple_display_pipe_funcs gm12u320_pipe_funcs = {
	.enable	    = gm12u320_pipe_enable,
	.disable    = gm12u320_pipe_disable,
	.update	    = gm12u320_pipe_update,
};

static const uint32_t gm12u320_pipe_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const uint64_t gm12u320_pipe_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static void gm12u320_driver_release(struct drm_device *dev)
{
	struct gm12u320_device *gm12u320 = dev->dev_private;

	gm12u320_usb_free(gm12u320);
	drm_mode_config_cleanup(dev);
	drm_dev_fini(dev);
	kfree(gm12u320);
}

DEFINE_DRM_GEM_SHMEM_FOPS(gm12u320_fops);

static struct drm_driver gm12u320_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,

	.name		 = DRIVER_NAME,
	.desc		 = DRIVER_DESC,
	.date		 = DRIVER_DATE,
	.major		 = DRIVER_MAJOR,
	.minor		 = DRIVER_MINOR,

	.release	 = gm12u320_driver_release,
	.fops		 = &gm12u320_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
};

static const struct drm_mode_config_funcs gm12u320_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int gm12u320_usb_probe(struct usb_interface *interface,
			      const struct usb_device_id *id)
{
	struct gm12u320_device *gm12u320;
	struct drm_device *dev;
	int ret;

	/*
	 * The gm12u320 presents itself to the system as 2 usb mass-storage
	 * interfaces, we only care about / need the first one.
	 */
	if (interface->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	gm12u320 = kzalloc(sizeof(*gm12u320), GFP_KERNEL);
	if (gm12u320 == NULL)
		return -ENOMEM;

	gm12u320->udev = interface_to_usbdev(interface);
	INIT_WORK(&gm12u320->fb_update.work, gm12u320_fb_update_work);
	mutex_init(&gm12u320->fb_update.lock);
	init_waitqueue_head(&gm12u320->fb_update.waitq);

	dev = &gm12u320->dev;
	ret = drm_dev_init(dev, &gm12u320_drm_driver, &interface->dev);
	if (ret) {
		kfree(gm12u320);
		return ret;
	}
	dev->dev_private = gm12u320;

	drm_mode_config_init(dev);
	dev->mode_config.min_width = GM12U320_USER_WIDTH;
	dev->mode_config.max_width = GM12U320_USER_WIDTH;
	dev->mode_config.min_height = GM12U320_HEIGHT;
	dev->mode_config.max_height = GM12U320_HEIGHT;
	dev->mode_config.funcs = &gm12u320_mode_config_funcs;

	ret = gm12u320_usb_alloc(gm12u320);
	if (ret)
		goto err_put;

	ret = gm12u320_set_ecomode(gm12u320);
	if (ret)
		goto err_put;

	ret = gm12u320_conn_init(gm12u320);
	if (ret)
		goto err_put;

	ret = drm_simple_display_pipe_init(&gm12u320->dev,
					   &gm12u320->pipe,
					   &gm12u320_pipe_funcs,
					   gm12u320_pipe_formats,
					   ARRAY_SIZE(gm12u320_pipe_formats),
					   gm12u320_pipe_modifiers,
					   &gm12u320->conn);
	if (ret)
		goto err_put;

	drm_mode_config_reset(dev);

	usb_set_intfdata(interface, dev);
	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_put;

	drm_fbdev_generic_setup(dev, 0);

	return 0;

err_put:
	drm_dev_put(dev);
	return ret;
}

static void gm12u320_usb_disconnect(struct usb_interface *interface)
{
	struct drm_device *dev = usb_get_intfdata(interface);
	struct gm12u320_device *gm12u320 = dev->dev_private;

	gm12u320_stop_fb_update(gm12u320);
	drm_dev_unplug(dev);
	drm_dev_put(dev);
}

static __maybe_unused int gm12u320_suspend(struct usb_interface *interface,
					   pm_message_t message)
{
	struct drm_device *dev = usb_get_intfdata(interface);
	struct gm12u320_device *gm12u320 = dev->dev_private;

	if (gm12u320->pipe_enabled)
		gm12u320_stop_fb_update(gm12u320);

	return 0;
}

static __maybe_unused int gm12u320_resume(struct usb_interface *interface)
{
	struct drm_device *dev = usb_get_intfdata(interface);
	struct gm12u320_device *gm12u320 = dev->dev_private;

	gm12u320_set_ecomode(gm12u320);
	if (gm12u320->pipe_enabled)
		gm12u320_start_fb_update(gm12u320);

	return 0;
}

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE(0x1de1, 0xc102) },
	{},
};
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver gm12u320_usb_driver = {
	.name = "gm12u320",
	.probe = gm12u320_usb_probe,
	.disconnect = gm12u320_usb_disconnect,
	.id_table = id_table,
#ifdef CONFIG_PM
	.suspend = gm12u320_suspend,
	.resume = gm12u320_resume,
	.reset_resume = gm12u320_resume,
#endif
};

module_usb_driver(gm12u320_usb_driver);
MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
MODULE_LICENSE("GPL");
