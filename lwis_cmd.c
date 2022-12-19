// SPDX-License-Identifier: GPL-2.0
/*
 * Google LWIS Command packets
 *
 * Copyright (c) 2022 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "lwis_cmd.h"

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "lwis_allocator.h"
#include "lwis_buffer.h"
#include "lwis_commands.h"
#include "lwis_device.h"
#include "lwis_device_dpm.h"
#include "lwis_device_i2c.h"
#include "lwis_device_ioreg.h"
#include "lwis_event.h"
#include "lwis_fence.h"
#include "lwis_i2c.h"
#include "lwis_io_entry.h"
#include "lwis_ioctl.h"
#include "lwis_ioreg.h"
#include "lwis_periodic_io.h"
#include "lwis_platform.h"
#include "lwis_transaction.h"
#include "lwis_util.h"

static int copy_pkt_to_user(struct lwis_device *lwis_dev, void __user *u_msg, void *k_msg,
			    size_t size)
{
	if (copy_to_user(u_msg, k_msg, size)) {
		dev_err(lwis_dev->dev, "Failed to copy %zu bytes to user\n", size);
		return -EFAULT;
	}

	return 0;
}

static int cmd_echo(struct lwis_device *lwis_dev, struct lwis_cmd_pkt *header,
		    struct lwis_cmd_echo __user *u_msg)
{
	struct lwis_cmd_echo echo_msg;
	char *buffer = NULL;

	if (copy_from_user((void *)&echo_msg, (void __user *)u_msg, sizeof(echo_msg))) {
		dev_err(lwis_dev->dev, "Failed to copy %zu bytes from user\n", sizeof(echo_msg));
		return -EFAULT;
	}

	if (echo_msg.msg.size == 0) {
		header->ret_code = 0;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}

	buffer = kmalloc(echo_msg.msg.size + 1, GFP_KERNEL);
	if (!buffer) {
		dev_err(lwis_dev->dev, "Failed to allocate buffer for echo message\n");
		header->ret_code = -ENOMEM;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}
	if (copy_from_user(buffer, (void __user *)echo_msg.msg.msg, echo_msg.msg.size)) {
		dev_err(lwis_dev->dev, "Failed to copy %zu bytes echo message from user\n",
			echo_msg.msg.size);
		kfree(buffer);
		header->ret_code = -EFAULT;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}
	buffer[echo_msg.msg.size] = '\0';

	if (echo_msg.msg.kernel_log) {
		dev_info(lwis_dev->dev, "LWIS_ECHO: %s\n", buffer);
	}
	kfree(buffer);

	header->ret_code = 0;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_time_query(struct lwis_device *lwis_dev, struct lwis_cmd_pkt *header,
			  struct lwis_cmd_time_query __user *u_msg)
{
	struct lwis_cmd_time_query time_query;
	time_query.timestamp_ns = ktime_to_ns(lwis_get_time());
	time_query.header.cmd_id = header->cmd_id;
	time_query.header.next = header->next;
	time_query.header.ret_code = 0;

	return copy_pkt_to_user(lwis_dev, u_msg, (void *)&time_query, sizeof(time_query));
}

static int cmd_get_device_info(struct lwis_device *lwis_dev, struct lwis_cmd_pkt *header,
			       struct lwis_cmd_device_info __user *u_msg)
{
	int i;
	struct lwis_cmd_device_info k_info = { .header.cmd_id = header->cmd_id,
					       .header.next = header->next,
					       .info.id = lwis_dev->id,
					       .info.type = lwis_dev->type,
					       .info.num_clks = 0,
					       .info.num_regs = 0,
					       .info.transaction_worker_thread_pid = -1,
					       .info.periodic_io_thread_pid = -1 };
	strscpy(k_info.info.name, lwis_dev->name, LWIS_MAX_NAME_STRING_LEN);

	if (lwis_dev->clocks) {
		k_info.info.num_clks = lwis_dev->clocks->count;
		for (i = 0; i < lwis_dev->clocks->count; i++) {
			if (i >= LWIS_MAX_CLOCK_NUM) {
				dev_err(lwis_dev->dev,
					"Clock count larger than LWIS_MAX_CLOCK_NUM\n");
				break;
			}
			strscpy(k_info.info.clks[i].name, lwis_dev->clocks->clk[i].name,
				LWIS_MAX_NAME_STRING_LEN);
			k_info.info.clks[i].clk_index = i;
			k_info.info.clks[i].frequency = 0;
		}
	}

	if (lwis_dev->type == DEVICE_TYPE_IOREG) {
		struct lwis_ioreg_device *ioreg_dev;
		ioreg_dev = container_of(lwis_dev, struct lwis_ioreg_device, base_dev);
		if (ioreg_dev->reg_list.count > 0) {
			k_info.info.num_regs = ioreg_dev->reg_list.count;
			for (i = 0; i < ioreg_dev->reg_list.count; i++) {
				if (i >= LWIS_MAX_REG_NUM) {
					dev_err(lwis_dev->dev,
						"Reg count larger than LWIS_MAX_REG_NUM\n");
					break;
				}
				strscpy(k_info.info.regs[i].name, ioreg_dev->reg_list.block[i].name,
					LWIS_MAX_NAME_STRING_LEN);
				k_info.info.regs[i].reg_index = i;
				k_info.info.regs[i].start = ioreg_dev->reg_list.block[i].start;
				k_info.info.regs[i].size = ioreg_dev->reg_list.block[i].size;
			}
		}
	}

	if (lwis_dev->transaction_worker_thread) {
		k_info.info.transaction_worker_thread_pid =
			lwis_dev->transaction_worker_thread->pid;
	}

	k_info.header.ret_code = 0;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)&k_info, sizeof(k_info));
}

static int cmd_device_enable(struct lwis_client *lwis_client, struct lwis_cmd_pkt *header,
			     struct lwis_cmd_pkt __user *u_msg)
{
	int ret = 0;
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;

	if (lwis_client->is_enabled) {
		header->ret_code = 0;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}

	mutex_lock(&lwis_dev->client_lock);
	if (lwis_dev->enabled > 0 && lwis_dev->enabled < INT_MAX) {
		lwis_dev->enabled++;
		lwis_client->is_enabled = true;
		ret = 0;
		goto exit_locked;
	} else if (lwis_dev->enabled == INT_MAX) {
		dev_err(lwis_dev->dev, "Enable counter overflow\n");
		ret = -EINVAL;
		goto exit_locked;
	}

	/* Clear event queues to make sure there is no stale event from
	 * previous session */
	lwis_client_event_queue_clear(lwis_client);
	lwis_client_error_event_queue_clear(lwis_client);

	ret = lwis_dev_power_up_locked(lwis_dev);
	if (ret < 0) {
		dev_err(lwis_dev->dev, "Failed to power up device\n");
		goto exit_locked;
	}

	lwis_dev->enabled++;
	lwis_client->is_enabled = true;
	lwis_dev->is_suspended = false;
	dev_info(lwis_dev->dev, "Device enabled\n");
