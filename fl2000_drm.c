// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2012, Red Hat
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

#define DRM_DRIVER_NAME "fl2000_drm"
#define DRM_DRIVER_DESC "USB-HDMI"
#define DRM_DRIVER_DATE "20181001"

#define DRM_DRIVER_MAJOR      0
#define DRM_DRIVER_MINOR      0
#define DRM_DRIVER_PATCHLEVEL 1

/* Maximum supported resolution, out-of-the-blue numbers */
#define FL20000_MAX_WIDTH  4000
#define FL20000_MAX_HEIGHT 4000

/* Force using 32-bit XRGB8888 on input for simplicity */
#define FL2000_FB_BPP 32
static const u32 fl2000_pixel_formats[] = {
	DRM_FORMAT_XRGB8888,
};

/* Maximum pixel clock set to 500MHz. It is hard to get more or less precise PLL configuration for
 * higher clock
 */
#define FL2000_MAX_PIXCLOCK 500000000

/* PLL computing precision is 6 digits after comma */
#define FL2000_PLL_PRECISION 1000000

/* Input xtal clock, Hz */
#define FL2000_XTAL 10000000 /* 10 MHz */

/* Internal vco clock min/max, Hz */
#define FL2000_VCOCLOCK_MIN 62500000 /* 62.5 MHz */
#define FL2000_VCOCLOCK_MAX 1000000000 /* 1GHz */

/* Maximum acceptable ppm error */
#define FL2000_PPM_ERR_MAX 500

/* Assume bulk transfers can use only 80% of USB bandwidth */
#define FL2000_BULK_BW_PERCENT 80

#define FL2000_BULK_BW_HIGH_SPEED	(480000000ull * 100 / FL2000_BULK_BW_PERCENT / 8)
#define FL2000_BULK_BW_SUPER_SPEED	(5000000000ull * 100 / FL2000_BULK_BW_PERCENT / 8)
#define FL2000_BULK_BW_SUPER_SPEED_PLUS (10000000000ull * 100 / FL2000_BULK_BW_PERCENT / 8)

static unsigned int fl2000_get_bytes_pix(enum usb_device_speed speed, unsigned int pixclock)
{
	unsigned int bytes_pix;
	unsigned int max_bw;

	/* Calculate maximum bandwidth, bytes per second */
	switch (speed) {
	case USB_SPEED_HIGH:
		max_bw = FL2000_BULK_BW_HIGH_SPEED;
		break;
	case USB_SPEED_SUPER:
		max_bw = FL2000_BULK_BW_SUPER_SPEED;
		break;
	case USB_SPEED_SUPER_PLUS:
		max_bw = FL2000_BULK_BW_SUPER_SPEED_PLUS;
		break;
	default:
		return 0;
	}

	/* Maximum bytes per pixel with maximum bandwidth */
	bytes_pix = max_bw / pixclock;
	switch (bytes_pix) {
	case 0: /* Not enough */
	case 1: /* RGB 332 not supported*/
		return 0;
	case 2: /* RGB 565 */
	case 3: /* RGB 888 */
		break;
	default:
		bytes_pix = 3;
		break;
	}

	return bytes_pix;
}

struct fl2000_drm_if {
	struct usb_device *usb_dev;
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct fl2000_stream *stream;
	struct fl2000_intr *intr;

	/* Only used when driving the integrated VGA DAC, i.e. when there is no bridge component */
	struct drm_connector connector;
	const struct drm_edid *drm_edid;
};

/* Read one EDID block over the FL2000's own I2C master.
 *
 * This deliberately bypasses the i2c_adapter the driver registers. That adapter describes the
 * hardware honestly - fl2000_i2c_quirks advertises max_read_len = 1 - so drm_edid_read_ddc() and
 * friends are rejected by i2c_check_quirks() before any transfer happens. It is also wasteful:
 * fl2000_i2c_xfer() reads a full dword from the chip and then throws three of the four bytes away.
 * Going straight to fl2000_i2c_dword() keeps all four, turning a 128 byte block into 32 USB
 * round trips rather than 128
 */
