/*
 * Google LWIS Fence
 *
 * Copyright (c) 2022 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/poll.h>

#include "lwis_device_top.h"
#include "lwis_commands.h"
#include "lwis_fence.h"
#include "lwis_transaction.h"

#define HASH_CLIENT(x) hash_ptr(x, LWIS_CLIENTS_HASH_BITS)

static int lwis_fence_release(struct inode *node, struct file *fp);
static ssize_t lwis_fence_get_status(struct file *fp, char __user *user_buffer, size_t len,
				     loff_t *offset);
static ssize_t lwis_fence_signal(struct file *fp, const char __user *user_buffer, size_t len,
				 loff_t *offset);
static unsigned int lwis_fence_poll(struct file *fp, poll_table *wait);

static const struct file_operations fence_file_ops = {
	.owner = THIS_MODULE,
	.release = lwis_fence_release,
	.read = lwis_fence_get_status,
	.write = lwis_fence_signal,
	.poll = lwis_fence_poll,
};

/*
 *  lwis_fence_release: Closing an instance of a LWIS fence
 */
static int lwis_fence_release(struct inode *node, struct file *fp)
{
	struct lwis_fence *lwis_fence = fp->private_data;
	struct lwis_fence_trigger_transaction_list *tx_list;
	struct lwis_pending_transaction_id *transaction_id;
	/* Temporary vars for traversal */
	struct hlist_node *n;
	struct list_head *it_tran, *it_tran_tmp;
	int i;

#ifdef LWIS_FENCE_DBG
	dev_info(lwis_fence->lwis_top_dev->dev, "Releasing lwis_fence fd-%d", lwis_fence->fd);
#endif
	if (lwis_fence->status == LWIS_FENCE_STATUS_NOT_SIGNALED) {
		dev_err(lwis_fence->lwis_top_dev->dev,
			"lwis_fence fd-%d release without being signaled", lwis_fence->fd);
	}

	if (!hash_empty(lwis_fence->transaction_list)) {
		hash_for_each_safe (lwis_fence->transaction_list, i, n, tx_list, node) {
			if (!list_empty(&tx_list->list)) {
				list_for_each_safe (it_tran, it_tran_tmp, &tx_list->list) {
					transaction_id =
						list_entry(it_tran,
							   struct lwis_pending_transaction_id,
							   list_node);
					list_del(&transaction_id->list_node);
					kfree(transaction_id);
				}
			}
			hash_del(&tx_list->node);
			kfree(tx_list);
		}
	}

	kfree(lwis_fence);
	return 0;
}

/*
 *  lwis_fence_get_status: Read the LWIS fence's status
 */
static ssize_t lwis_fence_get_status(struct file *fp, char __user *user_buffer, size_t len,
				     loff_t *offset)
{
	int status = 0;
	struct lwis_fence *lwis_fence = fp->private_data;
	int max_len, read_len;

	if (!lwis_fence) {
		dev_err(lwis_fence->lwis_top_dev->dev, "Cannot find lwis_fence instance\n");
		return -EFAULT;
	}

	max_len = sizeof(status) - *offset;
	if (len > max_len) {
		len = max_len;
	}

	mutex_lock(&lwis_fence->lock);
	status = lwis_fence->status;
	mutex_unlock(&lwis_fence->lock);

	read_len = len - copy_to_user((void __user *)user_buffer, (void *)&status + *offset, len);
	*offset += read_len;
#ifdef LWIS_FENCE_DBG
	dev_info(lwis_fence->lwis_top_dev->dev, "lwis_fence fd-%d reading status = %d",
		 lwis_fence->fd, lwis_fence->status);
#endif
	return read_len;
}

/*
 *  lwis_fence_signal: Signal fence with the error code from user
 */
