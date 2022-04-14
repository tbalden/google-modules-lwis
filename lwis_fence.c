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

#define LWIS_FENCE_DBG

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
#ifdef LWIS_FENCE_DBG
	dev_info(lwis_fence->lwis_top_dev->dev, "Releasing lwis_fence fd-%d", lwis_fence->fd);
#endif
	if (lwis_fence->status == LWIS_FENCE_STATUS_NOT_SIGNALED) {
		dev_err(lwis_fence->lwis_top_dev->dev,
			"lwis_fence fd-%d release without being signaled", lwis_fence->fd);
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
	struct lwis_fence *lwis_fence = fp->private_data;
	int max_len, read_len;

	if (!lwis_fence) {
		dev_err(lwis_fence->lwis_top_dev->dev, "Cannot find lwis_fence instance\n");
		return -EFAULT;
	}

	max_len = sizeof(lwis_fence->status) - *offset;
	if (len > max_len) {
		len = max_len;
	}

	read_len = len - copy_to_user((void __user *)user_buffer,
				      (void *)&lwis_fence->status + *offset, len);
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
	struct lwis_fence *lwis_fence = fp->private_data;
	if (!lwis_fence) {
		dev_err(lwis_fence->lwis_top_dev->dev, "Cannot find lwis_fence instance\n");
		return -EFAULT;
	}

	if (len != sizeof(lwis_fence->status)) {
		dev_err(lwis_fence->lwis_top_dev->dev,
			"Signal lwis_fence with incorrect buffer length\n");
		return -EINVAL;
	}

	len = len - copy_from_user(&lwis_fence->status, (void __user *)user_buffer, len);
	wake_up_interruptible(&lwis_fence->status_wait_queue);
#ifdef LWIS_FENCE_DBG
	dev_info(lwis_fence->lwis_top_dev->dev, "lwis_fence fd-%d setting status to %d",
		 lwis_fence->fd, lwis_fence->status);
#endif
	return len;
}

/*
 *  lwis_fence_poll: Poll status function of LWIS fence
 */
static unsigned int lwis_fence_poll(struct file *fp, poll_table *wait)
{
	struct lwis_fence *lwis_fence = fp->private_data;
	if (!lwis_fence) {
		dev_err(lwis_fence->lwis_top_dev->dev, "Cannot find lwis_fence instance\n");
		return POLLERR;
	}

	poll_wait(fp, &lwis_fence->status_wait_queue, wait);
	/* Check if the fence is already signaled */
	if (lwis_fence->status != LWIS_FENCE_STATUS_NOT_SIGNALED) {
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