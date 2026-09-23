/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/include/asm/system_misc.h
 *
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_SYSTEM_MISC_H
#define __ASM_SYSTEM_MISC_H

#ifndef __ASSEMBLY__

#include <linux/compiler.h>
#include <linux/linkage.h>
#include <linux/irqflags.h>
#include <linux/signal.h>
#include <linux/ratelimit.h>
#include <linux/reboot.h>

struct pt_regs;

void die(const char *msg, struct pt_regs *regs, int err);

struct siginfo;
void arm64_notify_die(const char *str, struct pt_regs *regs,
		      int signo, int sicode, void __user *addr,
		      int err);

void hook_debug_fault_code(int nr, int (*fn)(unsigned long, unsigned int,
					     struct pt_regs *),
			   int sig, int code, const char *name);

struct mm_struct;
extern void __show_regs(struct pt_regs *);

extern void (*arm_pm_restart)(enum reboot_mode reboot_mode, const char *cmd);

/*
 * CAF addition (not in mainline): optional hook a platform driver can set
 * to override the hardware-id string reported to userspace (originally
 * used to also print a "Hardware:" line in /proc/cpuinfo, a display
 * mainline itself has since dropped entirely for arm64; kept here as a
 * bare hook since drivers/soc/qcom/socinfo.c still assigns it).
 */
extern char *(*arch_read_hardware_id)(void);

#endif	/* __ASSEMBLY__ */

#endif	/* __ASM_SYSTEM_MISC_H */
