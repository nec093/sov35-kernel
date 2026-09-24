// SPDX-License-Identifier: GPL-2.0-only
/*
 * CAF arm_iommu_* DMA mapping API for arm64, ported from the msm-4.14
 * arch/arm64/mm/dma-mapping.c (bitmap IOVA allocator only).
 *
 * The MSM multimedia drivers (mdss, rotator, venus, camera, fastrpc, ...)
 * create their own IOMMU domain with arm_iommu_create_mapping(), attach a
 * context-bank device to it and then use the regular DMA API on that
 * device. Upstream 5.4 arm64 has no such layer: its devices get the
 * iommu-dma default domain instead. Attaching here moves the device's
 * group from its default domain to the driver's domain and installs
 * dma_map_ops that allocate IOVAs from that domain.
 *
 * Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 */

#include <linux/bitmap.h>
#include <linux/dma-contiguous.h>
#include <linux/dma-mapping.h>
#include <linux/dma-noncoherent.h>
#include <linux/export.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <asm/dma-iommu.h>

/* largest IOVA alignment, as a page order (CAF default) */
#define CAF_DMA_IOMMU_ALIGNMENT	9

static bool is_dma_coherent(struct device *dev, unsigned long attrs)
{
	if (attrs & DMA_ATTR_FORCE_COHERENT)
		return true;
	if (attrs & DMA_ATTR_FORCE_NON_COHERENT)
		return false;
	return dev_is_dma_coherent(dev);
}

static pgprot_t __get_dma_pgprot(unsigned long attrs, pgprot_t prot,
				 bool coherent)
{
	if (attrs & DMA_ATTR_STRONGLY_ORDERED)
		return pgprot_noncached(prot);
	if (!coherent || (attrs & DMA_ATTR_WRITE_COMBINE))
		return pgprot_writecombine(prot);
	return prot;
}

static int __get_iommu_pgprot(unsigned long attrs, int prot, bool coherent)
{
	if (!(attrs & DMA_ATTR_EXEC_MAPPING))
		prot |= IOMMU_NOEXEC;
	if (coherent)
		prot |= IOMMU_CACHE;
	return prot;
}

static void __dma_page_cpu_to_dev(struct page *page, unsigned long off,
				  size_t size, enum dma_data_direction dir)
{
	arch_sync_dma_for_device(page_to_phys(page) + off, size, dir);
}

static void __dma_page_dev_to_cpu(struct page *page, unsigned long off,
				  size_t size, enum dma_data_direction dir)
{
	arch_sync_dma_for_cpu(page_to_phys(page) + off, size, dir);
}

static void __dma_clear_buffer(struct page *page, size_t size,
			       unsigned long attrs, bool is_coherent)
{
	/*
	 * Ensure that the allocated pages are zeroed, and that any data
	 * lurking in the kernel direct-mapped region is invalidated.
	 */
	if (!(attrs & DMA_ATTR_SKIP_ZEROING))
		memset(page_address(page), 0, size);
	if (!is_coherent)
		arch_dma_prep_coherent(page, size);
}

static dma_addr_t __alloc_iova(struct dma_iommu_mapping *mapping,
			       size_t size)
{
	unsigned int order;
	unsigned int align;
	unsigned long count, start;
	unsigned long flags;

	size = PAGE_ALIGN(size);
	order = get_order(size);
	if (order > CAF_DMA_IOMMU_ALIGNMENT)
		order = CAF_DMA_IOMMU_ALIGNMENT;

	count = size >> PAGE_SHIFT;
	align = (1 << order) - 1;

	spin_lock_irqsave(&mapping->lock, flags);
	start = bitmap_find_next_zero_area(mapping->bitmap, mapping->bits, 0,
					   count, align);
	if (start > mapping->bits) {
		spin_unlock_irqrestore(&mapping->lock, flags);
		return DMA_MAPPING_ERROR;
	}
	bitmap_set(mapping->bitmap, start, count);
	spin_unlock_irqrestore(&mapping->lock, flags);

	return mapping->base + ((dma_addr_t)start << PAGE_SHIFT);
}

