# SPDX-License-Identifier: GPL-2.0
#
# Generic Hardware Spinlock framework
#

obj-$(CONFIG_HWSPINLOCK)		+= hwspinlock_core.o
obj-$(CONFIG_HWSPINLOCK_OMAP)		+= omap_hwspinlock.o
obj-$(CONFIG_HWSPINLOCK_QCOM)		+= qcom_hwspinlock.o
obj-$(CONFIG_HWSPINLOCK_SIRF)		+= sirf_hwspinlock.o
obj-$(CONFIG_HWSPINLOCK_SPRD)		+= sprd_hwspinlock.o
obj-$(CONFIG_HWSPINLOCK_STM32)		+= stm32_hwspinlock.o
obj-$(CONFIG_HSEM_U8500)		+= u8500_hsem.o
# CAF addition (not in mainline): Kconfig entry (REMOTE_SPINLOCK_MSM)
# already exists above, but this Makefile line was missing entirely.
obj-$(CONFIG_REMOTE_SPINLOCK_MSM)	+= msm_remote_spinlock.o
