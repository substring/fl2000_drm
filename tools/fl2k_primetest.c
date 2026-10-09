// SPDX-License-Identifier: GPL-2.0
/*
 * Check that buffers rendered by the main GPU can be imported by the FL2000 (PRIME), which is
 * what X reverse PRIME and GPU-rendered applications need on a display-only USB device.
 *
 * Build: cc -O2 -o fl2k_primetest fl2k_primetest.c $(pkg-config --cflags --libs libdrm gbm)
 */

#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

int main(void)
{
	int gpu = -1, fl = -1;
	struct gbm_device *gbm;
	uint64_t cap = 0;
	int failures = 0;

	for (int i = 0; i < 8; i++) {
		char path[32];
		drmVersionPtr ver;
		int fd;

		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		ver = drmGetVersion(fd);
		if (ver && !strcmp(ver->name, "fl2000_drm"))
			fl = fd;
		else if (ver && gpu < 0)
			gpu = fd;
		else
			close(fd);
		if (ver)
			drmFreeVersion(ver);
	}
	if (fl < 0 || gpu < 0) {
		fprintf(stderr, "need both a fl2000_drm card and a GPU card\n");
		return 1;
	}

	drmGetCap(fl, DRM_CAP_PRIME, &cap);
	printf("fl2000 PRIME cap: import=%d export=%d\n", !!(cap & DRM_PRIME_CAP_IMPORT),
	       !!(cap & DRM_PRIME_CAP_EXPORT));

	gbm = gbm_create_device(gpu);
	if (!gbm) {
		fprintf(stderr, "gbm_create_device failed\n");
		return 1;
	}

	static const int sizes[][2] = { { 320, 240 }, { 2560, 240 }, { 1920, 1080 } };
	for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		struct gbm_bo *bo;
		uint32_t handle = 0;
		int fd, ret;

		bo = gbm_bo_create(gbm, sizes[i][0], sizes[i][1], GBM_FORMAT_XRGB8888,
				   GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
		if (!bo) {
			printf("%dx%d: gbm_bo_create failed\n", sizes[i][0], sizes[i][1]);
			failures++;
			continue;
		}
		fd = gbm_bo_get_fd(bo);
		ret = drmPrimeFDToHandle(fl, fd, &handle);
		printf("%dx%d linear GPU buffer -> fl2000 import: %s\n", sizes[i][0], sizes[i][1],
		       ret ? strerror(errno) : "OK");
		if (ret)
			failures++;
		close(fd);
		gbm_bo_destroy(bo);
	}

	return failures ? 1 : 0;
}