static void __free_iova(struct dma_iommu_mapping *mapping,
			dma_addr_t addr, size_t size)
{
	unsigned long start, count, flags;

	addr &= PAGE_MASK;
	size = PAGE_ALIGN(size);
	start = (addr - mapping->base) >> PAGE_SHIFT;
	count = size >> PAGE_SHIFT;

	spin_lock_irqsave(&mapping->lock, flags);
	bitmap_clear(mapping->bitmap, start, count);
	spin_unlock_irqrestore(&mapping->lock, flags);
}

static struct page **__iommu_alloc_buffer(struct device *dev, size_t size,
					  gfp_t gfp, unsigned long attrs)
{
	struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);
	unsigned long alloc_sizes = mapping->domain->pgsize_bitmap;
	size_t count = size >> PAGE_SHIFT;
	bool is_coherent = is_dma_coherent(dev, attrs);
	unsigned long order_mask;
	struct page **pages;
	int i = 0;

	pages = kvzalloc(count * sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return NULL;

	if (attrs & DMA_ATTR_FORCE_CONTIGUOUS) {
		unsigned long order = get_order(size);
		struct page *page;

		page = dma_alloc_from_contiguous(dev, count, order,
						 gfp & __GFP_NOWARN);
		if (!page)
			goto error;

		__dma_clear_buffer(page, size, attrs, is_coherent);

		for (i = 0; i < count; i++)
			pages[i] = page + i;

		return pages;
	}

	/* IOMMU can map any pages, so himem can also be used here */
	gfp |= __GFP_NOWARN | __GFP_HIGHMEM;
	order_mask = alloc_sizes >> PAGE_SHIFT;
	order_mask &= (2U << MAX_ORDER) - 1;
	if (!order_mask)
		goto error;

	while (count) {
		int j, order;

		order_mask &= (2U << __fls(count)) - 1;
		order = __fls(order_mask);

		pages[i] = alloc_pages(order ? (gfp | __GFP_NORETRY) &
					~__GFP_RECLAIM : gfp, order);
		while (!pages[i] && order) {
			order_mask &= ~(1U << order);
			order = __fls(order_mask);
			pages[i] = alloc_pages(order ? (gfp | __GFP_NORETRY) &
					~__GFP_RECLAIM : gfp, order);
		}

		if (!pages[i])
			goto error;

		if (order) {
			split_page(pages[i], order);
			j = 1 << order;
			while (--j)
				pages[i + j] = pages[i] + j;
		}

		__dma_clear_buffer(pages[i], PAGE_SIZE << order, attrs,
				   is_coherent);
		i += 1 << order;
		count -= 1 << order;
	}

	return pages;
error:
	while (i--)
		if (pages[i])
			__free_pages(pages[i], 0);
	kvfree(pages);
	return NULL;
}

static void __iommu_free_buffer(struct device *dev, struct page **pages,
				size_t size, unsigned long attrs)
{
	int count = size >> PAGE_SHIFT;
	int i;

	if (attrs & DMA_ATTR_FORCE_CONTIGUOUS) {
		dma_release_from_contiguous(dev, pages[0], count);
	} else {
		for (i = 0; i < count; i++)
			if (pages[i])
				__free_pages(pages[i], 0);
	}
	kvfree(pages);
}

/* Create a mapping in device IO address space for specified pages */
static dma_addr_t __iommu_create_mapping(struct device *dev,
					 struct page **pages, size_t size,
					 unsigned long attrs)
{
	struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);
	unsigned int count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	dma_addr_t dma_addr, iova;
	int i, ret;
	int prot = IOMMU_READ | IOMMU_WRITE;

	dma_addr = __alloc_iova(mapping, size);
	if (dma_addr == DMA_MAPPING_ERROR)
		return dma_addr;

	prot = __get_iommu_pgprot(attrs, prot, is_dma_coherent(dev, attrs));

	iova = dma_addr;
	for (i = 0; i < count; ) {
		unsigned int next_pfn = page_to_pfn(pages[i]) + 1;
		phys_addr_t phys = page_to_phys(pages[i]);
		unsigned int len, j;

		for (j = i + 1; j < count; j++, next_pfn++)
			if (page_to_pfn(pages[j]) != next_pfn)
				break;

		len = (j - i) << PAGE_SHIFT;
		ret = iommu_map(mapping->domain, iova, phys, len, prot);
		if (ret < 0)
			goto fail;
		iova += len;
		i = j;
	}
	return dma_addr;
