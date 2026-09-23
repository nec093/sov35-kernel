/*
 * Copyright (c) 2013,2017-2018 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _ASM_DMA_CONTIGUOUS_H
#define _ASM_DMA_CONTIGUOUS_H

#ifdef __KERNEL__

#include <linux/types.h>

/*
 * CAF addition (not in mainline): declared but never given a body
 * anywhere in this tree. The "early fixup" this hooked was an arm32-era
 * workaround (remapping memory attributes for CMA regions on SoCs with
 * non-coherent DMA); arm64's cache/memory-attribute handling doesn't
 * need it, matching how include/asm-generic/dma-contiguous.h (the
 * fallback for archs that don't need special handling) implements it.
 */
static inline void dma_contiguous_early_fixup(phys_addr_t base,
					       unsigned long size)
{
}

#endif
#endif