exit_locked:
	mutex_unlock(&lwis_dev->client_lock);
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_device_disable(struct lwis_client *lwis_client, struct lwis_cmd_pkt *header,
			      struct lwis_cmd_pkt __user *u_msg)
{
	int ret = 0;
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;

	if (!lwis_client->is_enabled) {
		header->ret_code = 0;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}

	mutex_lock(&lwis_dev->client_lock);
	/* Clear event states for this client */
	lwis_client_event_states_clear(lwis_client);
	mutex_unlock(&lwis_dev->client_lock);

	/* Flush all periodic io to complete */
	ret = lwis_periodic_io_client_flush(lwis_client);
	if (ret) {
		dev_err(lwis_dev->dev, "Failed to wait for in-process periodic io to complete\n");
	}

	/* Flush all pending transactions */
	ret = lwis_transaction_client_flush(lwis_client);
	if (ret) {
		dev_err(lwis_dev->dev, "Failed to flush pending transactions\n");
	}

	/* Run cleanup transactions. */
	lwis_transaction_client_cleanup(lwis_client);

	mutex_lock(&lwis_dev->client_lock);
	if (lwis_dev->enabled > 1) {
		lwis_dev->enabled--;
		lwis_client->is_enabled = false;
		ret = 0;
		goto exit_locked;
	} else if (lwis_dev->enabled <= 0) {
		dev_err(lwis_dev->dev, "Disabling a device that is already disabled\n");
		ret = -EINVAL;
		goto exit_locked;
	}

	ret = lwis_dev_power_down_locked(lwis_dev);
	if (ret < 0) {
		dev_err(lwis_dev->dev, "Failed to power down device\n");
		goto exit_locked;
	}
	lwis_device_event_states_clear_locked(lwis_dev);

	lwis_dev->enabled--;
	lwis_client->is_enabled = false;
	lwis_dev->is_suspended = false;
	dev_info(lwis_dev->dev, "Device disabled\n");
exit_locked:
	mutex_unlock(&lwis_dev->client_lock);
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int copy_io_entries_from_cmd(struct lwis_device *lwis_dev,
				    struct lwis_cmd_io_entries __user *u_msg,
				    struct lwis_cmd_io_entries *k_msg,
				    struct lwis_io_entry **k_entries)
{
	struct lwis_io_entry *io_entries;
	uint32_t buf_size;

	/* Register io is not supported for the lwis device, return */
	if (!lwis_dev->vops.register_io) {
		dev_err(lwis_dev->dev, "Register IO not supported on this LWIS device\n");
		return -EINVAL;
	}

	/* Copy io_entries from userspace */
	if (copy_from_user(k_msg, (void __user *)u_msg, sizeof(*k_msg))) {
		dev_err(lwis_dev->dev, "Failed to copy io_entries header from userspace.\n");
		return -EFAULT;
	}
	buf_size = sizeof(struct lwis_io_entry) * k_msg->io.num_io_entries;
	if (buf_size / sizeof(struct lwis_io_entry) != k_msg->io.num_io_entries) {
		dev_err(lwis_dev->dev, "Failed to copy io_entries due to integer overflow.\n");
		return -EOVERFLOW;
	}
	io_entries = lwis_allocator_allocate(lwis_dev, buf_size);
	if (!io_entries) {
		dev_err(lwis_dev->dev, "Failed to allocate io_entries buffer\n");
		return -ENOMEM;
	}
	if (copy_from_user(io_entries, (void __user *)k_msg->io.io_entries, buf_size)) {
		dev_err(lwis_dev->dev, "Failed to copy io_entries from userspace.\n");
		lwis_allocator_free(lwis_dev, io_entries);
		return -EFAULT;
	}
	*k_entries = io_entries;

	return 0;
}

static int cmd_device_reset(struct lwis_client *lwis_client, struct lwis_cmd_pkt *header,
			    struct lwis_cmd_io_entries __user *u_msg)
{
	int ret = 0;
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;
	struct lwis_cmd_io_entries k_msg;
	struct lwis_io_entry *k_entries = NULL;
	unsigned long flags;
	bool device_enabled = false;

	ret = copy_io_entries_from_cmd(lwis_dev, u_msg, &k_msg, &k_entries);
	if (ret) {
		goto soft_reset_exit;
	}

	/* Clear event states, event queues and transactions for this client */
	mutex_lock(&lwis_dev->client_lock);
	lwis_client_event_states_clear(lwis_client);
	lwis_client_event_queue_clear(lwis_client);
	lwis_client_error_event_queue_clear(lwis_client);
	device_enabled = lwis_dev->enabled;
	mutex_unlock(&lwis_dev->client_lock);

	/* Flush all periodic io to complete */
	ret = lwis_periodic_io_client_flush(lwis_client);
	if (ret) {
		dev_err(lwis_dev->dev, "Failed to wait for in-process periodic io to complete\n");
	}

	/* Flush all pending transactions */
	ret = lwis_transaction_client_flush(lwis_client);
	if (ret) {
		dev_err(lwis_dev->dev, "Failed to flush all pending transactions\n");
	}

	/* Perform reset routine defined by the io_entries */
	if (device_enabled) {
		ret = lwis_ioctl_util_synchronous_process_io_entries(
			lwis_dev, k_msg.io.num_io_entries, k_entries, k_msg.io.io_entries);
	} else {
		dev_warn(lwis_dev->dev,
			 "Device is not enabled, IoEntries will not be executed in DEVICE_RESET\n");
	}

