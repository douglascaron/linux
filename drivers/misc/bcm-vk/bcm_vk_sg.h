/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright(c) 2018 Broadcom
 */

#ifndef __BCM_VK_SG_H__
#define __BCM_VK_SG_H__

#include <linux/dma-mapping.h>

struct bcm_vk_dma {
	/* for userland buffer */
	struct page **pages;
	int nr_pages;

	/* common */
	dma_addr_t handle;
	/*
	 * sglist is of the following LE format
	 * [U32] num_sg  = number of sg addresses (N)
	 * [U32] size[0] = size of address0
	 * [U32] addr_l[0] = lower 32-bits of address0
	 * [U32] addr_h[0] = higher 32-bits of address0
	 * ..
	 * [U32] size[N-1] = size of addressN-1
	 * [U32] addr_l[N-1] = lower 32-bits of addressN-1
	 * [U32] addr_h[N-1] = higher 32-bits of addressN-1
	 */
	uint32_t *sglist;
	int sglen; /* Length (bytes) of sglist */
	int direction;
};

struct _vk_data {
	uint32_t size;    /* data size in bytes */
	uint64_t address; /* Pointer to data     */
} __packed;

/*
 * Scatter-gather DMA buffer API.
 *
 * These functions provide a simple way to create a page list and a
 * scatter-gather list from userspace address and map the memory
 * for DMA operation.
 */
int bcm_vk_dma_alloc(struct device *dev,
		     struct bcm_vk_dma *dma,
		     int dir,
		     struct _vk_data *vkdata);

int bcm_vk_msg_free_sg(struct device *dev, struct bcm_vk_dma *dma, int num);

#endif