static ssize_t lwis_fence_signal(struct file *fp, const char __user *user_buffer, size_t len,
				 loff_t *offset)
{
	int status = 0;
	struct lwis_fence *lwis_fence = fp->private_data;
	struct lwis_fence_trigger_transaction_list *tx_list;
	/* Temporary vars for hash table traversal */
	struct hlist_node *n;
	int i;

	if (!lwis_fence) {
		dev_err(lwis_fence->lwis_top_dev->dev, "Cannot find lwis_fence instance\n");
		return -EFAULT;
	}

	if (len != sizeof(lwis_fence->status)) {
		dev_err(lwis_fence->lwis_top_dev->dev,
			"Signal lwis_fence fd-%d with incorrect buffer length\n", lwis_fence->fd);
		return -EINVAL;
	}

	/* Set lwis_fence's status if not signaled */
	len = len - copy_from_user(&status, (void __user *)user_buffer, len);
	mutex_lock(&lwis_fence->lock);
	if (lwis_fence->status != LWIS_FENCE_STATUS_NOT_SIGNALED) {
		/* Return error if fence is already signaled */
		dev_err(lwis_fence->lwis_top_dev->dev,
			"Cannot signal a lwis_fence fd-%d already signaled, status is %d\n",
			lwis_fence->fd, lwis_fence->status);
		mutex_unlock(&lwis_fence->lock);
		return -EINVAL;
	}
	lwis_fence->status = status;
	mutex_unlock(&lwis_fence->lock);

	wake_up_interruptible(&lwis_fence->status_wait_queue);
#ifdef LWIS_FENCE_DBG
	dev_info(lwis_fence->lwis_top_dev->dev, "lwis_fence fd-%d setting status to %d",
		 lwis_fence->fd, lwis_fence->status);
#endif

	hash_for_each_safe (lwis_fence->transaction_list, i, n, tx_list, node) {
		hash_del(&tx_list->node);
		lwis_transaction_fence_trigger(tx_list->owner, lwis_fence, &tx_list->list);
		if (!list_empty(&tx_list->list)) {
			dev_err(lwis_fence->lwis_top_dev->dev,
				"Fail to trigger all transactions\n");
		}
		kfree(tx_list);
	}

	return len;
}

/*
 *  lwis_fence_poll: Poll status function of LWIS fence
 */
static unsigned int lwis_fence_poll(struct file *fp, poll_table *wait)
{
	int status = 0;
	struct lwis_fence *lwis_fence = fp->private_data;
	if (!lwis_fence) {
		dev_err(lwis_fence->lwis_top_dev->dev, "Cannot find lwis_fence instance\n");
		return POLLERR;
	}

	poll_wait(fp, &lwis_fence->status_wait_queue, wait);

	mutex_lock(&lwis_fence->lock);
	status = lwis_fence->status;
	mutex_unlock(&lwis_fence->lock);

	/* Check if the fence is already signaled */
	if (status != LWIS_FENCE_STATUS_NOT_SIGNALED) {
#ifdef LWIS_FENCE_DBG
		dev_info(lwis_fence->lwis_top_dev->dev, "lwis_fence fd-%d poll return POLLIN",
			 lwis_fence->fd);
#endif
		return POLLIN;
	}

#ifdef LWIS_FENCE_DBG
	dev_info(lwis_fence->lwis_top_dev->dev, "lwis_fence fd-%d poll return 0", lwis_fence->fd);
#endif
	return 0;
}

int lwis_fence_create(struct lwis_device *lwis_dev)
{
	int fd_or_err;
	struct lwis_fence *new_fence;

	/* Allocate a new instance of lwis_fence struct */
	new_fence = kmalloc(sizeof(struct lwis_fence), GFP_ATOMIC);
	if (!new_fence) {
		dev_err(lwis_dev->dev, "Failed to allocate lwis_fence at creating new fence\n");
		return -ENOMEM;
	}

	/* Open a new fd for the new fence */
	fd_or_err = anon_inode_getfd("lwis_fence_file", &fence_file_ops, new_fence, O_RDWR);
	if (fd_or_err < 0) {
		kfree(new_fence);
		dev_err(lwis_dev->dev, "Failed to create a new file instance for lwis_fence\n");
		return fd_or_err;
	}

	new_fence->fd = fd_or_err;
	new_fence->lwis_top_dev = lwis_dev->top_dev;
	new_fence->status = LWIS_FENCE_STATUS_NOT_SIGNALED;
	init_waitqueue_head(&new_fence->status_wait_queue);
#ifdef LWIS_FENCE_DBG
	dev_info(lwis_dev->dev, "lwis_fence created new LWIS fence fd: %d", new_fence->fd);
#endif
	return fd_or_err;
}