	spin_lock_irqsave(&lwis_dev->lock, flags);
	lwis_device_event_states_clear_locked(lwis_dev);
	spin_unlock_irqrestore(&lwis_dev->lock, flags);
soft_reset_exit:
	if (k_entries) {
		lwis_allocator_free(lwis_dev, k_entries);
	}
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_device_suspend(struct lwis_client *lwis_client, struct lwis_cmd_pkt *header,
			      struct lwis_cmd_pkt __user *u_msg)
{
	int ret = 0;
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;

	if (!lwis_dev->suspend_sequence) {
		dev_err(lwis_dev->dev, "No suspend sequence defined\n");
		header->ret_code = -EINVAL;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}

	if (!lwis_client->is_enabled) {
		dev_err(lwis_dev->dev, "Trying to suspend a disabled device\n");
		header->ret_code = -EINVAL;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}

	if (lwis_dev->is_suspended) {
		header->ret_code = 0;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}

	mutex_lock(&lwis_dev->client_lock);
	/* Clear event states for this client */
	lwis_client_event_states_clear(lwis_client);
	mutex_unlock(&lwis_dev->client_lock);

	/* Flush all periodic io to complete */
	ret = lwis_periodic_io_client_flush(lwis_client);
	if (ret) {
		dev_err(lwis_dev->dev, "Failed to wait for in-process periodic io to complete\n");
	}

	/* Flush all pending transactions */
	ret = lwis_transaction_client_flush(lwis_client);
	if (ret) {
		dev_err(lwis_dev->dev, "Failed to flush pending transactions\n");
	}

	/* Run cleanup transactions. */
	lwis_transaction_client_cleanup(lwis_client);

	mutex_lock(&lwis_dev->client_lock);
	ret = lwis_dev_process_power_sequence(lwis_dev, lwis_dev->suspend_sequence,
					      /*set_active=*/false, /*skip_error=*/false);
	if (ret) {
		dev_err(lwis_dev->dev, "Error lwis_dev_process_power_sequence (%d)\n", ret);
		goto exit_locked;
	}

	lwis_device_event_states_clear_locked(lwis_dev);

	lwis_dev->is_suspended = true;
	dev_info(lwis_dev->dev, "Device suspended\n");
exit_locked:
	mutex_unlock(&lwis_dev->client_lock);
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_device_resume(struct lwis_client *lwis_client, struct lwis_cmd_pkt *header,
			     struct lwis_cmd_pkt __user *u_msg)
{
	int ret = 0;
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;

	if (!lwis_dev->resume_sequence) {
		dev_err(lwis_dev->dev, "No resume sequence defined\n");
		header->ret_code = -EINVAL;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}

	if (!lwis_dev->is_suspended) {
		header->ret_code = 0;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}

	mutex_lock(&lwis_dev->client_lock);
	/* Clear event queues to make sure there is no stale event from
	 * previous session */
	lwis_client_event_queue_clear(lwis_client);
	lwis_client_error_event_queue_clear(lwis_client);

	ret = lwis_dev_process_power_sequence(lwis_dev, lwis_dev->resume_sequence,
					      /*set_active=*/true, /*skip_error=*/false);
	if (ret) {
		dev_err(lwis_dev->dev, "Error lwis_dev_process_power_sequence (%d)\n", ret);
		goto exit_locked;
	}

	lwis_dev->is_suspended = false;
	dev_info(lwis_dev->dev, "Device resumed\n");
exit_locked:
	mutex_unlock(&lwis_dev->client_lock);
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_dma_buffer_enroll(struct lwis_client *lwis_client, struct lwis_cmd_pkt *header,
				 struct lwis_cmd_dma_buffer_enroll __user *u_msg)
{
	int ret = 0;
	struct lwis_cmd_dma_buffer_enroll buf_info;
	struct lwis_enrolled_buffer *buffer;
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;

	buffer = kmalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		dev_err(lwis_dev->dev, "Failed to allocate lwis_enrolled_buffer struct\n");
		header->ret_code = -ENOMEM;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}

	if (copy_from_user((void *)&buf_info, (void __user *)u_msg, sizeof(buf_info))) {
		dev_err(lwis_dev->dev, "Failed to copy %zu bytes from user\n", sizeof(buf_info));
		ret = -EFAULT;
		goto error_enroll;
	}

	buffer->info.fd = buf_info.info.fd;
	buffer->info.dma_read = buf_info.info.dma_read;
	buffer->info.dma_write = buf_info.info.dma_write;

	ret = lwis_buffer_enroll(lwis_client, buffer);
	if (ret) {
		dev_err(lwis_dev->dev, "Failed to enroll buffer\n");
		goto error_enroll;
	}

	buf_info.info.dma_vaddr = buffer->info.dma_vaddr;
	buf_info.header.cmd_id = header->cmd_id;
	buf_info.header.next = header->next;
	buf_info.header.ret_code = ret;
	ret = copy_pkt_to_user(lwis_dev, u_msg, (void *)&buf_info, sizeof(buf_info));
	if (ret) {
		lwis_buffer_disenroll(lwis_client, buffer);
		goto error_enroll;
	}

	return ret;

error_enroll:
	kfree(buffer);
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_dma_buffer_disenroll(struct lwis_client *lwis_client, struct lwis_cmd_pkt *header,
				    struct lwis_cmd_dma_buffer_disenroll __user *u_msg)
{
	int ret = 0;
	struct lwis_cmd_dma_buffer_disenroll info;
	struct lwis_enrolled_buffer *buffer;
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;

	if (copy_from_user((void *)&info, (void __user *)u_msg, sizeof(info))) {
		dev_err(lwis_dev->dev, "Failed to copy DMA virtual address from user\n");
		return -EFAULT;
	}

	buffer = lwis_client_enrolled_buffer_find(lwis_client, info.info.fd, info.info.dma_vaddr);
	if (!buffer) {
		dev_err(lwis_dev->dev, "Failed to find dma buffer for fd %d vaddr %pad\n",
			info.info.fd, &info.info.dma_vaddr);
		header->ret_code = -ENOENT;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}

