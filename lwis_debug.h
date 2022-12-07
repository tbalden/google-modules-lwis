/*
 * Google LWIS Debug Utilities
 *
 * Copyright (c) 2020 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "lwis_device.h"

/* Functions to save information for debugging */
void lwis_debug_save_register_io_to_history(struct lwis_device *lwis_dev,
					    struct lwis_io_entry *entry, size_t access_size);

/* Functions to print debugging info */
int lwis_debug_print_device_info(struct lwis_device *lwis_dev);
int lwis_debug_print_event_states_info(struct lwis_device *lwis_dev);
int lwis_debug_print_transaction_info(struct lwis_device *lwis_dev);
int lwis_debug_print_register_io_history(struct lwis_device *lwis_dev);
int lwis_debug_print_buffer_info(struct lwis_device *lwis_dev);

/* DebugFS specific functions */
int lwis_device_debugfs_setup(struct lwis_device *lwis_dev, struct dentry *dbg_root);
int lwis_device_debugfs_cleanup(struct lwis_device *lwis_dev);