fail:
	iommu_unmap(mapping->domain, dma_addr, iova - dma_addr);
	__free_iova(mapping, dma_addr, size);
	return DMA_MAPPING_ERROR;
}

static void __iommu_remove_mapping(struct device *dev, dma_addr_t iova,
				   size_t size)
{
	struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);

	/* add optional in-page offset from iova to size, align to page size */
	size = PAGE_ALIGN((iova & ~PAGE_MASK) + size);
	iova &= PAGE_MASK;

	iommu_unmap(mapping->domain, iova, size);
	__free_iova(mapping, iova, size);
}

/*
 * Atomic allocations come from the (physically contiguous) atomic pool;
 * their first page can be recovered from the IOVA.
 */
static void *__iommu_alloc_atomic(struct device *dev, size_t size,
				  dma_addr_t *handle, gfp_t gfp,
				  unsigned long attrs)
{
	size_t count = size >> PAGE_SHIFT;
	bool coherent = is_dma_coherent(dev, attrs);
	struct page **pages;
	struct page *page;
	void *addr;
	int i;

	pages = kvzalloc(count * sizeof(struct page *), gfp);
	if (!pages)
		return NULL;

	if (coherent) {
		page = alloc_pages(gfp, get_order(size));
		addr = page ? page_address(page) : NULL;
	} else {
		addr = dma_alloc_from_pool(size, &page, gfp);
	}
	if (!addr)
		goto err_free;

	for (i = 0; i < count; i++)
		pages[i] = page + i;

	*handle = __iommu_create_mapping(dev, pages, size, attrs);
	if (*handle == DMA_MAPPING_ERROR)
		goto err_mapping;

	kvfree(pages);
	return addr;

err_mapping:
	if (coherent)
		__free_pages(page, get_order(size));
	else
		dma_free_from_pool(addr, size);
err_free:
	kvfree(pages);
	return NULL;
}

static void *arm_iommu_alloc_attrs(struct device *dev, size_t size,
				   dma_addr_t *handle, gfp_t gfp,
				   unsigned long attrs)
{
	bool coherent = is_dma_coherent(dev, attrs);
	pgprot_t prot = __get_dma_pgprot(attrs, PAGE_KERNEL, coherent);
	struct page **pages;
	void *addr;

	*handle = DMA_MAPPING_ERROR;
	size = PAGE_ALIGN(size);

	if (!gfpflags_allow_blocking(gfp))
		return __iommu_alloc_atomic(dev, size, handle, gfp, attrs);

	/* split_page() cannot handle __GFP_COMP pages */
	gfp &= ~(__GFP_COMP);

	pages = __iommu_alloc_buffer(dev, size, gfp, attrs);
	if (!pages)
		return NULL;

	*handle = __iommu_create_mapping(dev, pages, size, attrs);
	if (*handle == DMA_MAPPING_ERROR)
		goto err_buffer;

	if (attrs & DMA_ATTR_NO_KERNEL_MAPPING)
		return pages;

	addr = dma_common_pages_remap(pages, size, prot,
				      __builtin_return_address(0));
	if (!addr)
		goto err_mapping;

	return addr;

err_mapping:
	__iommu_remove_mapping(dev, *handle, size);
err_buffer:
	__iommu_free_buffer(dev, pages, size, attrs);
	*handle = DMA_MAPPING_ERROR;
	return NULL;
}

/* pages backing a non-atomic allocation */
static struct page **__iommu_get_pages(void *cpu_addr, unsigned long attrs)
{
	if (attrs & DMA_ATTR_NO_KERNEL_MAPPING)
		return cpu_addr;
	return dma_common_find_pages(cpu_addr);
}

static void arm_iommu_free_attrs(struct device *dev, size_t size,
				 void *cpu_addr, dma_addr_t handle,
				 unsigned long attrs)
{
	struct page **pages;

	size = PAGE_ALIGN(size);

	if (dma_in_atomic_pool(cpu_addr, size)) {
		__iommu_remove_mapping(dev, handle, size);
		dma_free_from_pool(cpu_addr, size);
		return;
	}

	pages = __iommu_get_pages(cpu_addr, attrs);
	if (!pages) {
		if (is_dma_coherent(dev, attrs) &&
		    virt_addr_valid(cpu_addr)) {
			/* coherent atomic allocation */
			__iommu_remove_mapping(dev, handle, size);
			__free_pages(virt_to_page(cpu_addr), get_order(size));
			return;
		}
		WARN(1, "trying to free invalid coherent area: %p\n",
		     cpu_addr);
		return;
	}

	if (!(attrs & DMA_ATTR_NO_KERNEL_MAPPING))
		dma_common_free_remap(cpu_addr, size);

	__iommu_remove_mapping(dev, handle, size);
	__iommu_free_buffer(dev, pages, size, attrs);
}