	ret = lwis_buffer_disenroll(lwis_client, buffer);
	if (ret) {
		dev_err(lwis_dev->dev, "Failed to disenroll dma buffer for fd %d vaddr %pad\n",
			info.info.fd, &info.info.dma_vaddr);
		header->ret_code = ret;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}

	kfree(buffer);
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_dma_buffer_cpu_access(struct lwis_client *lwis_client, struct lwis_cmd_pkt *header,
				     struct lwis_cmd_dma_buffer_cpu_access __user *u_msg)
{
	int ret = 0;
	struct lwis_cmd_dma_buffer_cpu_access op;
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;

	if (copy_from_user((void *)&op, (void __user *)u_msg, sizeof(op))) {
		dev_err(lwis_dev->dev, "Failed to copy buffer CPU access operation from user\n");
		return -EFAULT;
	}

	ret = lwis_buffer_cpu_access(lwis_client, &op.op);
	if (ret) {
		dev_err(lwis_dev->dev, "Failed to prepare for cpu access for fd %d\n", op.op.fd);
	}

	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_dma_buffer_alloc(struct lwis_client *lwis_client, struct lwis_cmd_pkt *header,
				struct lwis_cmd_dma_buffer_alloc __user *u_msg)
{
	int ret = 0;
	struct lwis_cmd_dma_buffer_alloc alloc_info;
	struct lwis_allocated_buffer *buffer;
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;

	buffer = kmalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		dev_err(lwis_dev->dev, "Failed to allocated lwis_allocated_buffer\n");
		return -ENOMEM;
	}

	if (copy_from_user((void *)&alloc_info, (void __user *)u_msg, sizeof(alloc_info))) {
		dev_err(lwis_dev->dev, "Failed to copy %zu bytes from user\n", sizeof(alloc_info));
		ret = -EFAULT;
		goto error_alloc;
	}

	ret = lwis_buffer_alloc(lwis_client, &alloc_info.info, buffer);
	if (ret) {
		dev_err(lwis_dev->dev, "Failed to allocate buffer\n");
		goto error_alloc;
	}

	alloc_info.header.ret_code = 0;
	ret = copy_pkt_to_user(lwis_dev, u_msg, (void *)&alloc_info, sizeof(alloc_info));
	if (ret) {
		lwis_buffer_free(lwis_client, buffer);
		ret = -EFAULT;
		goto error_alloc;
	}

	return ret;

error_alloc:
	kfree(buffer);
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_dma_buffer_free(struct lwis_client *lwis_client, struct lwis_cmd_pkt *header,
			       struct lwis_cmd_dma_buffer_free __user *u_msg)
{
	int ret = 0;
	struct lwis_cmd_dma_buffer_free info;
	struct lwis_allocated_buffer *buffer;
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;

	if (copy_from_user((void *)&info, (void __user *)u_msg, sizeof(info))) {
		dev_err(lwis_dev->dev, "Failed to copy file descriptor from user\n");
		return -EFAULT;
	}

	buffer = lwis_client_allocated_buffer_find(lwis_client, info.fd);
	if (!buffer) {
		dev_err(lwis_dev->dev, "Cannot find allocated buffer FD %d\n", info.fd);
		header->ret_code = -ENOENT;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}

	ret = lwis_buffer_free(lwis_client, buffer);
	if (ret) {
		dev_err(lwis_dev->dev, "Failed to free buffer FD %d\n", info.fd);
		header->ret_code = ret;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}

	kfree(buffer);

	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_reg_io(struct lwis_device *lwis_dev, struct lwis_cmd_pkt *header,
		      struct lwis_cmd_io_entries __user *u_msg)
{
	int ret = 0;
	struct lwis_cmd_io_entries k_msg;
	struct lwis_io_entry *k_entries = NULL;

	ret = copy_io_entries_from_cmd(lwis_dev, u_msg, &k_msg, &k_entries);
	if (ret) {
		goto reg_io_exit;
	}

	/* Walk through and execute the entries */
	ret = lwis_ioctl_util_synchronous_process_io_entries(lwis_dev, k_msg.io.num_io_entries,
							     k_entries, k_msg.io.io_entries);

reg_io_exit:
	if (k_entries) {
		lwis_allocator_free(lwis_dev, k_entries);
	}
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_event_control_get(struct lwis_client *lwis_client, struct lwis_cmd_pkt *header,
				 struct lwis_cmd_event_control_get __user *u_msg)
{
	int ret = 0;
	struct lwis_cmd_event_control_get control;
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;

	if (copy_from_user((void *)&control, (void __user *)u_msg, sizeof(control))) {
		dev_err(lwis_dev->dev, "Failed to copy %zu bytes from user\n", sizeof(control));
		return -EFAULT;
	}

	ret = lwis_client_event_control_get(lwis_client, control.ctl.event_id, &control.ctl);
	if (ret) {
		dev_err(lwis_dev->dev, "Failed to get event: %lld (err:%d)\n", control.ctl.event_id,
			ret);
		header->ret_code = ret;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}

	control.header.ret_code = 0;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)&control, sizeof(control));
}

static int cmd_event_control_set(struct lwis_client *lwis_client, struct lwis_cmd_pkt *header,
				 struct lwis_cmd_event_control_set __user *u_msg)
{
	struct lwis_cmd_event_control_set k_msg;
	struct lwis_event_control *k_event_controls;
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;
	int ret = 0;
	int i;
	size_t buf_size;

	if (copy_from_user((void *)&k_msg, (void __user *)u_msg, sizeof(k_msg))) {
		dev_err(lwis_dev->dev, "Failed to copy ioctl message from user\n");
		return -EFAULT;
	}

	/*  Copy event controls from user buffer. */
	buf_size = sizeof(struct lwis_event_control) * k_msg.list.num_event_controls;
	if (buf_size / sizeof(struct lwis_event_control) != k_msg.list.num_event_controls) {
		dev_err(lwis_dev->dev, "Failed to copy event controls due to integer overflow.\n");
		header->ret_code = -EOVERFLOW;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}
	k_event_controls = kmalloc(buf_size, GFP_KERNEL);
	if (!k_event_controls) {
		dev_err(lwis_dev->dev, "Failed to allocate event controls\n");
		header->ret_code = -ENOMEM;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	}
	if (copy_from_user(k_event_controls, (void __user *)k_msg.list.event_controls, buf_size)) {
		dev_err(lwis_dev->dev, "Failed to copy event controls from user\n");
		ret = -EFAULT;
		goto exit;
	}