static struct lwis_fence_trigger_transaction_list *transaction_list_find(struct lwis_fence *fence,
									 struct lwis_client *owner)
{
	int hash_key = HASH_CLIENT(owner);
	struct lwis_fence_trigger_transaction_list *tx_list;
	hash_for_each_possible (fence->transaction_list, tx_list, node, hash_key) {
		if (tx_list->owner == owner) {
			return tx_list;
		}
	}
	return NULL;
}

static struct lwis_fence_trigger_transaction_list *
transaction_list_create(struct lwis_fence *fence, struct lwis_client *owner)
{
	struct lwis_fence_trigger_transaction_list *tx_list =
		kmalloc(sizeof(struct lwis_fence_trigger_transaction_list), GFP_ATOMIC);
	if (!tx_list) {
		dev_err(fence->lwis_top_dev->dev, "Cannot allocate new event list\n");
		return NULL;
	}
	tx_list->owner = owner;
	INIT_LIST_HEAD(&tx_list->list);
	hash_add(fence->transaction_list, &tx_list->node, HASH_CLIENT(owner));
	return tx_list;
}

static struct lwis_fence_trigger_transaction_list *
transaction_list_find_or_create(struct lwis_fence *fence, struct lwis_client *owner)
{
	struct lwis_fence_trigger_transaction_list *list = transaction_list_find(fence, owner);
	return (list == NULL) ? transaction_list_create(fence, owner) : list;
}

static int lwis_trigger_fence_add_transaction(int fence_fd, struct lwis_client *client,
					      struct lwis_transaction *transaction)
{
	struct file *fp;
	struct lwis_fence *lwis_fence;
	struct lwis_pending_transaction_id *pending_transaction_id;
	struct lwis_fence_trigger_transaction_list *tx_list;
	int ret = 0;

	fp = fget(fence_fd);
	if (fp == NULL) {
		dev_err(client->lwis_dev->dev, "Failed to find lwis_fence with fd %d\n", fence_fd);
		return -EBADF;
	}
	lwis_fence = fp->private_data;
	if (lwis_fence->fd != fence_fd) {
		dev_err(client->lwis_dev->dev,
			"Invalid lwis_fence with fd %d. Contains stale data \n", fence_fd);
		return -EBADF;
	}

	pending_transaction_id = kmalloc(sizeof(struct lwis_pending_transaction_id), GFP_ATOMIC);
	if (!pending_transaction_id) {
		dev_err(client->lwis_dev->dev,
			"Failed to allocate lwis_pending_transaction_id at adding transactions to fence\n");
		return -ENOMEM;
	}
	pending_transaction_id->id = transaction->info.id;

	mutex_lock(&lwis_fence->lock);
	if (lwis_fence->status == LWIS_FENCE_STATUS_NOT_SIGNALED) {
		lwis_fence->fp = fp;
		tx_list = transaction_list_find_or_create(lwis_fence, client);
		list_add(&pending_transaction_id->list_node, &tx_list->list);
#ifdef LWIS_FENCE_DBG
		dev_info(client->lwis_dev->dev,
			 "lwis_fence transaction id %llu added to its trigger fence fd %d ",
			 transaction->info.id, lwis_fence->fd);
#endif
	} else {
		dev_err(client->lwis_dev->dev,
			"Bad lwis_fence fd-%d already signaled with error code %d \n", fence_fd,
			lwis_fence->status);
		kfree(pending_transaction_id);
		fput(fp);
		ret = -EINVAL;
	}
	mutex_unlock(&lwis_fence->lock);
	return ret;
}


bool lwis_triggered_by_condition(struct lwis_transaction *transaction)
{
	return (transaction->info.trigger_condition.num_nodes > 0);
}

