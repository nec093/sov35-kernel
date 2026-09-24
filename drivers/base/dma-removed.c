// SPDX-License-Identifier: GPL-2.0-only
/*
 * "removed-dma-pool" reserved memory: a no-map carveout handed out to one
 * or more devices through dma_alloc_attrs(). The MSM peripheral loaders
 * (modem, adsp, slpi, venus, the GPU zap shader) must place firmware
 * images in these regions, which TrustZone authenticates them in.
 *
 * Copyright (c) 2013-2017, The Linux Foundation. All rights reserved.
 * Copyright (C) 2000-2004 Russell King
 *
 * Ported to 5.4: the "no-map-fixup" carve-out resizing depended on bootmem
 * interfaces that no longer exist and is not supported (no user on MSM8996).
 */
#include <linux/bitmap.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/sizes.h>
#include <linux/slab.h>

struct removed_region {
	phys_addr_t	base;
	int		nr_pages;
	unsigned long	*bitmap;
	struct mutex	lock;
};

#define NO_KERNEL_MAPPING_DUMMY	0x2222

static int dma_init_removed_memory(phys_addr_t phys_addr, size_t size,
				   struct removed_region **mem)
{
	struct removed_region *dma_mem;
	int pages = size >> PAGE_SHIFT;
	int bitmap_size = BITS_TO_LONGS(pages) * sizeof(long);

	dma_mem = kzalloc(sizeof(*dma_mem), GFP_KERNEL);
	if (!dma_mem)
		return -ENOMEM;
	dma_mem->bitmap = kzalloc(bitmap_size, GFP_KERNEL);
	if (!dma_mem->bitmap) {
		kfree(dma_mem);
		return -ENOMEM;
	}

	dma_mem->base = phys_addr;
	dma_mem->nr_pages = pages;
	mutex_init(&dma_mem->lock);

	*mem = dma_mem;
	return 0;
}

static void *removed_alloc(struct device *dev, size_t size,
			   dma_addr_t *handle, gfp_t gfp, unsigned long attrs)
{
	bool no_kernel_mapping = attrs & DMA_ATTR_NO_KERNEL_MAPPING;
	bool skip_zeroing = attrs & DMA_ATTR_SKIP_ZEROING;
	struct removed_region *dma_mem = dev->removed_mem;
	unsigned long order;
	unsigned int align;
	void *addr = NULL;
	int pageno, nbits;

	if (!gfpflags_allow_blocking(gfp))
		return NULL;

	size = PAGE_ALIGN(size);
	nbits = size >> PAGE_SHIFT;
	order = get_order(size);
	if (order > get_order(SZ_1M))
		order = get_order(SZ_1M);
	align = (1 << order) - 1;

	mutex_lock(&dma_mem->lock);
	pageno = bitmap_find_next_zero_area(dma_mem->bitmap, dma_mem->nr_pages,
					    0, nbits, align);
	if (pageno < dma_mem->nr_pages) {
		phys_addr_t base = dma_mem->base + (phys_addr_t)pageno *
				   PAGE_SIZE;

		*handle = base;
		bitmap_set(dma_mem->bitmap, pageno, nbits);

		if (no_kernel_mapping && skip_zeroing) {
			addr = (void *)NO_KERNEL_MAPPING_DUMMY;
			goto out;
		}

		addr = ioremap(base, size);
		if (WARN_ON(!addr)) {
			bitmap_clear(dma_mem->bitmap, pageno, nbits);
		} else {
			if (!skip_zeroing)
				memset_io(addr, 0, size);
			if (no_kernel_mapping) {
				iounmap(addr);
				addr = (void *)NO_KERNEL_MAPPING_DUMMY;
			}
		}
	}
out:
	mutex_unlock(&dma_mem->lock);
	return addr;
}

static int removed_mmap(struct device *dev, struct vm_area_struct *vma,
			void *cpu_addr, dma_addr_t dma_addr, size_t size,
			unsigned long attrs)
{
	return -ENXIO;
}