	for (i = 0; i < k_msg.list.num_event_controls; i++) {
		ret = lwis_client_event_control_set(lwis_client, &k_event_controls[i]);
		if (ret) {
			dev_err(lwis_dev->dev, "Failed to apply event control 0x%llx\n",
				k_event_controls[i].event_id);
			goto exit;
		}
	}
exit:
	kfree(k_event_controls);
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_event_dequeue(struct lwis_client *lwis_client, struct lwis_cmd_pkt *header,
			     struct lwis_cmd_event_dequeue __user *u_msg)
{
	struct lwis_cmd_event_dequeue info;
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;
	struct lwis_event_entry *event;
	int ret = 0;
	int err = 0;
	bool is_error_event = false;

	if (copy_from_user((void *)&info, (void __user *)u_msg, sizeof(info))) {
		dev_err(lwis_dev->dev, "Failed to copy %zu bytes from user\n", sizeof(info));
		return -EFAULT;
	}

	mutex_lock(&lwis_dev->client_lock);
	/* Peek at the front element of error event queue first */
	ret = lwis_client_error_event_peek_front(lwis_client, &event);
	if (ret == 0) {
		is_error_event = true;
	} else if (ret != -ENOENT) {
		dev_err(lwis_dev->dev, "Error dequeueing error event: %d\n", ret);
		mutex_unlock(&lwis_dev->client_lock);
		header->ret_code = ret;
		return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
	} else {
		/* Nothing at error event queue, continue to check normal
		 * event queue */
		ret = lwis_client_event_peek_front(lwis_client, &event);
		if (ret) {
			if (ret != -ENOENT) {
				dev_err(lwis_dev->dev, "Error dequeueing event: %d\n", ret);
			}
			mutex_unlock(&lwis_dev->client_lock);
			header->ret_code = ret;
			return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
		}
	}

	/* We need to check if we have an adequate payload buffer */
	if (event->event_info.payload_size > info.info.payload_buffer_size) {
		/* Nope, we don't. Let's inform the user and bail */
		info.info.payload_size = event->event_info.payload_size;
		err = -EAGAIN;
	} else {
		info.info.event_id = event->event_info.event_id;
		info.info.event_counter = event->event_info.event_counter;
		info.info.timestamp_ns = event->event_info.timestamp_ns;
		info.info.payload_size = event->event_info.payload_size;

		/* Here we have a payload and the buffer is big enough */
		if (event->event_info.payload_size > 0 && info.info.payload_buffer) {
			/* Copy over the payload buffer to userspace */
			if (copy_to_user((void __user *)info.info.payload_buffer,
					 (void *)event->event_info.payload_buffer,
					 event->event_info.payload_size)) {
				dev_err(lwis_dev->dev, "Failed to copy %zu bytes to user\n",
					event->event_info.payload_size);
				mutex_unlock(&lwis_dev->client_lock);
				return -EFAULT;
			}
		}
	}
	/* If we didn't -EAGAIN up above, we can pop and discard the front of
	 * the event queue because we're done dealing with it. If we got the
	 * -EAGAIN case, we didn't actually dequeue this event and userspace
	 * should try again with a bigger payload_buffer.
	 */
	if (!err) {
		if (is_error_event) {
			ret = lwis_client_error_event_pop_front(lwis_client, NULL);
		} else {
			ret = lwis_client_event_pop_front(lwis_client, NULL);
		}
		if (ret) {
			dev_err(lwis_dev->dev, "Error dequeueing event: %d\n", ret);
			mutex_unlock(&lwis_dev->client_lock);
			header->ret_code = ret;
			return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
		}
	}
	mutex_unlock(&lwis_dev->client_lock);
	/* Now let's copy the actual info struct back to user */
	info.header.ret_code = err;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)&info, sizeof(info));
}

static int construct_transaction_from_cmd(struct lwis_client *client,
					  struct lwis_cmd_transaction_info __user *u_msg,
					  struct lwis_transaction **transaction)
{
	int ret;
	struct lwis_cmd_transaction_info k_info;
	struct lwis_transaction *k_transaction;
	struct lwis_device *lwis_dev = client->lwis_dev;

	k_transaction = kmalloc(sizeof(*k_transaction), GFP_KERNEL);
	if (!k_transaction) {
		dev_err(lwis_dev->dev, "Failed to allocate transaction info\n");
		return -ENOMEM;
	}

	if (copy_from_user((void *)&k_info, (void __user *)u_msg, sizeof(k_info))) {
		dev_err(lwis_dev->dev, "Failed to copy transaction info from user\n");
		ret = -EFAULT;
		goto error_free_transaction;
	}

	memcpy(&k_transaction->info, &k_info.info, sizeof(k_transaction->info));

	ret = lwis_ioctl_util_construct_io_entry(client, k_transaction->info.io_entries,
						 k_transaction->info.num_io_entries,
						 &k_transaction->info.io_entries);
	if (ret) {
		dev_err(lwis_dev->dev, "Failed to prepare lwis io entries for transaction\n");
		goto error_free_transaction;
	}

	k_transaction->resp = NULL;
	k_transaction->is_weak_transaction = false;
	INIT_LIST_HEAD(&k_transaction->event_list_node);
	INIT_LIST_HEAD(&k_transaction->process_queue_node);
	INIT_LIST_HEAD(&k_transaction->completion_fence_list);

	*transaction = k_transaction;
	return 0;

error_free_transaction:
	kfree(k_transaction);
	return ret;
}

static int cmd_transaction_submit(struct lwis_client *client, struct lwis_cmd_pkt *header,
				  struct lwis_cmd_transaction_info __user *u_msg)
{
	struct lwis_transaction *k_transaction = NULL;
	struct lwis_cmd_transaction_info k_transaction_info;
	struct lwis_device *lwis_dev = client->lwis_dev;
	int ret = 0;
	unsigned long flags;

