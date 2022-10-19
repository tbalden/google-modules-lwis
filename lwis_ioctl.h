/*
 * Google LWIS IOCTL Handler
 *
 * Copyright (c) 2018 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef LWIS_IOCTL_H_
#define LWIS_IOCTL_H_

#include "lwis_device.h"

/*
 *  lwis_ioctl_handler: Handle all IOCTL commands via the file descriptor.
 */
int lwis_ioctl_handler(struct lwis_client *lwis_client, unsigned int type, unsigned long param);

/*
 *  lwis_ioctl_util_synchronous_process_io_entries: Synchronous process lwis_io_entry
 */
int lwis_ioctl_util_synchronous_process_io_entries(struct lwis_device *lwis_dev, int num_io_entries,
						   struct lwis_io_entry *io_entries,
						   struct lwis_io_entry *user_msg);

/*
 *  lwis_ioctl_util_construct_io_entry: Allocate kernel lwis_io_entry from user space input
 */
int lwis_ioctl_util_construct_io_entry(struct lwis_client *client,
				       struct lwis_io_entry *user_entries, size_t num_io_entries,
				       struct lwis_io_entry **io_entries);

#endif /* LWIS_IOCTL_H_ */
