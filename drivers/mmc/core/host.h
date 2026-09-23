/*
 *  linux/drivers/mmc/core/host.h
 *
 *  Copyright (C) 2003 Russell King, All Rights Reserved.
 *  Copyright 2007 Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _MMC_CORE_HOST_H
#define _MMC_CORE_HOST_H
#include <linux/mmc/host.h>

#define cls_dev_to_mmc_host(d)	container_of(d, struct mmc_host, class_dev)

int mmc_register_host_class(void);
void mmc_unregister_host_class(void);

void mmc_retune_enable(struct mmc_host *host);
void mmc_retune_disable(struct mmc_host *host);
void mmc_retune_hold(struct mmc_host *host);
void mmc_retune_release(struct mmc_host *host);
int mmc_retune(struct mmc_host *host);

void mmc_latency_hist_sysfs_init(struct mmc_host *host);
void mmc_latency_hist_sysfs_exit(struct mmc_host *host);

/*
 * Mainline additions (not in CAF), used by drivers/mmc/core/block.c
 * (pristine v5.4.302's blk-mq block driver, see
 * drivers/mmc/core/Makefile).
 */
static inline void mmc_retune_hold_now(struct mmc_host *host)
{
	host->retune_now = 0;
	host->hold_retune += 1;
}

/*
 * mmc_retune_recheck() is NOT duplicated here: it already exists,
 * identically, as a static inline in the public
 * include/linux/mmc/host.h.
 */

static inline bool mmc_host_done_complete(struct mmc_host *host)
{
	return host->caps & MMC_CAP_DONE_COMPLETE;
}

/*
 * mmc_pre_req()/mmc_post_req() are NOT defined here: CAF's own core.c
 * already has static 3-arg versions
 * (mmc_pre_req(host, mrq, is_first_req)) doing the same
 * host->ops->pre_req/post_req dispatch; see core.c and core.h for the
 * non-static, 2-arg-compatible declarations block.c actually calls.
 */

#endif