	if (lwis_dev->type == DEVICE_TYPE_SLC || lwis_dev->type == DEVICE_TYPE_DPM) {
		dev_err(lwis_dev->dev, "not supported device type: %d\n", lwis_dev->type);
		ret = -EINVAL;
		goto err_exit;
	}

	ret = construct_transaction_from_cmd(client, u_msg, &k_transaction);
	if (ret) {
		goto err_exit;
	}

	ret = lwis_initialize_transaction_fences(client, k_transaction);
	if (ret) {
		lwis_transaction_free(lwis_dev, k_transaction);
		goto err_exit;
	}

	spin_lock_irqsave(&client->transaction_lock, flags);
	ret = lwis_transaction_submit_locked(client, k_transaction);
	k_transaction_info.info = k_transaction->info;
	spin_unlock_irqrestore(&client->transaction_lock, flags);
	if (ret) {
		k_transaction_info.info.id = LWIS_ID_INVALID;
		lwis_transaction_free(lwis_dev, k_transaction);
	}

	k_transaction_info.header.cmd_id = header->cmd_id;
	k_transaction_info.header.next = header->next;
	k_transaction_info.header.ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)&k_transaction_info,
				sizeof(k_transaction_info));

err_exit:
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_transaction_cancel(struct lwis_client *client, struct lwis_cmd_pkt *header,
				  struct lwis_cmd_transaction_cancel __user *u_msg)
{
	int ret = 0;
	struct lwis_cmd_transaction_cancel k_msg;
	struct lwis_device *lwis_dev = client->lwis_dev;

	if (copy_from_user((void *)&k_msg, (void __user *)u_msg, sizeof(k_msg))) {
		dev_err(lwis_dev->dev, "Failed to copy transaction ID from user\n");
		return -EFAULT;
	}

	ret = lwis_transaction_cancel(client, k_msg.id);
	if (ret) {
		dev_warn_ratelimited(lwis_dev->dev, "Failed to cancel transaction id 0x%llx (%d)\n",
				     k_msg.id, ret);
	}

	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_transaction_replace(struct lwis_client *client, struct lwis_cmd_pkt *header,
				   struct lwis_cmd_transaction_info __user *u_msg)
{
	struct lwis_transaction *k_transaction = NULL;
	struct lwis_cmd_transaction_info k_transaction_info;
	struct lwis_device *lwis_dev = client->lwis_dev;
	int ret = 0;
	unsigned long flags;

	ret = construct_transaction_from_cmd(client, u_msg, &k_transaction);
	if (ret) {
		goto err_exit;
	}

	ret = lwis_initialize_transaction_fences(client, k_transaction);
	if (ret) {
		lwis_transaction_free(lwis_dev, k_transaction);
		goto err_exit;
	}

	spin_lock_irqsave(&client->transaction_lock, flags);
	ret = lwis_transaction_replace_locked(client, k_transaction);
	k_transaction_info.info = k_transaction->info;
	spin_unlock_irqrestore(&client->transaction_lock, flags);
	if (ret) {
		k_transaction_info.info.id = LWIS_ID_INVALID;
		lwis_transaction_free(lwis_dev, k_transaction);
	}

	k_transaction_info.header.cmd_id = header->cmd_id;
	k_transaction_info.header.next = header->next;
	k_transaction_info.header.ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)&k_transaction_info,
				sizeof(k_transaction_info));

err_exit:
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int construct_periodic_io_from_cmd(struct lwis_client *client,
					  struct lwis_cmd_periodic_io_info __user *u_msg,
					  struct lwis_periodic_io **periodic_io)
{
	int ret = 0;
	struct lwis_periodic_io *k_periodic_io;
	struct lwis_cmd_periodic_io_info k_info;
	struct lwis_device *lwis_dev = client->lwis_dev;

	k_periodic_io = kmalloc(sizeof(struct lwis_periodic_io), GFP_KERNEL);
	if (!k_periodic_io) {
		dev_err(lwis_dev->dev, "Failed to allocate periodic io\n");
		return -ENOMEM;
	}

	if (copy_from_user((void *)&k_info, (void __user *)u_msg, sizeof(k_info))) {
		dev_err(lwis_dev->dev, "Failed to copy periodic io info from user\n");
		ret = -EFAULT;
		goto error_free_periodic_io;
	}

	memcpy(&k_periodic_io->info, &k_info.info, sizeof(k_periodic_io->info));

	ret = lwis_ioctl_util_construct_io_entry(client, k_periodic_io->info.io_entries,
						 k_periodic_io->info.num_io_entries,
						 &k_periodic_io->info.io_entries);
	if (ret) {
		dev_err(lwis_dev->dev, "Failed to prepare lwis io entries for periodic io\n");
		goto error_free_periodic_io;
	}

	k_periodic_io->resp = NULL;
	k_periodic_io->periodic_io_list = NULL;

	*periodic_io = k_periodic_io;
	return 0;

error_free_periodic_io:
	kfree(k_periodic_io);
	return ret;
}

static int cmd_periodic_io_submit(struct lwis_client *client, struct lwis_cmd_pkt *header,
				  struct lwis_cmd_periodic_io_info __user *u_msg)
{
	int ret = 0;
	struct lwis_cmd_periodic_io_info k_periodic_io_info;
	struct lwis_periodic_io *k_periodic_io = NULL;
	struct lwis_device *lwis_dev = client->lwis_dev;

	ret = construct_periodic_io_from_cmd(client, u_msg, &k_periodic_io);
	if (ret) {
		goto err_exit;
	}

	ret = lwis_periodic_io_submit(client, k_periodic_io);
	k_periodic_io_info.info = k_periodic_io->info;
	if (ret) {
		k_periodic_io_info.info.id = LWIS_ID_INVALID;
		lwis_periodic_io_free(lwis_dev, k_periodic_io);
		goto err_exit;
	}

	k_periodic_io_info.header.cmd_id = header->cmd_id;
	k_periodic_io_info.header.next = header->next;
	k_periodic_io_info.header.ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)&k_periodic_io_info,
				sizeof(k_periodic_io_info));

err_exit:
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_periodic_io_cancel(struct lwis_client *client, struct lwis_cmd_pkt *header,
				  struct lwis_cmd_periodic_io_cancel __user *u_msg)
{
	int ret = 0;
	struct lwis_cmd_periodic_io_cancel k_msg;
	struct lwis_device *lwis_dev = client->lwis_dev;

