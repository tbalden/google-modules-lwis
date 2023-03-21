/*
 * Google LWIS IOCTL Handler
 *
 * Copyright (c) 2018 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME "-ioctl: " fmt

#include "lwis_ioctl.h"

#include <linux/kernel.h>
#include <linux/mm.h>
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
#include "lwis_ioctl_cmd.h"
#include "lwis_ioreg.h"
#include "lwis_periodic_io.h"
#include "lwis_platform.h"
#include "lwis_regulator.h"
#include "lwis_transaction.h"
#include "lwis_util.h"

#define IOCTL_TO_ENUM(x) _IOC_NR(x)
#define IOCTL_ARG_SIZE(x) _IOC_SIZE(x)
#define STRINGIFY(x) #x

static void lwis_ioctl_pr_err(struct lwis_device *lwis_dev, unsigned int ioctl_type, int errno)
{
	unsigned int type = IOCTL_TO_ENUM(ioctl_type);
	static char type_name[32];
	size_t exp_size;

	switch (type) {
	case IOCTL_TO_ENUM(LWIS_CMD_PACKET):
		strscpy(type_name, STRINGIFY(LWIS_CMD_PACKET), sizeof(type_name));
		exp_size = IOCTL_ARG_SIZE(LWIS_CMD_PACKET);
		break;
	default:
		strscpy(type_name, "UNDEFINED", sizeof(type_name));
		exp_size = 0;
		break;
	};

	if (strcmp(type_name, "UNDEFINED") && exp_size != IOCTL_ARG_SIZE(ioctl_type)) {
		dev_err_ratelimited(
			lwis_dev->dev,
			"Failed to process %s (errno: %d), expecting argument with length of %zu, got length of %d. Mismatch kernel version?\n",
			type_name, errno, exp_size, IOCTL_ARG_SIZE(ioctl_type));
	} else {
		dev_err_ratelimited(lwis_dev->dev, "Failed to process %s (errno: %d)\n", type_name,
				    errno);
	}
}

static int register_read(struct lwis_device *lwis_dev, struct lwis_io_entry *read_entry,
			 struct lwis_io_entry *user_msg)
{
	int ret = 0;
	uint8_t *user_buf;
	bool batch_mode = false;

	if (read_entry->type == LWIS_IO_ENTRY_READ_BATCH) {
		batch_mode = true;
		/* Save the userspace buffer address */
		user_buf = read_entry->rw_batch.buf;
		/* Allocate read buffer */
		read_entry->rw_batch.buf =
			lwis_allocator_allocate(lwis_dev, read_entry->rw_batch.size_in_bytes);
		if (!read_entry->rw_batch.buf) {
			dev_err_ratelimited(lwis_dev->dev,
					    "Failed to allocate register read buffer\n");
			return -ENOMEM;
		}
	} else if (read_entry->type != LWIS_IO_ENTRY_READ) {
		/* Type must be either READ or READ_BATCH */
		dev_err(lwis_dev->dev, "Invalid io_entry type for REGISTER_READ\n");
		return -EINVAL;
	}

	ret = lwis_dev->vops.register_io(lwis_dev, read_entry, lwis_dev->native_value_bitwidth);
	if (ret) {
		dev_err_ratelimited(lwis_dev->dev, "Failed to read registers\n");
		goto reg_read_exit;
	}

	/* Copy read data back to userspace */
	if (batch_mode) {
		if (copy_to_user((void __user *)user_buf, read_entry->rw_batch.buf,
				 read_entry->rw_batch.size_in_bytes)) {
			ret = -EFAULT;
			dev_err_ratelimited(
				lwis_dev->dev,
				"Failed to copy register read buffer back to userspace\n");
		}
	} else {
		if (copy_to_user((void __user *)user_msg, read_entry, sizeof(*read_entry))) {
			ret = -EFAULT;
			dev_err_ratelimited(
				lwis_dev->dev,
				"Failed to copy register read entry back to userspace\n");
		}
	}

reg_read_exit:
	if (batch_mode) {
		lwis_allocator_free(lwis_dev, read_entry->rw_batch.buf);
		read_entry->rw_batch.buf = NULL;
	}
	return ret;
}