/* first page of a physically contiguous (atomic) allocation */
static struct page *__iommu_first_page(struct device *dev, dma_addr_t handle)
{
	struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);
	phys_addr_t phys = iommu_iova_to_phys(mapping->domain,
					      handle & PAGE_MASK);

	return phys ? phys_to_page(phys) : NULL;
}

static int arm_iommu_mmap_attrs(struct device *dev,
				struct vm_area_struct *vma, void *cpu_addr,
				dma_addr_t dma_addr, size_t size,
				unsigned long attrs)
{
	unsigned long uaddr = vma->vm_start;
	unsigned long usize = vma->vm_end - vma->vm_start;
	unsigned long nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	bool coherent = is_dma_coherent(dev, attrs);
	struct page **pages;
	unsigned long i = 0;

	vma->vm_page_prot = __get_dma_pgprot(attrs, vma->vm_page_prot,
					     coherent);

	if (dma_in_atomic_pool(cpu_addr, size) ||
	    !(pages = __iommu_get_pages(cpu_addr, attrs))) {
		struct page *page = __iommu_first_page(dev, dma_addr);

		if (!page)
			return -ENXIO;
		return remap_pfn_range(vma, uaddr, page_to_pfn(page), usize,
				       vma->vm_page_prot);
	}

	do {
		int ret;

		if (i >= nr_pages)
			return -ENXIO;
		ret = vm_insert_page(vma, uaddr, pages[i++]);
		if (ret) {
			pr_err("Remapping memory failed: %d\n", ret);
			return ret;
		}
		uaddr += PAGE_SIZE;
		usize -= PAGE_SIZE;
	} while (usize > 0);

	return 0;
}

static int arm_iommu_get_sgtable(struct device *dev, struct sg_table *sgt,
				 void *cpu_addr, dma_addr_t dma_addr,
				 size_t size, unsigned long attrs)
{
	unsigned int count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	struct page **pages = NULL;

	if (!dma_in_atomic_pool(cpu_addr, size))
		pages = __iommu_get_pages(cpu_addr, attrs);

	if (!pages) {
		struct page *page = __iommu_first_page(dev, dma_addr);
		int ret;

		if (!page)
			return -ENXIO;
		ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
		if (!ret)
			sg_set_page(sgt->sgl, page, PAGE_ALIGN(size), 0);
		return ret;
	}

	return sg_alloc_table_from_pages(sgt, pages, count, 0, size,
					 GFP_KERNEL);
}

static int __dma_direction_to_prot(enum dma_data_direction dir)
{
	switch (dir) {
	case DMA_BIDIRECTIONAL:
		return IOMMU_READ | IOMMU_WRITE;
	case DMA_TO_DEVICE:
		return IOMMU_READ;
	case DMA_FROM_DEVICE:
		return IOMMU_WRITE;
	default:
		return 0;
	}
}

static int arm_iommu_map_sg(struct device *dev, struct scatterlist *sg,
			    int nents, enum dma_data_direction dir,
			    unsigned long attrs)
{
	struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);
	unsigned int total_length = 0, current_offset = 0;
	int prot = __dma_direction_to_prot(dir);
	bool coherent = is_dma_coherent(dev, attrs);
	struct scatterlist *s;
	dma_addr_t iova;
	size_t ret;
	int i;

	for_each_sg(sg, s, nents, i) {
		total_length += s->length;
		if (!coherent && !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
			__dma_page_cpu_to_dev(sg_page(s), s->offset,
					      s->length, dir);
	}

	iova = __alloc_iova(mapping, total_length);
	if (iova == DMA_MAPPING_ERROR) {
		dev_err(dev, "Couldn't allocate iova for sg %p\n", sg);
		return 0;
	}
	prot = __get_iommu_pgprot(attrs, prot, coherent);

	ret = iommu_map_sg(mapping->domain, iova, sg, nents, prot);
	if (ret != total_length) {
		__free_iova(mapping, iova, total_length);
		return 0;
	}

	for_each_sg(sg, s, nents, i) {
		s->dma_address = iova + current_offset;
		s->dma_length = total_length - current_offset;
		current_offset += s->length;
	}

	return nents;
}

