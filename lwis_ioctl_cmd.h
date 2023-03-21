/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Google LWIS Command packets
 *
 * Copyright (c) 2022 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef LWIS_CMD_H_
#define LWIS_CMD_H_

#include "lwis_device.h"

/*
 *  lwis_ioctl_handle_cmd_pkt: Handle command packets from IOCTL
 */
int lwis_ioctl_handle_cmd_pkt(struct lwis_client *lwis_client,
			      struct lwis_cmd_pkt __user *user_msg);

#endif /* LWIS_CMD_H_ */