static int register_write(struct lwis_device *lwis_dev, struct lwis_io_entry *write_entry)
{
	int ret = 0;
	uint8_t *user_buf;
	bool batch_mode = false;

	if (write_entry->type == LWIS_IO_ENTRY_WRITE_BATCH) {
		batch_mode = true;
		/* Save the userspace buffer address */
		user_buf = write_entry->rw_batch.buf;
		/* Allocate write buffer and copy contents from userspace */
		write_entry->rw_batch.buf =
			lwis_allocator_allocate(lwis_dev, write_entry->rw_batch.size_in_bytes);
		if (!write_entry->rw_batch.buf) {
			dev_err_ratelimited(lwis_dev->dev,
					    "Failed to allocate register write buffer\n");
			return -ENOMEM;
		}

		if (copy_from_user(write_entry->rw_batch.buf, (void __user *)user_buf,
				   write_entry->rw_batch.size_in_bytes)) {
			ret = -EFAULT;
			dev_err_ratelimited(lwis_dev->dev,
					    "Failed to copy write buffer from userspace\n");
			goto reg_write_exit;
		}
	} else if (write_entry->type != LWIS_IO_ENTRY_WRITE) {
		/* Type must be either WRITE or WRITE_BATCH */
		dev_err(lwis_dev->dev, "Invalid io_entry type for REGISTER_WRITE\n");
		return -EINVAL;
	}

	ret = lwis_dev->vops.register_io(lwis_dev, write_entry, lwis_dev->native_value_bitwidth);
	if (ret) {
		dev_err_ratelimited(lwis_dev->dev, "Failed to write registers\n");
	}

reg_write_exit:
	if (batch_mode) {
		lwis_allocator_free(lwis_dev, write_entry->rw_batch.buf);
		write_entry->rw_batch.buf = NULL;
	}
	return ret;
}

static int register_modify(struct lwis_device *lwis_dev, struct lwis_io_entry *modify_entry)
{
	int ret = 0;

	ret = lwis_dev->vops.register_io(lwis_dev, modify_entry, lwis_dev->native_value_bitwidth);
	if (ret) {
		dev_err_ratelimited(lwis_dev->dev, "Failed to read registers for modify\n");
	}

	return ret;
}

int lwis_ioctl_util_synchronous_process_io_entries(struct lwis_device *lwis_dev, int num_io_entries,
						   struct lwis_io_entry *io_entries,
						   struct lwis_io_entry *user_msg)
{
	int ret = 0, i = 0;

	/* Use write memory barrier at the beginning of I/O entries if the access protocol
	 * allows it */
	if (lwis_dev->vops.register_io_barrier != NULL) {
		lwis_dev->vops.register_io_barrier(lwis_dev,
						   /*use_read_barrier=*/false,
						   /*use_write_barrier=*/true);
	}
	for (i = 0; i < num_io_entries; i++) {
		switch (io_entries[i].type) {
		case LWIS_IO_ENTRY_MODIFY:
			ret = register_modify(lwis_dev, &io_entries[i]);
			break;
		case LWIS_IO_ENTRY_READ:
		case LWIS_IO_ENTRY_READ_BATCH:
			ret = register_read(lwis_dev, &io_entries[i], user_msg + i);
			break;
		case LWIS_IO_ENTRY_WRITE:
		case LWIS_IO_ENTRY_WRITE_BATCH:
			ret = register_write(lwis_dev, &io_entries[i]);
			break;
		case LWIS_IO_ENTRY_POLL:
			ret = lwis_io_entry_poll(lwis_dev, &io_entries[i]);
			break;
		case LWIS_IO_ENTRY_READ_ASSERT:
			ret = lwis_io_entry_read_assert(lwis_dev, &io_entries[i]);
			break;
		default:
			dev_err(lwis_dev->dev, "Unknown io_entry operation\n");
			ret = -EINVAL;
		}
		if (ret) {
			dev_err(lwis_dev->dev, "Register io_entry failed\n");
			goto exit;
		}
	}
exit:
	/* Use read memory barrier at the end of I/O entries if the access protocol
	 * allows it */
	if (lwis_dev->vops.register_io_barrier != NULL) {
		lwis_dev->vops.register_io_barrier(lwis_dev,
						   /*use_read_barrier=*/true,
						   /*use_write_barrier=*/false);
	}
	return ret;
}