	if (copy_from_user((void *)&k_msg, (void __user *)u_msg, sizeof(k_msg))) {
		dev_err(lwis_dev->dev, "Failed to copy periodic io ID from user\n");
		return -EFAULT;
	}

	ret = lwis_periodic_io_cancel(client, k_msg.id);
	if (ret) {
		dev_err_ratelimited(lwis_dev->dev, "Failed to clear periodic io id 0x%llx\n",
				    k_msg.id);
	}

	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_dpm_clk_update(struct lwis_device *lwis_dev, struct lwis_cmd_pkt *header,
			      struct lwis_cmd_dpm_clk_update __user *u_msg)
{
	int ret;
	struct lwis_cmd_dpm_clk_update k_msg;
	struct lwis_clk_setting *clk_settings;
	size_t buf_size;

	if (copy_from_user((void *)&k_msg, (void __user *)u_msg, sizeof(k_msg))) {
		dev_err(lwis_dev->dev, "Failed to copy ioctl message from user\n");
		return -EFAULT;
	}

	buf_size = sizeof(struct lwis_clk_setting) * k_msg.settings.num_settings;
	if (buf_size / sizeof(struct lwis_clk_setting) != k_msg.settings.num_settings) {
		dev_err(lwis_dev->dev, "Failed to copy clk settings due to integer overflow.\n");
		ret = -EOVERFLOW;
		goto exit;
	}
	clk_settings = kmalloc(buf_size, GFP_KERNEL);
	if (!clk_settings) {
		dev_err(lwis_dev->dev, "Failed to allocate clock settings\n");
		ret = -ENOMEM;
		goto exit;
	}

	if (copy_from_user(clk_settings, (void __user *)k_msg.settings.settings, buf_size)) {
		dev_err(lwis_dev->dev, "Failed to copy clk settings from user\n");
		kfree(clk_settings);
		ret = -EFAULT;
		goto exit;
	}

	ret = lwis_dpm_update_clock(lwis_dev, clk_settings, k_msg.settings.num_settings);
	kfree(clk_settings);
exit:
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_dpm_qos_update(struct lwis_device *lwis_dev, struct lwis_cmd_pkt *header,
			      struct lwis_cmd_dpm_qos_update __user *u_msg)
{
	struct lwis_cmd_dpm_qos_update k_msg;
	struct lwis_qos_setting *k_qos_settings;
	int ret = 0;
	int i;
	size_t buf_size;

	if (lwis_dev->type != DEVICE_TYPE_DPM) {
		dev_err(lwis_dev->dev, "not supported device type: %d\n", lwis_dev->type);
		ret = -EINVAL;
		goto exit;
	}

	if (copy_from_user((void *)&k_msg, (void __user *)u_msg, sizeof(k_msg))) {
		dev_err(lwis_dev->dev, "Failed to copy ioctl message from user\n");
		return -EFAULT;
	}

	// Copy qos settings from user buffer.
	buf_size = sizeof(struct lwis_qos_setting) * k_msg.reqs.num_settings;
	if (buf_size / sizeof(struct lwis_qos_setting) != k_msg.reqs.num_settings) {
		dev_err(lwis_dev->dev, "Failed to copy qos settings due to integer overflow.\n");
		ret = -EOVERFLOW;
		goto exit;
	}
	k_qos_settings = kmalloc(buf_size, GFP_KERNEL);
	if (!k_qos_settings) {
		dev_err(lwis_dev->dev, "Failed to allocate qos settings\n");
		ret = -ENOMEM;
		goto exit;
	}
	if (copy_from_user(k_qos_settings, (void __user *)k_msg.reqs.qos_settings, buf_size)) {
		dev_err(lwis_dev->dev, "Failed to copy clk settings from user\n");
		kfree(k_qos_settings);
		ret = -EFAULT;
		goto exit;
	}

	for (i = 0; i < k_msg.reqs.num_settings; i++) {
		ret = lwis_dpm_update_qos(lwis_dev, &k_qos_settings[i]);
		if (ret) {
			dev_err(lwis_dev->dev, "Failed to apply qos setting, ret: %d\n", ret);
			kfree(k_qos_settings);
			goto exit;
		}
	}
	kfree(k_qos_settings);
exit:
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

static int cmd_dpm_get_clock(struct lwis_device *lwis_dev, struct lwis_cmd_pkt *header,
			     struct lwis_cmd_dpm_clk_get __user *u_msg)
{
	struct lwis_cmd_dpm_clk_get current_setting;
	struct lwis_device *target_device;
	int ret = 0;

	if (lwis_dev->type != DEVICE_TYPE_DPM) {
		dev_err(lwis_dev->dev, "not supported device type: %d\n", lwis_dev->type);
		ret = -EINVAL;
		goto err_exit;
	}

	if (copy_from_user((void *)&current_setting, (void __user *)u_msg,
			   sizeof(current_setting))) {
		dev_err(lwis_dev->dev, "failed to copy from user\n");
		return -EFAULT;
	}

	target_device = lwis_find_dev_by_id(current_setting.setting.device_id);
	if (!target_device) {
		dev_err(lwis_dev->dev, "could not find lwis device by id %d\n",
			current_setting.setting.device_id);
		ret = -ENODEV;
		goto err_exit;
	}

	if (target_device->enabled == 0 && target_device->type != DEVICE_TYPE_DPM) {
		dev_warn(target_device->dev, "%s disabled, can't get clk\n", target_device->name);
		ret = -EPERM;
		goto err_exit;
	}