static void arm_iommu_unmap_sg(struct device *dev, struct scatterlist *sg,
			       int nents, enum dma_data_direction dir,
			       unsigned long attrs)
{
	struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);
	unsigned int total_length = sg_dma_len(sg);
	dma_addr_t iova = sg_dma_address(sg);
	struct scatterlist *s;
	int i;

	if (!is_dma_coherent(dev, attrs) && !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		for_each_sg(sg, s, nents, i)
			__dma_page_dev_to_cpu(sg_page(s), s->offset,
					      s->length, dir);

	total_length = PAGE_ALIGN((iova & ~PAGE_MASK) + total_length);
	iova &= PAGE_MASK;

	iommu_unmap(mapping->domain, iova, total_length);
	__free_iova(mapping, iova, total_length);
}

static void arm_iommu_sync_sg_for_cpu(struct device *dev,
				      struct scatterlist *sg, int nents,
				      enum dma_data_direction dir)
{
	struct scatterlist *s;
	int i;

	if (dev_is_dma_coherent(dev))
		return;
	for_each_sg(sg, s, nents, i)
		__dma_page_dev_to_cpu(sg_page(s), s->offset, s->length, dir);
}

static void arm_iommu_sync_sg_for_device(struct device *dev,
					 struct scatterlist *sg, int nents,
					 enum dma_data_direction dir)
{
	struct scatterlist *s;
	int i;

	if (dev_is_dma_coherent(dev))
		return;
	for_each_sg(sg, s, nents, i)
		__dma_page_cpu_to_dev(sg_page(s), s->offset, s->length, dir);
}

static dma_addr_t arm_iommu_map_page(struct device *dev, struct page *page,
				     unsigned long offset, size_t size,
				     enum dma_data_direction dir,
				     unsigned long attrs)
{
	struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);
	bool coherent = is_dma_coherent(dev, attrs);
	int ret, prot, len, start_offset, map_offset;
	dma_addr_t dma_addr;

	if (!coherent && !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		__dma_page_cpu_to_dev(page, offset, size, dir);

	map_offset = offset & ~PAGE_MASK;
	start_offset = offset & PAGE_MASK;
	len = PAGE_ALIGN(map_offset + size);

	dma_addr = __alloc_iova(mapping, len);
	if (dma_addr == DMA_MAPPING_ERROR)
		return dma_addr;

	prot = __dma_direction_to_prot(dir);
	prot = __get_iommu_pgprot(attrs, prot, coherent);

	ret = iommu_map(mapping->domain, dma_addr,
			page_to_phys(page) + start_offset, len, prot);
	if (ret < 0) {
		__free_iova(mapping, dma_addr, len);
		return DMA_MAPPING_ERROR;
	}

	return dma_addr + map_offset;
}

static void arm_iommu_unmap_page(struct device *dev, dma_addr_t handle,
				 size_t size, enum dma_data_direction dir,
				 unsigned long attrs)
{
	struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);
	dma_addr_t iova = handle & PAGE_MASK;
	int offset = handle & ~PAGE_MASK;
	int len = PAGE_ALIGN(size + offset);
	phys_addr_t phys = iommu_iova_to_phys(mapping->domain, iova);

	if (phys && !(is_dma_coherent(dev, attrs) ||
		      (attrs & DMA_ATTR_SKIP_CPU_SYNC)))
		__dma_page_dev_to_cpu(phys_to_page(phys), offset, size, dir);

	iommu_unmap(mapping->domain, iova, len);
	__free_iova(mapping, iova, len);
}

static void arm_iommu_sync_single_for_cpu(struct device *dev,
					  dma_addr_t handle, size_t size,
					  enum dma_data_direction dir)
{
	struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);
	phys_addr_t phys = iommu_iova_to_phys(mapping->domain,
					      handle & PAGE_MASK);

	if (phys && !dev_is_dma_coherent(dev))
		__dma_page_dev_to_cpu(phys_to_page(phys),
				      handle & ~PAGE_MASK, size, dir);
}

