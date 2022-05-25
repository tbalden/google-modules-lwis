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

#include <linux/hashtable.h>
#include <linux/list.h>

#include "lwis_device.h"

#define LWIS_FENCE_DBG
#define LWIS_CLIENTS_HASH_BITS 8

struct lwis_fence {
	int fd;
	struct file *fp;
	int status;
	struct mutex lock;
	/* Top device for printing logs */
	struct lwis_device *lwis_top_dev;
	/* Status wait queue for waking up userspace */
	wait_queue_head_t status_wait_queue;
	/* Hash table of transactions that's triggered by this fence */
	DECLARE_HASHTABLE(transaction_list, LWIS_CLIENTS_HASH_BITS);
};

struct lwis_fence_trigger_transaction_list {
	struct lwis_client *owner;
	struct list_head list;
	struct hlist_node node;
};

/*
 *  lwis_fence_create: Create a new lwis_fence.
 */
int lwis_fence_create(struct lwis_device *lwis_dev);

/*
 *  lwis_fence_get: Get the lwis_fence associated with the fd.
 */
struct lwis_device *lwis_fence_get(int fd);

/*
 *  lwis_trigger_fence_add_transaction: Add the transaction to the trigger-fence's transactions list.
 *  Returns: 0 if fence is not signaled and transaction is added to the list
 *           -EBADFD if no lwis_fence is found with the fd
 *           -EALREADY if fence is already signaled OK and transaction should be turned into an immediate transaction
 *           -EINVAL if fence is signaled with error
 */
int lwis_trigger_fence_add_transaction(int fence_fd, struct lwis_client *client,
				       struct lwis_transaction *transaction);

#endif /* LWIS_IOCTL_H_ */