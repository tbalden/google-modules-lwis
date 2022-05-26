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

int ioctl_lwis_fence_create(struct lwis_device *lwis_dev, int32_t __user *msg);

/*
 *  lwis_fence_get: Get the lwis_fence associated with the fd.
 */
struct lwis_device *lwis_fence_get(int fd);

#ifdef LWIS_FENCE_ENABLED
bool lwis_triggered_by_condition(struct lwis_transaction *transaction);

bool lwis_event_triggered_condition_ready(struct lwis_transaction *transaction,
					  struct lwis_transaction *weak_transaction,
					  int64_t event_id, int64_t event_counter);


bool lwis_fence_triggered_condition_ready(struct lwis_transaction *transaction,
					  struct lwis_fence *fence);

/*
 *  lwis_parse_trigger_condition: Add the transaction to the associated trigger
 *  fence and event lists.
 */
int lwis_parse_trigger_condition(struct lwis_client *client,
				 struct lwis_transaction *transaction);
#else
static inline
bool lwis_triggered_by_condition(struct lwis_transaction *transaction)
{
	return false;
}

static inline
bool lwis_event_triggered_condition_ready(struct lwis_transaction *transaction,
					  struct lwis_transaction *weak_transaction,
					  int64_t event_id, int64_t event_counter)
{
	return false;
}

static inline
bool lwis_fence_triggered_condition_ready(struct lwis_transaction *transaction,
					  struct lwis_fence *fence)
{
	return false;
}

static inline
int lwis_parse_trigger_condition(struct lwis_client *client, struct
				 lwis_transaction *transaction)
{
	return 0;
}
#endif

#endif /* LWIS_IOCTL_H_ */