static void arm_iommu_sync_single_for_device(struct device *dev,
					     dma_addr_t handle, size_t size,
					     enum dma_data_direction dir)
{
	struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);
	phys_addr_t phys = iommu_iova_to_phys(mapping->domain,
					      handle & PAGE_MASK);

	if (phys && !dev_is_dma_coherent(dev))
		__dma_page_cpu_to_dev(phys_to_page(phys),
				      handle & ~PAGE_MASK, size, dir);
}

static dma_addr_t arm_iommu_dma_map_resource(struct device *dev,
					     phys_addr_t phys_addr,
					     size_t size,
					     enum dma_data_direction dir,
					     unsigned long attrs)
{
	struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);
	size_t offset = phys_addr & ~PAGE_MASK;
	size_t len = PAGE_ALIGN(size + offset);
	dma_addr_t dma_addr;
	int prot;

	dma_addr = __alloc_iova(mapping, len);
	if (dma_addr == DMA_MAPPING_ERROR)
		return dma_addr;

	prot = __dma_direction_to_prot(dir) | IOMMU_MMIO;

	if (iommu_map(mapping->domain, dma_addr, phys_addr - offset,
		      len, prot)) {
		__free_iova(mapping, dma_addr, len);
		return DMA_MAPPING_ERROR;
	}
	return dma_addr + offset;
}

static void arm_iommu_dma_unmap_resource(struct device *dev, dma_addr_t addr,
					 size_t size,
					 enum dma_data_direction dir,
					 unsigned long attrs)
{
	struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);
	size_t offset = addr & ~PAGE_MASK;
	size_t len = PAGE_ALIGN(size + offset);

	iommu_unmap(mapping->domain, addr - offset, len);
	__free_iova(mapping, addr - offset, len);
}

static int arm_iommu_dma_supported(struct device *dev, u64 mask)
{
	return 1;
}

static const struct dma_map_ops caf_iommu_ops = {
	.alloc			= arm_iommu_alloc_attrs,
	.free			= arm_iommu_free_attrs,
	.mmap			= arm_iommu_mmap_attrs,
	.get_sgtable		= arm_iommu_get_sgtable,

	.map_page		= arm_iommu_map_page,
	.unmap_page		= arm_iommu_unmap_page,
	.sync_single_for_cpu	= arm_iommu_sync_single_for_cpu,
	.sync_single_for_device	= arm_iommu_sync_single_for_device,

	.map_sg			= arm_iommu_map_sg,
	.unmap_sg		= arm_iommu_unmap_sg,
	.sync_sg_for_cpu	= arm_iommu_sync_sg_for_cpu,
	.sync_sg_for_device	= arm_iommu_sync_sg_for_device,

	.map_resource		= arm_iommu_dma_map_resource,
	.unmap_resource		= arm_iommu_dma_unmap_resource,

	.dma_supported		= arm_iommu_dma_supported,
};

/**
 * arm_iommu_create_mapping
 * @bus: pointer to the bus holding the client device (for IOMMU calls)
 * @base: start address of the valid IO address space
 * @size: maximum size of the valid IO address space
 *
 * Clients may use iommu_domain_set_attr() to set additional flags prior
 * to calling arm_iommu_attach_device() to complete initialization.
 */
struct dma_iommu_mapping *
arm_iommu_create_mapping(struct bus_type *bus, dma_addr_t base, size_t size)
{
	unsigned int bits = size >> PAGE_SHIFT;
	struct dma_iommu_mapping *mapping;

	if (!bits)
		return ERR_PTR(-EINVAL);

	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping)
		return ERR_PTR(-ENOMEM);

	mapping->base = base;
	mapping->bits = bits;

	mapping->domain = iommu_domain_alloc(bus);
	if (!mapping->domain) {
		kfree(mapping);
		return ERR_PTR(-ENOMEM);
	}

	mapping->init = false;
	return mapping;
}
EXPORT_SYMBOL(arm_iommu_create_mapping);

