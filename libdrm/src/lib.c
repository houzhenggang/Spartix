/*----------------------------------------------------------------------
 * Copyright (C) 2016, 2017 Pedro Falcato
 *
 * This file is part of Spartix, and is made available under
 * the terms of the GNU General Public License version 2.
 *
 * You can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 2 as published by the Free Software
 * Foundation.
 *----------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <drm/drm.h>

int drm_fd = -1;
int drm_request_info(struct drm_info *info)
{
	if(!info)
		return -1;
	if(ioctl(drm_fd, DRM_REQUEST_DRMINFO, info) < 0)
	{
		printf("libdrm: %s: DRM_REQUEST_DRMINFO failed!\n", __FUNCTION__);
		return -1;
	}
	return 0;
}
int drm_initialize(struct drm_info **out)
{
	drm_fd = open("/dev/drm", O_RDWR);
	
	if(drm_fd == -1)
	{
		printf("libdrm: /dev/drm not found!\n");
		return -1;
	}
	/* TODO: When lsmod(2) gets implemented, use it to check if drm is loaded in case of an error */
	struct drm_info *buf = malloc(sizeof(struct drm_info));
	if(!buf)
	{
		printf("libdrm: %s: Not enough memory\n", __FUNCTION__);
		return -1;
	}
	memset(buf, 0, sizeof(struct drm_info));
	if(drm_request_info(buf) < 0)
		return -1;
	*out = buf;

	return 0;
}