static int fl2000_get_edid_block(void *data, u8 *buf, unsigned int block, size_t len)
{
	struct fl2000_drm_if *drm_if = data;
	struct usb_device *usb_dev = drm_if->usb_dev;
	unsigned int base = block * EDID_LENGTH;

	/* i2c_offset in FL2000_VGA_I2C_SC_REG is 8 bits wide and there is no E-DDC segment
	 * pointer, so only the base block and one extension are reachable
	 */
	if (base + len > 256)
		return -E2BIG;

	for (size_t i = 0; i < len; i += sizeof(u32)) {
		union {
			u32 w;
			u8 b[4];
		} dword;
		int ret;

		ret = fl2000_i2c_dword(usb_dev, true, FL2000_DDC_ADDR, base + i, &dword.w);
		if (ret)
			return ret;

		memcpy(&buf[i], dword.b, min_t(size_t, sizeof(dword.b), len - i));
	}

	return 0;
}

/* Line frequency window accepted in 15 kHz CRT mode, Hz: NTSC and PAL rates with some margin */
#define FL2000_CRT15K_HFREQ_MIN 14500
#define FL2000_CRT15K_HFREQ_MAX 16800

/* Progressive 15 kHz modes for a TV or arcade monitor. Centring is a first guess, to be tuned
 * on a real CRT
 */
