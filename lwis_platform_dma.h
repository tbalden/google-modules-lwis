/*
 * Google LWIS Declarations of Platform-specific DMA Functions
 *
 * Copyright (c) 2018 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef LWIS_PLATFORM_DMA_H_
#define LWIS_PLATFORM_DMA_H_

#include "lwis_device.h"

/*
 * Does the actual platform specific parts of mapping a DMA buffer into the
 * device memory space, returning an IOMMU DMA virtual-address or an ERR_PTR
 * on error
 */
dma_addr_t lwis_platform_dma_buffer_map(struct lwis_device *lwis_dev,
					struct dma_buf_attachment *attachment,
					off_t offset, size_t size,
					enum dma_data_direction direction,
					int flags);

/*
 * Does the actual platform specific parts of unmapping a DMA buffer from the
 * device memory space
 */
int lwis_platform_dma_buffer_unmap(struct lwis_device *lwis_dev,
				   struct dma_buf_attachment *attachment,
				   dma_addr_t address);


#endif /* LWIS_PLATFORM_DMA_H_ */
