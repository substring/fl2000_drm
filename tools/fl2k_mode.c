// SPDX-License-Identifier: GPL-2.0
/*
 * Set an arbitrary modeline on the FL2000 and hold it, showing a test pattern.
 *
 *   fl2k_mode [-p pattern] [-t seconds] CLOCK_MHZ HDISP HSS HSE HTOT VDISP VSS VSE VTOT [FLAGS...]
 *
 * The modeline uses the X11 syntax, FLAGS being any of +hsync -hsync +vsync -vsync interlace.
 * Patterns: lines (alternating 1-pixel white/black lines, the default), grid, white, bars.
 * The mode is held until the timeout expires, or until Enter/SIGINT without -t.
 *
 * Build: cc -O2 -o fl2k_mode fl2k_mode.c $(pkg-config --cflags --libs libdrm)
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define DRIVER_NAME "fl2000_drm"

static volatile sig_atomic_t quit;

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

static int open_card(void)
{
	char path[32];

	for (int i = 0; i < 16; i++) {
		drmVersionPtr ver;
		int fd;

		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		ver = drmGetVersion(fd);
		if (ver && !strcmp(ver->name, DRIVER_NAME)) {
			drmFreeVersion(ver);
			fprintf(stderr, "using %s\n", path);
			return fd;
		}
		if (ver)
			drmFreeVersion(ver);
		close(fd);
	}

	return -1;
}

static void draw(uint32_t *px, uint32_t pitch, int w, int h, const char *pattern)
{
	static const uint32_t bars[] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
					 0xff00ff, 0xff0000, 0x0000ff, 0x000000 };

	for (int y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)px + (size_t)y * pitch);

		for (int x = 0; x < w; x++) {
			uint32_t c;

			if (!strcmp(pattern, "white"))
				c = 0xffffff;
			else if (!strcmp(pattern, "bars"))
				c = bars[x * 8 / w];
			else if (!strcmp(pattern, "grid"))
				c = (x % 16 == 0 || y % 16 == 0 || x == w - 1 || y == h - 1) ?
					    0xffffff : 0x000000;
			else
				c = (y & 1) ? 0x000000 : 0xffffff;

			/* Always outline the active area so cropping and centring are visible */
			if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
				c = 0xff0000;

			row[x] = c;
		}
	}
}

static int parse_mode(int argc, char **argv, drmModeModeInfo *mode)
{
	double clock;

	if (argc < 9)
		return -1;

	memset(mode, 0, sizeof(*mode));
	clock = strtod(argv[0], NULL);
	mode->clock = (uint32_t)(clock * 1000.0 + 0.5);
	mode->hdisplay = atoi(argv[1]);
	mode->hsync_start = atoi(argv[2]);
	mode->hsync_end = atoi(argv[3]);
	mode->htotal = atoi(argv[4]);
	mode->vdisplay = atoi(argv[5]);
	mode->vsync_start = atoi(argv[6]);
	mode->vsync_end = atoi(argv[7]);
	mode->vtotal = atoi(argv[8]);

	for (int i = 9; i < argc; i++) {
		if (!strcmp(argv[i], "+hsync"))
			mode->flags |= DRM_MODE_FLAG_PHSYNC;
		else if (!strcmp(argv[i], "-hsync"))
			mode->flags |= DRM_MODE_FLAG_NHSYNC;
		else if (!strcmp(argv[i], "+vsync"))
			mode->flags |= DRM_MODE_FLAG_PVSYNC;
		else if (!strcmp(argv[i], "-vsync"))
			mode->flags |= DRM_MODE_FLAG_NVSYNC;
		else if (!strcmp(argv[i], "interlace"))
			mode->flags |= DRM_MODE_FLAG_INTERLACE;
		else
			return -1;
	}

	mode->vrefresh = (uint32_t)(mode->clock * 1000.0 / (mode->htotal * mode->vtotal) + 0.5);
	mode->type = DRM_MODE_TYPE_USERDEF;
	snprintf(mode->name, sizeof(mode->name), "%dx%d", mode->hdisplay, mode->vdisplay);

	return 0;
}

