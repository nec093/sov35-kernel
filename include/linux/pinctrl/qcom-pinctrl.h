/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * TLMM extras of newer Qualcomm pinctrl drivers, used by the audio
 * techpack. The msm8996 TLMM driver has no MPM wakeup register control
 * (wakeup routing is done by the MPM irqchip), so these are no-ops.
 */
#ifndef __LINUX_PINCTRL_MSM_H__
#define __LINUX_PINCTRL_MSM_H__

#include <linux/types.h>

static inline int msm_gpio_mpm_wake_set(unsigned int gpio, bool enable)
{
	return 0;
}

#endif /* __LINUX_PINCTRL_MSM_H__ */
