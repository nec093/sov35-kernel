/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Private header for the mmc subsystem
 *
 * Copyright (C) 2016 Linaro Ltd
 *
 * Author: Ulf Hansson <ulf.hansson@linaro.org>
 */

#ifndef _MMC_CORE_CARD_H
#define _MMC_CORE_CARD_H

#include <linux/mmc/card.h>

#define mmc_card_name(c)	((c)->cid.prod_name)
#define mmc_card_id(c)		(dev_name(&(c)->dev))
#define mmc_dev_to_card(d)	container_of(d, struct mmc_card, dev)

/* Card states */
#define MMC_STATE_PRESENT	(1<<0)		/* present in sysfs */
#define MMC_STATE_READONLY	(1<<1)		/* card is read-only */
#define MMC_STATE_BLOCKADDR	(1<<2)		/* card uses block-addressing */
#define MMC_CARD_SDXC		(1<<3)		/* card is SDXC */
#define MMC_CARD_REMOVED	(1<<4)		/* card has been removed */
#define MMC_STATE_SUSPENDED	(1<<5)		/* card is suspended */

#define mmc_card_present(c)	((c)->state & MMC_STATE_PRESENT)
#define mmc_card_readonly(c)	((c)->state & MMC_STATE_READONLY)
#define mmc_card_blockaddr(c)	((c)->state & MMC_STATE_BLOCKADDR)
#define mmc_card_ext_capacity(c) ((c)->state & MMC_CARD_SDXC)
#define mmc_card_removed(c)	((c) && ((c)->state & MMC_CARD_REMOVED))
#define mmc_card_suspended(c)	((c)->state & MMC_STATE_SUSPENDED)

#define mmc_card_set_present(c)	((c)->state |= MMC_STATE_PRESENT)
#define mmc_card_set_readonly(c) ((c)->state |= MMC_STATE_READONLY)
#define mmc_card_set_blockaddr(c) ((c)->state |= MMC_STATE_BLOCKADDR)
#define mmc_card_set_ext_capacity(c) ((c)->state |= MMC_CARD_SDXC)
#define mmc_card_set_removed(c) ((c)->state |= MMC_CARD_REMOVED)
#define mmc_card_set_suspended(c) ((c)->state |= MMC_STATE_SUSPENDED)
#define mmc_card_clr_suspended(c) ((c)->state &= ~MMC_STATE_SUSPENDED)

/*
 * struct mmc_fixup and the MMC_FIXUP()/SDIO_FIXUP()/add_quirk()-family
 * helpers pristine originally defined here privately, but CAF's own
 * include/linux/mmc/card.h (the public header, already #included
 * above) defines the identical apparatus for wider reuse outside this
 * driver -- keeping both caused a redefinition error, so this driver-
 * private copy was dropped in favor of the public one.
 */

#define cid_rev(hwrev, fwrev, year, month)	\
	(((u64) hwrev) << 40 |			\
	 ((u64) fwrev) << 32 |			\
	 ((u64) year) << 16 |			\
	 ((u64) month))

#define cid_rev_card(card)			\
	cid_rev(card->cid.hwrev,		\
		    card->cid.fwrev,		\
		    card->cid.year,		\
		    card->cid.month)

/*
 * mmc_card_lenient_fn0()/mmc_blksz_for_byte_mode()/mmc_card_disable_cd()/
 * mmc_card_nonstd_func_interface()/mmc_card_broken_byte_mode_512()/
 * mmc_card_long_read_time()/mmc_card_broken_irq_polling()/
 * mmc_card_broken_hpi() were defined here too, identically, in
 * pristine v5.4.302 -- but CAF's public include/linux/mmc/card.h
 * (already included above) already provides the same definitions, so
 * this driver-private copy was dropped to avoid the redefinition.
 */

#endif