static int arm_iommu_init_mapping(struct device *dev,
				  struct dma_iommu_mapping *mapping)
{
	unsigned int bitmap_size = BITS_TO_LONGS(mapping->bits) * sizeof(long);
	dma_addr_t iova_end;

	if (mapping->init) {
		kref_get(&mapping->kref);
		return 0;
	}

	iova_end = mapping->base + ((dma_addr_t)mapping->bits << PAGE_SHIFT)
		   - 1;
	if (iova_end > dma_get_mask(dev)) {
		dev_err(dev, "dma mask %llx too small for requested iova range %pad to %pad\n",
			dma_get_mask(dev), &mapping->base, &iova_end);
		return -EINVAL;
	}

	mapping->bitmap = kvzalloc(bitmap_size, GFP_KERNEL);
	if (!mapping->bitmap)
		return -ENOMEM;

	spin_lock_init(&mapping->lock);
	mapping->ops = &caf_iommu_ops;
	kref_init(&mapping->kref);
	mapping->init = true;
	return 0;
}

static void arm_iommu_release(struct kref *kref)
{
	struct dma_iommu_mapping *mapping =
		container_of(kref, struct dma_iommu_mapping, kref);

	kvfree(mapping->bitmap);
	iommu_domain_free(mapping->domain);
	kfree(mapping);
}

/*
 * arm_iommu_release_mapping
 * @mapping: allocted via arm_iommu_create_mapping()
 *
 * Frees all resources associated with the iommu mapping.
 * The device associated with this mapping must be in the 'detached' state
 */
void arm_iommu_release_mapping(struct dma_iommu_mapping *mapping)
{
	if (!mapping)
		return;

	if (!mapping->init) {
		iommu_domain_free(mapping->domain);
		kfree(mapping);
		return;
	}

	kref_put(&mapping->kref, arm_iommu_release);
}
EXPORT_SYMBOL(arm_iommu_release_mapping);

/**
 * arm_iommu_attach_device
 * @dev: valid struct device pointer
 * @mapping: io address space mapping structure (returned from
 *	arm_iommu_create_mapping)
 *
 * Moves the device's group from its default (iommu-dma) domain to the
 * mapping's domain and replaces the device's dma_map_ops.
 */
int arm_iommu_attach_device(struct device *dev,
			    struct dma_iommu_mapping *mapping)
{
	struct iommu_group *group = iommu_group_get(dev);
	struct iommu_domain *cur;
	int err;

	if (!group) {
		dev_err(dev, "No iommu associated with device\n");
		return -EINVAL;
	}

	cur = iommu_get_domain_for_dev(dev);
	if (cur && cur != iommu_group_default_domain(group)) {
		dev_err(dev, "Device already attached to other iommu_domain\n");
		err = -EINVAL;
		goto out;
	}

	err = iommu_attach_group(mapping->domain, group);
	if (err)
		goto out;

	err = arm_iommu_init_mapping(dev, mapping);
	if (err) {
		iommu_detach_group(mapping->domain, group);
		goto out;
	}

	dev->archdata.mapping = mapping;
	dev->archdata.saved_dma_ops = get_dma_ops(dev);
	set_dma_ops(dev, mapping->ops);

	pr_debug("Attached IOMMU controller to %s device.\n", dev_name(dev));
out:
	iommu_group_put(group);
	return err;
}
EXPORT_SYMBOL(arm_iommu_attach_device);

/**
 * arm_iommu_detach_device
 * @dev: valid struct device pointer
 *
 * Detaches the provided device from a previously attached map and gives
 * it back its default domain and dma_map_ops.
 */
void arm_iommu_detach_device(struct device *dev)
{
	struct dma_iommu_mapping *mapping = to_dma_iommu_mapping(dev);
	struct iommu_group *group;

	if (!mapping) {
		dev_warn(dev, "Not attached\n");
		return;
	}

	group = iommu_group_get(dev);
	if (!group) {
		dev_err(dev, "No iommu associated with device\n");
		return;
	}

	iommu_detach_group(mapping->domain, group);
	iommu_group_put(group);

	dev->archdata.mapping = NULL;
	set_dma_ops(dev, dev->archdata.saved_dma_ops);

	pr_debug("Detached IOMMU controller from %s device.\n", dev_name(dev));
}
EXPORT_SYMBOL(arm_iommu_detach_device);