bool lwis_event_triggered_condition_ready(struct lwis_transaction *transaction,
					  struct lwis_transaction *weak_transaction,
					  int64_t event_id, int64_t event_counter)
{
	int32_t operator_type;
	size_t all_signaled;
	struct lwis_transaction_info *info = &transaction->info;
	int i;

	operator_type = info->trigger_condition.operator_type;
	all_signaled = info->trigger_condition.num_nodes;

	for (i = 0; i < info->trigger_condition.num_nodes; i++) {
		if (info->trigger_condition.trigger_nodes[i].type == LWIS_TRIGGER_EVENT &&
		    info->trigger_condition.trigger_nodes[i].event.id == event_id &&
		    info->trigger_condition.trigger_nodes[i].event.counter == event_counter) {
			transaction->signaled_count++;
			list_del(&weak_transaction->event_list_node);
			kfree(weak_transaction);
			break;
		}
	}

	if (i >= info->trigger_condition.num_nodes) {
		/* No event counter is matched */
		return false;
	}

	if (operator_type == LWIS_TRIGGER_NODE_OPERATOR_AND &&
	    transaction->signaled_count == all_signaled) {
		return true;
	} else if (operator_type == LWIS_TRIGGER_NODE_OPERATOR_OR) {
		return true;
	} else if (operator_type == LWIS_TRIGGER_NODE_OPERATOR_NONE) {
		return true;
	}

	return false;
}

bool lwis_fence_triggered_condition_ready(struct lwis_transaction *transaction,
					  struct lwis_fence *fence)
{
	int32_t operator_type;
	size_t all_signaled;

	operator_type = transaction->info.trigger_condition.operator_type;
	all_signaled = transaction->info.trigger_condition.num_nodes;

	transaction->signaled_count++;
	if ((operator_type == LWIS_TRIGGER_NODE_OPERATOR_AND ||
	     operator_type == LWIS_TRIGGER_NODE_OPERATOR_OR) &&
	    transaction->signaled_count == all_signaled) {
		return true;
	} else if (operator_type == LWIS_TRIGGER_NODE_OPERATOR_AND && fence->status != 0) {
		/*
		   This condition is ready to cancel transaction as long as there is
		   an error condition from fence with operator type "AND".
		   No matter whether all condition nodes are signaled.
		*/
		return true;
	} else if (operator_type == LWIS_TRIGGER_NODE_OPERATOR_OR && fence->status == 0) {
		return true;
	} else if (operator_type == LWIS_TRIGGER_NODE_OPERATOR_NONE) {
		return true;
	}

	return false;
}

int lwis_parse_trigger_condition(struct lwis_client *client,
				 struct lwis_transaction *transaction)
{
	struct lwis_transaction_info *info;
	struct lwis_device *lwis_dev;
	int i, ret;
	int fd_or_err;

	if (!transaction || !client) {
		dev_err(client->lwis_dev->dev, "Invalid lwis transaction\n");
		return -EINVAL;
	}

	info = &transaction->info;
	lwis_dev = client->lwis_dev;

	if (info->trigger_condition.num_nodes > LWIS_TRIGGER_NODES_MAX_NUM) {
		dev_err(lwis_dev->dev,
			"Trigger condition contains %lu node, more than the limit of %d\n",
			info->trigger_condition.num_nodes,
			LWIS_TRIGGER_NODES_MAX_NUM);
		return -EINVAL;
	}

	for (i = 0; i < info->trigger_condition.num_nodes; i++) {
		if (info->trigger_condition.trigger_nodes[i].type ==
				LWIS_TRIGGER_FENCE_PLACEHOLDER) {
			fd_or_err = lwis_fence_create(lwis_dev);
			if (fd_or_err < 0) {
				return fd_or_err;
			}
			info->trigger_condition.trigger_nodes[i].fence_fd = fd_or_err;
		}

		if (info->trigger_condition.trigger_nodes[i].type == LWIS_TRIGGER_EVENT) {
			ret = lwis_trigger_event_add_weak_transaction(
				client, info->id,
				info->trigger_condition.trigger_nodes[i].event.id);
		} else {
			ret = lwis_trigger_fence_add_transaction(
				info->trigger_condition.trigger_nodes[i].fence_fd, client,
				transaction);
		}
		if (ret) {
			return ret;
		}
	}

	return 0;
}

int ioctl_lwis_fence_create(struct lwis_device *lwis_dev, int32_t __user *msg)
{
	int32_t fd_or_err;

	fd_or_err = lwis_fence_create(lwis_dev);
	if (fd_or_err < 0) {
		return fd_or_err;
	}

	if (copy_to_user((void __user *)msg, &fd_or_err, sizeof(int32_t))) {
		dev_err(lwis_dev->dev, "failed to copy to user\n");
		return -EFAULT;
	}

	return 0;
}