int lwis_ioctl_util_construct_io_entry(struct lwis_client *client,
				       struct lwis_io_entry *user_entries, size_t num_io_entries,
				       struct lwis_io_entry **io_entries)
{
	int i;
	int ret = 0;
	int last_buf_alloc_idx = -1;
	size_t entry_size;
	struct lwis_io_entry *k_entries;
	uint8_t *user_buf;
	uint8_t *k_buf;
	struct lwis_device *lwis_dev = client->lwis_dev;

	entry_size = num_io_entries * sizeof(struct lwis_io_entry);
	if (entry_size / sizeof(struct lwis_io_entry) != num_io_entries) {
		dev_err(lwis_dev->dev, "Failed to prepare io entries due to integer overflow\n");
		return -EOVERFLOW;
	}
	k_entries = lwis_allocator_allocate(lwis_dev, entry_size);
	if (!k_entries) {
		dev_err(lwis_dev->dev, "Failed to allocate io entries\n");
		return -ENOMEM;
	}

	if (copy_from_user((void *)k_entries, (void __user *)user_entries, entry_size)) {
		ret = -EFAULT;
		dev_err(lwis_dev->dev, "Failed to copy io entries from user\n");
		goto error_free_entries;
	}

	/*
	 * For batch writes, need to allocate kernel buffers to deep copy the
	 * write values. Don't need to do this for batch reads because memory
	 * will be allocated in the form of lwis_io_result in io processing.
	 */
	for (i = 0; i < num_io_entries; ++i) {
		if (k_entries[i].type == LWIS_IO_ENTRY_WRITE_BATCH) {
			user_buf = k_entries[i].rw_batch.buf;
			k_buf = lwis_allocator_allocate(lwis_dev,
							k_entries[i].rw_batch.size_in_bytes);
			if (!k_buf) {
				dev_err_ratelimited(lwis_dev->dev,
						    "Failed to allocate io write buffer\n");
				ret = -ENOMEM;
				goto error_free_buf;
			}
			last_buf_alloc_idx = i;
			k_entries[i].rw_batch.buf = k_buf;
			if (copy_from_user(k_buf, (void __user *)user_buf,
					   k_entries[i].rw_batch.size_in_bytes)) {
				ret = -EFAULT;
				dev_err_ratelimited(
					lwis_dev->dev,
					"Failed to copy io write buffer from userspace\n");
				goto error_free_buf;
			}
		}
	}

	*io_entries = k_entries;
	return 0;

error_free_buf:
	for (i = 0; i <= last_buf_alloc_idx; ++i) {
		if (k_entries[i].type == LWIS_IO_ENTRY_WRITE_BATCH) {
			lwis_allocator_free(lwis_dev, k_entries[i].rw_batch.buf);
			k_entries[i].rw_batch.buf = NULL;
		}
	}
error_free_entries:
	lwis_allocator_free(lwis_dev, k_entries);
	*io_entries = NULL;
	return ret;
}

int lwis_ioctl_handler(struct lwis_client *lwis_client, unsigned int type, unsigned long param)
{
	int ret = 0;
	struct lwis_device *lwis_dev = lwis_client->lwis_dev;

	switch (type) {
	case LWIS_CMD_PACKET:
		ret = lwis_ioctl_handle_cmd_pkt(lwis_client, (struct lwis_cmd_pkt *)param);
		break;
	default:
		dev_err_ratelimited(lwis_dev->dev, "Unknown IOCTL operation\n");
		ret = -EINVAL;
	};

	if (ret && ret != -ENOENT && ret != -ETIMEDOUT && ret != -EAGAIN) {
		lwis_ioctl_pr_err(lwis_dev, type, ret);
	}

	return ret;
}