	current_setting.setting.frequency_hz = (int64_t)lwis_dpm_read_clock(target_device);
	current_setting.header.ret_code = 0;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)&current_setting, sizeof(current_setting));

err_exit:
	header->ret_code = ret;
	return copy_pkt_to_user(lwis_dev, u_msg, (void *)header, sizeof(*header));
}

int lwis_ioctl_handle_cmd_pkt(struct lwis_client *lwis_client, struct lwis_cmd_pkt __user *user_msg)
{
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;
	struct lwis_cmd_pkt header;
	int ret = 0;

	while (user_msg) {
		/* Copy cmd packet header from userspace */
		if (copy_from_user(&header, (void __user *)user_msg, sizeof(header))) {
			dev_err(lwis_dev->dev,
				"Failed to copy cmd packet header from userspace.\n");
			return -EFAULT;
		}

		switch (header.cmd_id) {
		case LWIS_CMD_ID_ECHO:
			ret = cmd_echo(lwis_dev, &header, (struct lwis_cmd_echo __user *)user_msg);
			break;
		case LWIS_CMD_ID_TIME_QUERY:
			ret = cmd_time_query(lwis_dev, &header,
					     (struct lwis_cmd_time_query __user *)user_msg);
			break;
		case LWIS_CMD_ID_GET_DEVICE_INFO:
			ret = cmd_get_device_info(lwis_dev, &header,
						  (struct lwis_cmd_device_info __user *)user_msg);
			break;
		case LWIS_CMD_ID_DEVICE_ENABLE:
			ret = cmd_device_enable(lwis_client, &header,
						(struct lwis_cmd_pkt __user *)user_msg);
			break;
		case LWIS_CMD_ID_DEVICE_DISABLE:
			ret = cmd_device_disable(lwis_client, &header,
						 (struct lwis_cmd_pkt __user *)user_msg);
			break;
		case LWIS_CMD_ID_DEVICE_RESET:
			ret = cmd_device_reset(lwis_client, &header,
					       (struct lwis_cmd_io_entries __user *)user_msg);
			break;
		case LWIS_CMD_ID_DEVICE_SUSPEND:
			ret = cmd_device_suspend(lwis_client, &header,
						 (struct lwis_cmd_pkt __user *)user_msg);
			break;
		case LWIS_CMD_ID_DEVICE_RESUME:
			ret = cmd_device_resume(lwis_client, &header,
						(struct lwis_cmd_pkt __user *)user_msg);
			break;
		case LWIS_CMD_ID_DMA_BUFFER_ENROLL:
			ret = cmd_dma_buffer_enroll(
				lwis_client, &header,
				(struct lwis_cmd_dma_buffer_enroll __user *)user_msg);
			break;
		case LWIS_CMD_ID_DMA_BUFFER_DISENROLL:
			ret = cmd_dma_buffer_disenroll(
				lwis_client, &header,
				(struct lwis_cmd_dma_buffer_disenroll __user *)user_msg);
			break;
		case LWIS_CMD_ID_DMA_BUFFER_CPU_ACCESS:
			ret = cmd_dma_buffer_cpu_access(
				lwis_client, &header,
				(struct lwis_cmd_dma_buffer_cpu_access __user *)user_msg);
			break;
		case LWIS_CMD_ID_DMA_BUFFER_ALLOC:
			ret = cmd_dma_buffer_alloc(
				lwis_client, &header,
				(struct lwis_cmd_dma_buffer_alloc __user *)user_msg);
			break;
		case LWIS_CMD_ID_DMA_BUFFER_FREE:
			ret = cmd_dma_buffer_free(
				lwis_client, &header,
				(struct lwis_cmd_dma_buffer_free __user *)user_msg);
			break;
		case LWIS_CMD_ID_REG_IO:
			ret = cmd_reg_io(lwis_dev, &header,
					 (struct lwis_cmd_io_entries __user *)user_msg);
			break;
		case LWIS_CMD_ID_EVENT_CONTROL_GET:
			ret = cmd_event_control_get(
				lwis_client, &header,
				(struct lwis_cmd_event_control_get __user *)user_msg);
			break;
		case LWIS_CMD_ID_EVENT_CONTROL_SET:
			ret = cmd_event_control_set(
				lwis_client, &header,
				(struct lwis_cmd_event_control_set __user *)user_msg);
			break;
		case LWIS_CMD_ID_EVENT_DEQUEUE:
			ret = cmd_event_dequeue(lwis_client, &header,
						(struct lwis_cmd_event_dequeue __user *)user_msg);
			break;
		case LWIS_CMD_ID_TRANSACTION_SUBMIT:
			ret = cmd_transaction_submit(
				lwis_client, &header,
				(struct lwis_cmd_transaction_info __user *)user_msg);
			break;
		case LWIS_CMD_ID_TRANSACTION_CANCEL:
			ret = cmd_transaction_cancel(
				lwis_client, &header,
				(struct lwis_cmd_transaction_cancel __user *)user_msg);
			break;
		case LWIS_CMD_ID_TRANSACTION_REPLACE:
			ret = cmd_transaction_replace(
				lwis_client, &header,
				(struct lwis_cmd_transaction_info __user *)user_msg);
			break;
		case LWIS_CMD_ID_PERIODIC_IO_SUBMIT:
			ret = cmd_periodic_io_submit(
				lwis_client, &header,
				(struct lwis_cmd_periodic_io_info __user *)user_msg);
			break;
		case LWIS_CMD_ID_PERIODIC_IO_CANCEL:
			ret = cmd_periodic_io_cancel(
				lwis_client, &header,
				(struct lwis_cmd_periodic_io_cancel __user *)user_msg);
			break;
		case LWIS_CMD_ID_DPM_CLK_UPDATE:
			ret = cmd_dpm_clk_update(lwis_dev, &header,
						 (struct lwis_cmd_dpm_clk_update __user *)user_msg);
			break;
		case LWIS_CMD_ID_DPM_QOS_UPDATE:
			ret = cmd_dpm_qos_update(lwis_dev, &header,
						 (struct lwis_cmd_dpm_qos_update __user *)user_msg);
			break;
		case LWIS_CMD_ID_DPM_GET_CLOCK:
			ret = cmd_dpm_get_clock(lwis_dev, &header,
						(struct lwis_cmd_dpm_clk_get __user *)user_msg);
			break;
		default:
			dev_err_ratelimited(lwis_dev->dev, "Unknown command id\n");
			header.ret_code = -EINVAL;
			ret = copy_pkt_to_user(lwis_dev, user_msg, (void *)&header, sizeof(header));
		}
		if (ret) {
			return ret;
		}
		user_msg = header.next;
	}

	return ret;
}
