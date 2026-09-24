/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_DEVICE_H
#define __ASM_DEVICE_H

struct dev_archdata {
#ifdef CONFIG_IOMMU_API
	void *iommu;			/* private IOMMU data */
#endif
#ifdef CONFIG_ARM64_DMA_USE_IOMMU
	struct dma_iommu_mapping	*mapping;
	const struct dma_map_ops	*saved_dma_ops;
#endif
};

struct pdev_archdata {
};

#endif