static const struct drm_display_mode fl2000_crt15k_modes[] = {
	/* 640x240@60, 12.27 MHz, 15.73 kHz: the console mode, 80x15 text */
	{ DRM_MODE("640x240", DRM_MODE_TYPE_DRIVER, 12270, 640, 656, 714, 780, 0, 240, 244, 247,
		   262, 0, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 320x240@60, 6.7 MHz, 15.73 kHz */
	{ DRM_MODE("320x240", DRM_MODE_TYPE_DRIVER, 6700, 320, 336, 368, 426, 0, 240, 244, 247,
		   262, 0, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 2560x240@60, 52 MHz super resolution, 15.63 kHz */
	{ DRM_MODE("2560x240", DRM_MODE_TYPE_DRIVER, 52000, 2560, 2624, 2864, 3328, 0, 240, 244,
		   247, 262, 0, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 720x288@50, 13.5 MHz, 15.63 kHz */
	{ DRM_MODE("720x288", DRM_MODE_TYPE_DRIVER, 13500, 720, 732, 795, 864, 0, 288, 290, 293,
		   312, 0, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
};

static int fl2000_crt15k_get_modes(struct drm_connector *connector)
{
	int count = 0;

	for (int i = 0; i < ARRAY_SIZE(fl2000_crt15k_modes); i++) {
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(connector->dev, &fl2000_crt15k_modes[i]);
		if (!mode)
			continue;
		if (i == 0)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
		count++;
	}

	return count;
}

static int fl2000_connector_get_modes(struct drm_connector *connector)
{
	struct fl2000_drm_if *drm_if = container_of(connector, struct fl2000_drm_if, connector);
	int count;

	/* A TV has no EDID worth reading, and the VGA fallback list would be harmful */
	if (fl2000_crt15k)
		return fl2000_crt15k_get_modes(connector);

	/* Re-read rather than cache: a VGA sink can be swapped without us being told */
	drm_edid_free(drm_if->drm_edid);
	drm_if->drm_edid = drm_edid_read_custom(connector, fl2000_get_edid_block, drm_if);

	drm_edid_connector_update(connector, drm_if->drm_edid);

	if (drm_if->drm_edid)
		return drm_edid_connector_add_modes(connector);

	/* No EDID. VGA monitors without usable DDC are common (missing pins 12/15 in the cable,
	 * adapters, gated chip paths), and are exactly why DRM carries standard fallback modes.
	 * Offer VESA modes up to a conservative analog ceiling and prefer the old safe default;
	 * the user can force something bigger explicitly
	 */
	count = drm_add_modes_noedid(connector, 1920, 1200);
	drm_set_preferred_mode(connector, 1024, 768);

	return count;
}

/* VGA has no hotplug detect line, so probe the sink by talking to its DDC EEPROM */
static enum drm_connector_status fl2000_connector_detect(struct drm_connector *connector, bool force)
{
	struct fl2000_drm_if *drm_if = container_of(connector, struct fl2000_drm_if, connector);
	u32 header;

	UNUSED(force);

	if (!fl2000_i2c_dword(drm_if->usb_dev, true, FL2000_DDC_ADDR, 0, &header))
		return connector_status_connected;

	/* A DDC NAK does not prove absence: this dongle NAKs the DDC EEPROM even with a
	 * Windows-proven monitor attached and awake. The chip's analog sense register is the
	 * reliable presence signal here (tracks physical cable attach/detach exactly), and a
	 * desktop needs a definitive 'connected' to enable the output - compositors ignore
	 * 'unknown'. Without EDID, get_modes() serves the VESA fallback list
	 */
	switch (fl2000_connector_sense(drm_if->usb_dev)) {
	case 1:
		return connector_status_connected;
	case 0:
		return connector_status_disconnected;
	default:
		return connector_status_unknown;
	}
}

static const struct drm_connector_helper_funcs fl2000_connector_helper_funcs = {
	.get_modes = fl2000_connector_get_modes,
};

static const struct drm_connector_funcs fl2000_connector_funcs = {
	.detect = fl2000_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

DEFINE_DRM_GEM_FOPS(fl2000_drm_driver_fops);

/* No .release callback: it runs after the refcount hits zero, and the old one called
 * drm_atomic_helper_shutdown() from there - which takes a fresh drm_dev_get() on the dying
 * device (refcount saturation, "use-after-free" WARN when the compositor dropped the last fd
 * after hot-unplug). Shutdown now happens in fl2000_drm_if_release() while the device is
 * alive. The old drm_mode_config_cleanup() call was also redundant-to-harmful: mode config
 * comes from drmm_mode_config_init() and is cleaned up by the managed release path
 */
static struct drm_driver fl2000_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.ioctls = NULL,
	.fops = &fl2000_drm_driver_fops,

	/* Shmem rather than DMA buffers: the chip is fed by the CPU anyway, and shmem lets a GPU
	 * render into buffers we import without mapping them for DMA. The DRM device sits on a USB
	 * interface, which has no DMA mask: GEM DMA imports WARNed in dma_map_sg() and, without an
	 * IOMMU translating for the USB controller, could not be contiguous anyway
	 */
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,

	.name = DRM_DRIVER_NAME,
	.desc = DRM_DRIVER_DESC,
	.major = DRM_DRIVER_MAJOR,
	.minor = DRM_DRIVER_MINOR,
	.patchlevel = DRM_DRIVER_PATCHLEVEL,
};

static const struct drm_mode_config_funcs fl2000_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Integer division compute of ppm error */
static u64 fl2000_pll_ppm_err(u64 clock_mil, u32 vco_clk, u32 divisor)
{
	u64 pll_clk_mil = (u64)vco_clk * FL2000_PLL_PRECISION / divisor;
	u64 pll_clk_err;

	/* Not using abs() here to avoid possible overflow */
	if (pll_clk_mil > clock_mil)
		pll_clk_err = pll_clk_mil - clock_mil;
	else
		pll_clk_err = clock_mil - pll_clk_mil;

	return pll_clk_err / (clock_mil / FL2000_PLL_PRECISION);
}

static inline u32 fl2000_pll_get_divisor(u64 clock_mil, u32 vco_clk, u64 *min_ppm_err)
{
	/* The divisor field is 8 bits wide on paper, but only its low 6 bits reach the PLL:
	 * measured on a FL2000DX, divisor 100 runs at the clock of 100 & 0x3f = 36. The Windows
	 * driver's ASIC PLL table never goes above 61 either. Larger divisors are what low pixel
	 * clocks (15 kHz modes) asked for, so they came out about three times too fast
	 */
	static const u32 divisor_arr[] = {
		2,  4,	6,  7,  8,	9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
		22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
		42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
		62, 63
	};
	u32 best_divisor = 0;

	/* Iterate over array */
	for (int i = 0; i < ARRAY_SIZE(divisor_arr); i++) {
		u32 divisor = divisor_arr[i];
		u64 ppm_err = fl2000_pll_ppm_err(clock_mil, vco_clk, divisor);

		if (ppm_err < *min_ppm_err) {
			*min_ppm_err = ppm_err;
			best_divisor = divisor;
		}
	}

	return best_divisor;
}

/* Try to match pixel clock - find parameters with minimal PLL error */
static u64 fl2000_pll_calc(u64 clock_mil, struct fl2000_pll *pll, u32 *clock_calculated)
{
	static const u32 prescaler_max = 2;
	static const u32 multiplier_max = 128;
	u64 min_ppm_err = (u64)(-1);

	for (u32 prescaler = 1; prescaler <= prescaler_max; prescaler++)
		for (u32 multiplier = 1; multiplier <= multiplier_max; multiplier++) {
			/* Do not need precision here yet, no 10^6 multiply */
			u32 vco_clk = FL2000_XTAL / prescaler * multiplier;
			u32 divisor;

			if (vco_clk < FL2000_VCOCLOCK_MIN || vco_clk > FL2000_VCOCLOCK_MAX)
				continue;

			divisor = fl2000_pll_get_divisor(clock_mil, vco_clk, &min_ppm_err);
			if (divisor == 0)
				continue;

			pll->prescaler = prescaler;
			pll->multiplier = multiplier;
			pll->divisor = divisor;
			if (vco_clk < 125000000)
				pll->function = 0;
			else if (vco_clk < 250000000)
				pll->function = 1;
			else if (vco_clk < 500000000)
				pll->function = 2;
			else
				pll->function = 3;
			*clock_calculated = vco_clk / divisor;
		}

	return min_ppm_err;
}

static int fl2000_mode_calc(const struct drm_display_mode *mode,
			    struct drm_display_mode *adjusted_mode, struct fl2000_pll *pll)
{
	u64 ppm_err;
	u32 clock_calculated;
	u64 clock_mil_adjusted;
	const u64 clock_mil = (u64)mode->clock * 1000 * FL2000_PLL_PRECISION;
	const int max_h_adjustment = 10;

	if (mode->clock * 1000 > FL2000_MAX_PIXCLOCK)
		return -1;

	/* Try to match pixel clock slightly adjusting htotal value, sequence is:
	 * 0, -1, 1, -2, 2, -3, 3, -3, 4, -4, 5, -5, ...
	 * Here, 's' is used for sign, 'm' is used for modulo, and 'd' is the adjustment value
	 */
	for (int m = 0, s = 0, d = 0; m <= max_h_adjustment * 2; m++, s = -s, d += m * s) {
		/* Maximum pixel clock 1GHz, or 10^9Hz. Multiply by 10^6 we get 10^15Hz. Assume
		 * maximum htotal is 10000 pix (no way) we get 10^19 max value and using u64 which
		 * is 1.8*10^19 no overflow can occur. Assume all this was checked before
		 */
		clock_mil_adjusted = clock_mil * (mode->htotal + d) / mode->htotal;

		/* To keep precision use clock multiplied by 10^6 */
		ppm_err = fl2000_pll_calc(clock_mil_adjusted, pll, &clock_calculated);

		/* Stop searching as soon as the first valid option found */
		if (ppm_err < FL2000_PPM_ERR_MAX) {
			if (adjusted_mode) {
				drm_mode_copy(adjusted_mode, mode);
				adjusted_mode->htotal += d;
				adjusted_mode->clock = clock_calculated / 1000;
			}

			return 0;
		}
	}

	/* Cannot find PLL configuration that satisfy requirements */
	return -1;
}

static enum drm_mode_status fl2000_display_mode_valid(struct drm_simple_display_pipe *pipe,
						      const struct drm_display_mode *mode)
{
	struct drm_device *drm = pipe->crtc.dev;
	struct drm_display_mode adjusted_mode;
	struct fl2000_pll pll;
	struct fl2000_drm_if *drm_if = drm->dev_private;
	struct usb_device *usb_dev = drm_if->usb_dev;

	/* Get PLL configuration and check if mode adjustments needed */
	if (fl2000_mode_calc(mode, &adjusted_mode, &pll))
		return MODE_BAD;

	if (fl2000_get_bytes_pix(usb_dev->speed, adjusted_mode.clock) == 0)
		return MODE_BAD;

	/* In CRT mode nothing outside the 15 kHz band may reach the screen, whatever asked for it */
	if (fl2000_crt15k) {
		u32 hfreq = div_u64((u64)adjusted_mode.clock * 1000, adjusted_mode.htotal);

		if (hfreq < FL2000_CRT15K_HFREQ_MIN || hfreq > FL2000_CRT15K_HFREQ_MAX)
			return MODE_H_ILLEGAL;
	}

	/* No interlace support found in the timing generator so far */
	if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
		return MODE_NO_INTERLACE;

	/* Register field widths: anything larger would be silently truncated */
	if (adjusted_mode.htotal > 4095 || adjusted_mode.vtotal > 4095)
		return MODE_BAD;
	if (adjusted_mode.hsync_end - adjusted_mode.hsync_start < 1 ||
	    adjusted_mode.hsync_end - adjusted_mode.hsync_start > 511)
		return MODE_HSYNC_WIDE;
	if (adjusted_mode.vsync_end - adjusted_mode.vsync_start < 1 ||
	    adjusted_mode.vsync_end - adjusted_mode.vsync_start > 15)
		return MODE_VSYNC_WIDE;
	/* vstart also goes to the 10 bit start_latency field */
	if (adjusted_mode.vtotal - adjusted_mode.vsync_start + 1 > 1023)
		return MODE_V_ILLEGAL;

	return MODE_OK;
}

static void fl2000_display_enable(struct drm_simple_display_pipe *pipe,
				  struct drm_crtc_state *cstate,
				  struct drm_plane_state *plane_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = pipe->crtc.dev;
	struct fl2000_drm_if *drm_if = drm->dev_private;

	/* TODO: check cstate/pstate? */
	UNUSED(cstate);
	UNUSED(plane_state);

	fl2000_stream_enable(drm_if->stream);

	drm_crtc_vblank_on(crtc);
}

static void fl2000_display_disable(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = pipe->crtc.dev;
	struct fl2000_drm_if *drm_if = drm->dev_private;

	/* Vblanks on this device ARE stream URB completions, so they stop the moment the
	 * stream does (and never come at all after hot-unplug). Turn vblanks off first: it
	 * wakes every waiter and flushes pending events, instead of letting the atomic helper
	 * wait out a timeout against a stream that can no longer tick
	 */
	drm_crtc_vblank_off(crtc);

	fl2000_stream_disable(drm_if->stream);
}

static void fb2000_dirty(struct drm_framebuffer *fb, struct drm_rect *rect)
{
	int ret;
	int idx;
	struct drm_device *drm = fb->dev;
	struct fl2000_drm_if *drm_if = drm->dev_private;
	struct iosys_map map[DRM_FORMAT_MAX_PLANES];
	struct iosys_map data[DRM_FORMAT_MAX_PLANES];

	UNUSED(rect);

	if (!drm_dev_enter(fb->dev, &idx)) {
		dev_err(drm->dev, "DRM enter failed!");
		return;
	}

	/* Works for our own buffers and for imported ones alike (a GPU renders, we display) */
	ret = drm_gem_fb_vmap(fb, map, data);
	if (ret)
		goto out;

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (!ret) {
		fl2000_stream_compress(drm_if->stream, data[0].vaddr, fb->height, fb->width,
				       fb->pitches[0]);
		drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	}

	drm_gem_fb_vunmap(fb, map);
out:
	drm_dev_exit(idx);
}

static void fl2000_display_update(struct drm_simple_display_pipe *pipe,
				  struct drm_plane_state *old_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct drm_pending_vblank_event *event = crtc->state->event;
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_rect rect;

	if (drm_atomic_helper_damage_merged(old_state, state, &rect))
		fb2000_dirty(state->fb, &rect);

	if (event) {
		crtc->state->event = NULL;

		spin_lock_irq(&drm->event_lock);
		if (crtc->state->active && drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, event);
		else
			drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irq(&drm->event_lock);
	}
}

/* Logical pipe management (no HW configuration here) */
static const struct drm_simple_display_pipe_funcs fl2000_display_funcs = {
	.mode_valid = fl2000_display_mode_valid,
	.enable = fl2000_display_enable,
	.disable = fl2000_display_disable,
	.update = fl2000_display_update
};

static void fl2000_output_mode_set(struct drm_encoder *encoder, struct drm_display_mode *mode,
				   struct drm_display_mode *adjusted_mode)
{
	struct drm_device *drm = encoder->dev;
	struct fl2000_drm_if *drm_if = drm->dev_private;
	struct usb_device *usb_dev = drm_if->usb_dev;
	struct fl2000_timings timings;
	struct fl2000_pll pll;
	unsigned int bytes_pix;

	/* Get PLL configuration and cehc if mode adjustments needed */
	if (fl2000_mode_calc(mode, adjusted_mode, &pll))
		return;

	/* Check how many bytes per pixel shall be used with adjusted clock */
	bytes_pix = fl2000_get_bytes_pix(usb_dev->speed, adjusted_mode->clock);
	if (!bytes_pix)
		return;

	dev_info(drm->dev, "Mode requested:  " DRM_MODE_FMT, DRM_MODE_ARG(mode));
	dev_info(drm->dev, "Mode configured: " DRM_MODE_FMT, DRM_MODE_ARG(adjusted_mode));

	/* Prepare timing configuration */
	timings.hactive = adjusted_mode->hdisplay;
	timings.htotal = adjusted_mode->htotal;
	timings.hsync_width = adjusted_mode->hsync_end - adjusted_mode->hsync_start;
	timings.hstart = adjusted_mode->htotal - adjusted_mode->hsync_start + 1;
	timings.vactive = adjusted_mode->vdisplay;
	timings.vtotal = adjusted_mode->vtotal;
	timings.vsync_width = adjusted_mode->vsync_end - adjusted_mode->vsync_start;
	timings.vstart = adjusted_mode->vtotal - adjusted_mode->vsync_start + 1;

	/* Set PLL settings */
	fl2000_set_pll(usb_dev, &pll);

	/* Debug bit overrides, in place before the reset latches anything */
	fl2000_debugfs_apply_pokes(usb_dev);

	/* Reset FL2000 & confirm PLL settings */
	fl2000_reset(usb_dev);

	/* Set timings settings */
	fl2000_set_timings(usb_dev, &timings);

	/* Pixel format according to number of bytes per pixel */
	fl2000_set_pixfmt(usb_dev, bytes_pix);

	/* Positive only when asked for: CRTs that care want negative syncs */
	fl2000_set_polarity(usb_dev, adjusted_mode->flags & DRM_MODE_FLAG_PHSYNC,
			    adjusted_mode->flags & DRM_MODE_FLAG_PVSYNC);

	/* Configure frame transfers */
	fl2000_set_transfers(usb_dev);

	/* Enable interrupts */
	fl2000_enable_interrupts(usb_dev);

	fl2000_afe_magic(usb_dev);

	/* And again, in case the reset or the setup above cleared them */
	fl2000_debugfs_apply_pokes(usb_dev);

	fl2000_stream_mode_set(drm_if->stream, mode->hdisplay * mode->vdisplay, bytes_pix);
}

/* FL2000 HW control functions: mode configuration, turn on/off */
static const struct drm_encoder_helper_funcs fl2000_encoder_funcs = {
	.mode_set = fl2000_output_mode_set,
};

static void fl2000_drm_if_release(struct fl2000_drm_if *drm_if, struct device *master)
{
	struct drm_device *drm = &drm_if->drm;
	struct usb_device *usb_dev = drm_if->usb_dev;

	dev_info(master, "Unbinding FL2000 master");

	/* Detach bridge. Nothing was bound in VGA mode */
	if (!fl2000_vga_mode)
		component_unbind_all(master, drm);

	/* Stop streaming interface */
	fl2000_stream_destroy(usb_dev);

	/* Stop interrupts interface */
	fl2000_intr_destroy(usb_dev);

	/* Prepare to DRM device shutdown. The atomic shutdown must run HERE, while the device
	 * is still alive - not from the .release callback where the refcount is already zero.
	 * The pipe callbacks fail fast against an already-removed USB device, so this is safe
	 * on hot-unplug too
	 */
	drm_kms_helper_poll_fini(drm);
	drm_atomic_helper_shutdown(drm);
	drm_dev_unplug(drm);

	drm_edid_free(drm_if->drm_edid);
	drm_if->drm_edid = NULL;

	/* No drm_dev_put() here - the drm_device came from devm_drm_dev_alloc(master) and is
	 * released with the master device
	 */
}

/* TODO: release on errors! */
int fl2000_drm_bind(struct device *master)
{
	int ret = 0;
	struct usb_device *usb_dev = to_usb_device(master->parent);
	struct fl2000_drm_if *drm_if;
	struct drm_device *drm;
	struct drm_mode_config *mode_config;
	u64 dma_mask;

	dev_info(master, "Binding FL2000 master");

	drm_if = devm_drm_dev_alloc(master, &fl2000_drm_driver, struct fl2000_drm_if, drm);
	if (IS_ERR(drm_if)) {
		dev_err(master, "Cannot allocate DRM structure (%ld)", PTR_ERR(drm_if));
		return (int)PTR_ERR(drm_if);
	}
	drm = &drm_if->drm;
	drm_if->usb_dev = usb_dev;
	drm->dev_private = drm_if;

	/* Let fl2000_drm_unbind() find us again. master is the i2c adapter device, whose drvdata
	 * is otherwise unused - this driver never calls i2c_set_adapdata()
	 */
	dev_set_drvdata(master, drm_if);

	ret = drmm_mode_config_init(drm);
	if (ret) {
		dev_err(master, "Cannot initialize DRM mode (%d)", ret);
		return ret;
	}

	mode_config = &drm->mode_config;
	mode_config->funcs = &fl2000_mode_config_funcs;
	mode_config->min_width = 1;
	mode_config->max_width = FL20000_MAX_WIDTH;
	mode_config->min_height = 1;
	mode_config->max_height = FL20000_MAX_HEIGHT;

	/* Set DMA mask for DRM device from mask of the 'parent' USB device */
	dma_mask = dma_get_mask(&usb_dev->dev);
	ret = dma_set_coherent_mask(drm->dev, dma_mask);
	if (ret) {
		dev_err(drm->dev, "Cannot set DRM device DMA mask (%d)", ret);
		return ret;
	}

	/* On a VGA dongle there is no bridge chip, so stand up our own connector for the chip's
	 * integrated DAC and hand it to the pipe. Must happen before the pipe is initialised,
	 * which is what attaches the connector to the encoder
	 */
	if (fl2000_vga_mode) {
		drm_connector_helper_add(&drm_if->connector, &fl2000_connector_helper_funcs);

		/* No HPD line on VGA, so rely on periodic polling of the DDC bus */
		drm_if->connector.polled =
			DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;

		ret = drm_connector_init(drm, &drm_if->connector, &fl2000_connector_funcs,
					 DRM_MODE_CONNECTOR_VGA);
		if (ret) {
			dev_err(drm->dev, "Cannot initialize VGA connector (%d)", ret);
			return ret;
		}
	}

	ret = drm_simple_display_pipe_init(drm, &drm_if->pipe, &fl2000_display_funcs,
					   fl2000_pixel_formats, ARRAY_SIZE(fl2000_pixel_formats),
					   NULL, fl2000_vga_mode ? &drm_if->connector : NULL);
	if (ret) {
		dev_err(drm->dev, "Cannot configure simple display pipe (%d)", ret);
		return ret;
	}

	/* Register 'mode_set' function to operate prior to bridge */
	drm_encoder_helper_add(&drm_if->pipe.encoder, &fl2000_encoder_funcs);

	/* Start streaming interface */
	drm_if->stream = fl2000_stream_create(usb_dev, &drm_if->pipe.crtc);

	/* Start interrupts interface */
	drm_if->intr = fl2000_intr_create(usb_dev, drm);

	/* Attach bridge. In VGA mode there is no component to bind - the connector created above
	 * stands in for it
	 */
	if (!fl2000_vga_mode) {
		ret = component_bind_all(master, &drm_if->pipe);
		if (ret) {
			dev_err(drm->dev, "Cannot attach bridge (%d)", ret);
			return ret;
		}
	}

	drm_mode_config_reset(drm);

	ret = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (ret) {
		dev_err(drm->dev, "Failed to initialize %d VBLANK(s) (%d)",
			drm->mode_config.num_crtc, ret);
		return ret;
	}

	drm_kms_helper_poll_init(drm);

	drm_plane_enable_fb_damage_clips(&drm_if->pipe.plane);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		dev_err(drm->dev, "Cannot register DRM device (%d)", ret);
		return ret;
	}

	fl2000_reset(usb_dev);
	fl2000_usb_magic(usb_dev);

	drm_client_setup_with_fourcc(drm, DRM_FORMAT_XRGB8888);

	return 0;
}

void fl2000_drm_unbind(struct device *master)
{
	struct fl2000_drm_if *drm_if = dev_get_drvdata(master);

	/* This used to devres_release() fl2000_drm_if_release from &usb_dev->dev, but that
	 * callback was never registered as a devres, so the release silently did nothing and the
	 * DRM device was never torn down
	 */
	if (!drm_if)
		return;

	dev_set_drvdata(master, NULL);

	fl2000_drm_if_release(drm_if, master);
}