static void usage(void)
{
	fprintf(stderr, "usage: fl2k_mode [-p lines|grid|white|bars] [-t seconds] CLOCK_MHZ "
			"HDISP HSS HSE HTOT VDISP VSS VSE VTOT [+hsync|-hsync] [+vsync|-vsync] "
			"[interlace]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *pattern = "lines";
	struct drm_mode_create_dumb create = { 0 };
	struct drm_mode_map_dumb map = { 0 };
	struct drm_mode_destroy_dumb destroy = { 0 };
	drmModeModeInfo mode;
	drmModeRes *res;
	drmModeConnector *conn;
	uint32_t fb_id, conn_id, crtc_id;
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	double timeout = -1;
	void *px;
	int fd, opt, ret;

	while ((opt = getopt(argc, argv, "+p:t:")) != -1) {
		switch (opt) {
		case 'p':
			pattern = optarg;
			break;
		case 't':
			timeout = strtod(optarg, NULL);
			break;
		default:
			usage();
		}
	}
	if (parse_mode(argc - optind, argv + optind, &mode))
		usage();

	fd = open_card();
	if (fd < 0) {
		fprintf(stderr, "no %s card found\n", DRIVER_NAME);
		return 1;
	}

	res = drmModeGetResources(fd);
	if (!res || res->count_connectors < 1 || res->count_crtcs < 1) {
		fprintf(stderr, "no connector/crtc\n");
		return 1;
	}
	conn_id = res->connectors[0];
	crtc_id = res->crtcs[0];
	conn = drmModeGetConnector(fd, conn_id);
	if (conn)
		fprintf(stderr, "connector %u: %s\n", conn_id,
			conn->connection == DRM_MODE_CONNECTED ? "connected" : "not connected");

	create.width = mode.hdisplay;
	create.height = mode.vdisplay;
	create.bpp = 32;
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create)) {
		perror("create dumb");
		return 1;
	}

	handles[0] = create.handle;
	pitches[0] = create.pitch;
	if (drmModeAddFB2(fd, mode.hdisplay, mode.vdisplay, DRM_FORMAT_XRGB8888, handles, pitches,
			  offsets, &fb_id, 0)) {
		perror("addfb");
		return 1;
	}

	map.handle = create.handle;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
		perror("map dumb");
		return 1;
	}
	px = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
	if (px == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	draw(px, create.pitch, mode.hdisplay, mode.vdisplay, pattern);

	ret = drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, &conn_id, 1, &mode);
	if (ret) {
		fprintf(stderr, "setcrtc failed: %s\n", strerror(-ret ? -ret : errno));
		return 1;
	}
	/* Flush the CPU-drawn content: the driver copies the framebuffer on damage */
	drmModeDirtyFB(fd, fb_id, NULL, 0);

	fprintf(stderr, "mode set: %.3f MHz %d %d %d %d / %d %d %d %d, %.4f Hz nominal\n",
		mode.clock / 1000.0, mode.hdisplay, mode.hsync_start, mode.hsync_end, mode.htotal,
		mode.vdisplay, mode.vsync_start, mode.vsync_end, mode.vtotal,
		mode.clock * 1000.0 / ((double)mode.htotal * mode.vtotal));

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (timeout >= 0) {
		usleep((useconds_t)(timeout * 1e6));
	} else {
		struct pollfd pfd = { .fd = 0, .events = POLLIN };

		while (!quit && poll(&pfd, 1, 200) == 0)
			;
	}

	drmModeSetCrtc(fd, crtc_id, 0, 0, 0, NULL, 0, NULL);
	drmModeRmFB(fd, fb_id);
	munmap(px, create.size);
	destroy.handle = create.handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	close(fd);

	return 0;
}
