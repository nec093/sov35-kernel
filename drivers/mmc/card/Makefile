#
# Makefile for MMC/SD card drivers
#

# CAF's mmc_block (block.o+queue.o) used the legacy single-queue block
# layer API, entirely removed by mainline's blk-mq transition. Rather
# than porting that rewrite by hand, use pristine v5.4.302's own
# drivers/mmc/core/{block,queue}.c (already blk-mq + CQE based, and
# was sitting there unused since the original wholesale checkout) --
# see drivers/mmc/core/Makefile. sdhci-msm.c doesn't implement CQE
# (uses its own older cmdq_hci.c instead of cqhci.c), but pristine's
# block/queue.c handle a CQE-incapable host fine via the ordinary
# non-command-queued blk-mq path -- see include/linux/mmc/host.h's
# struct mmc_cqe_ops comment.
# obj-$(CONFIG_MMC_BLOCK)		+= mmc_block.o
# mmc_block-objs			:= block.o queue.o
obj-$(CONFIG_MMC_TEST)		+= mmc_test.o

obj-$(CONFIG_SDIO_UART)		+= sdio_uart.o

obj-$(CONFIG_MMC_BLOCK_TEST)		+= mmc_block_test.o