static void removed_free(struct device *dev, size_t size, void *cpu_addr,
			 dma_addr_t handle, unsigned long attrs)
{
	bool no_kernel_mapping = attrs & DMA_ATTR_NO_KERNEL_MAPPING;
	struct removed_region *dma_mem = dev->removed_mem;

	size = PAGE_ALIGN(size);
	if (!no_kernel_mapping)
		iounmap(cpu_addr);
	mutex_lock(&dma_mem->lock);
	bitmap_clear(dma_mem->bitmap, (handle - dma_mem->base) >> PAGE_SHIFT,
		     size >> PAGE_SHIFT);
	mutex_unlock(&dma_mem->lock);
}

static dma_addr_t removed_map_page(struct device *dev, struct page *page,
				   unsigned long offset, size_t size,
				   enum dma_data_direction dir,
				   unsigned long attrs)
{
	return DMA_MAPPING_ERROR;
}

static void removed_unmap_page(struct device *dev, dma_addr_t dma_handle,
			       size_t size, enum dma_data_direction dir,
			       unsigned long attrs)
{
}

static int removed_map_sg(struct device *dev, struct scatterlist *sg,
			  int nents, enum dma_data_direction dir,
			  unsigned long attrs)
{
	return 0;
}

static void removed_unmap_sg(struct device *dev, struct scatterlist *sg,
			     int nents, enum dma_data_direction dir,
			     unsigned long attrs)
{
}

static void removed_sync_single(struct device *dev, dma_addr_t dma_handle,
				size_t size, enum dma_data_direction dir)
{
}

static void removed_sync_sg(struct device *dev, struct scatterlist *sg,
			    int nents, enum dma_data_direction dir)
{
}

const struct dma_map_ops removed_dma_ops = {
	.alloc			= removed_alloc,
	.free			= removed_free,
	.mmap			= removed_mmap,
	.map_page		= removed_map_page,
	.unmap_page		= removed_unmap_page,
	.map_sg			= removed_map_sg,
	.unmap_sg		= removed_unmap_sg,
	.sync_single_for_cpu	= removed_sync_single,
	.sync_single_for_device	= removed_sync_single,
	.sync_sg_for_cpu	= removed_sync_sg,
	.sync_sg_for_device	= removed_sync_sg,
};
EXPORT_SYMBOL(removed_dma_ops);

static int rmem_removed_device_init(struct reserved_mem *rmem,
				    struct device *dev)
{
	struct removed_region *mem = rmem->priv;

	if (dev->removed_mem)
		return -EBUSY;

	if (!mem && dma_init_removed_memory(rmem->base, rmem->size, &mem)) {
		pr_info("Reserved memory: failed to init DMA memory pool at %pa, size %ld MiB\n",
			&rmem->base, (unsigned long)rmem->size / SZ_1M);
		return -EINVAL;
	}
	rmem->priv = mem;
	dev->removed_mem = mem;
	set_dma_ops(dev, &removed_dma_ops);
	return 0;
}

static void rmem_removed_device_release(struct reserved_mem *rmem,
					struct device *dev)
{
	dev->removed_mem = NULL;
	set_dma_ops(dev, NULL);
}

static const struct reserved_mem_ops removed_mem_ops = {
	.device_init	= rmem_removed_device_init,
	.device_release	= rmem_removed_device_release,
};

static int __init removed_dma_setup(struct reserved_mem *rmem)
{
	unsigned long node = rmem->fdt_node;

	if (of_get_flat_dt_prop(node, "no-map-fixup", NULL)) {
		pr_err("Removed memory: no-map-fixup is not supported\n");
		return -EINVAL;
	}

	rmem->ops = &removed_mem_ops;
	pr_info("Removed memory: created DMA memory pool at %pa, size %ld MiB\n",
		&rmem->base, (unsigned long)rmem->size / SZ_1M);
	return 0;
}
RESERVEDMEM_OF_DECLARE(removed_dma, "removed-dma-pool", removed_dma_setup);
