/*
 * Google LWIS Fence
 *
 * Copyright (c) 2022 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef LWIS_FENCE_H_
#define LWIS_FENCE_H_

#include "lwis_device.h"

struct lwis_fence {
	int fd;
	int status;
	/* Top device for printing logs */
	struct lwis_device *lwis_top_dev;
	/* Status wait queue for waking up userspace */
	wait_queue_head_t status_wait_queue;
};

/*
 *  lwis_fence_create: Create a new lwis_fence.
 */
int lwis_fence_create(struct lwis_device *lwis_dev);

#endif /* LWIS_IOCTL_H_ */