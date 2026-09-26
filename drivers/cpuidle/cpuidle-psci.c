// SPDX-License-Identifier: GPL-2.0-only
/*
 * PSCI CPU idle driver.
 *
 * Copyright (C) 2019 ARM Ltd.
 * Author: Lorenzo Pieralisi <lorenzo.pieralisi@arm.com>
 */

#define pr_fmt(fmt) "CPUidle PSCI: " fmt

#include <linux/cpuidle.h>
#include <linux/cpumask.h>
#include <linux/cpu_pm.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/psci.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include <linux/topology.h>

#include <asm/cpuidle.h>

#include "dt_idle_states.h"

static DEFINE_PER_CPU_READ_MOSTLY(u32 *, psci_power_state);

/*
 * Some firmware (the MSM8996 TZ) only implements the OS-initiated suspend
 * mode and cannot be switched to platform coordination, so a request
 * that also powers down the cluster is DENIED unless every other online
 * core of that cluster already sits in a core power-down state. This
 * kernel has no PSCI power-domain hierarchy to do that election, so do
 * it here the way the CAF lpm-levels driver does, under a per-cluster
 * lock: the cluster state is only requested by the last core entering a
 * non-WFI state, only when no IPI is pending for its siblings and only
 * when the earliest wake-up of all the cluster's cores is far enough
 * away to cover the state's residency; otherwise the deepest core-only
 * state is used. A request the firmware still denies is not retried
 * right away, the CPU goes back through the idle loop instead.
 *
 * On MSM8996 both shortcuts hang the whole CPU subsystem within seconds
 * of boot (silent watchdog reset): collapsing the L2 while a sibling is
 * about to wake up, and re-entering the firmware straight after a denied
 * cluster request.
 *
 * Cluster states are recognised by a non-zero affinity level in
 * StateID[25:24], which is how the Qualcomm StateIDs encode it.
 */
#define PSCI_STATEID_AFF_LEVEL(s)	(((s) >> 24) & 0x3)

struct psci_cluster {
	raw_spinlock_t		lock;
	struct cpumask		idle;	/* cores in a non-WFI state */
};

#define PSCI_MAX_CLUSTERS	8

static struct psci_cluster psci_clusters[PSCI_MAX_CLUSTERS] = {
	[0 ... PSCI_MAX_CLUSTERS - 1] = {
		.lock = __RAW_SPIN_LOCK_UNLOCKED(psci_clusters.lock),
	},
};
static DEFINE_PER_CPU(struct psci_cluster *, psci_cpu_cluster);
static DEFINE_PER_CPU(ktime_t, psci_next_wake);

/* allows turning the cluster states off at runtime for debugging */
static bool cluster_idle = true;
module_param(cluster_idle, bool, 0644);

/* deepest core-only state below @idx, 0 if there is none */
static int psci_core_state(const u32 *state, int idx)
{
	while (--idx > 0)
		if (!PSCI_STATEID_AFF_LEVEL(state[idx - 1]))
			return idx;
	return 0;
}

/* called with cl->lock held */
static bool psci_cluster_can_collapse(struct psci_cluster *cl, int cpu,
				      ktime_t now, unsigned int residency_us)
{
	const struct cpumask *siblings = topology_core_cpumask(cpu);
	ktime_t earliest = KTIME_MAX;
	int c;

	for_each_cpu_and(c, siblings, cpu_online_mask) {
		if (!cpumask_test_cpu(c, &cl->idle))
			return false;
		if (c != cpu && per_cpu(pending_ipi, c))
			return false;
		earliest = min(earliest, per_cpu(psci_next_wake, c));
	}

	return ktime_us_delta(earliest, now) >= residency_us;
}

static int psci_enter_idle_state(struct cpuidle_device *dev,
				struct cpuidle_driver *drv, int idx)
{
	u32 *state = __this_cpu_read(psci_power_state);
	struct psci_cluster *cl = __this_cpu_read(psci_cpu_cluster);
	ktime_t now;
	u32 param;
	int ret;

	if (!idx || !cl)
		return CPU_PM_CPU_IDLE_ENTER_PARAM(psci_cpu_suspend_enter,
						   idx, state[idx - 1]);

	/*
	 * The clock event programmed for this CPU is when it will wake up
	 * next: the retained scheduler tick if the governor kept it, the
	 * next timer otherwise. tick_nohz_get_sleep_length() would ignore
	 * a retained tick (and must only be called by the governor).
	 */
	now = ktime_get();
	__this_cpu_write(psci_next_wake, tick_nohz_get_next_hrtimer());

	param = state[idx - 1];
	raw_spin_lock(&cl->lock);
	cpumask_set_cpu(dev->cpu, &cl->idle);
	if (PSCI_STATEID_AFF_LEVEL(param) && psci_core_state(state, idx) &&
	    (!READ_ONCE(cluster_idle) ||
	     !psci_cluster_can_collapse(cl, dev->cpu, now,
					drv->states[idx].target_residency))) {
		idx = psci_core_state(state, idx);
		param = state[idx - 1];
	}
	raw_spin_unlock(&cl->lock);

	ret = cpu_pm_enter();
	if (!ret) {
		ret = psci_cpu_suspend_enter(param);
		cpu_pm_exit();
	}

	raw_spin_lock(&cl->lock);
	cpumask_clear_cpu(dev->cpu, &cl->idle);
	raw_spin_unlock(&cl->lock);

	return ret ? -1 : idx;
}

static struct cpuidle_driver psci_idle_driver __initdata = {
	.name = "psci_idle",
	.owner = THIS_MODULE,
	/*
	 * PSCI idle states relies on architectural WFI to
	 * be represented as state index 0.
	 */
	.states[0] = {
		.enter                  = psci_enter_idle_state,
		.exit_latency           = 1,
		.target_residency       = 1,
		.power_usage		= UINT_MAX,
		.name                   = "WFI",
		.desc                   = "ARM WFI",
	}
};

static const struct of_device_id psci_idle_state_match[] __initconst = {
	{ .compatible = "arm,idle-state",
	  .data = psci_enter_idle_state },
	{ },
};

static int __init psci_dt_parse_state_node(struct device_node *np, u32 *state)
{
	int err = of_property_read_u32(np, "arm,psci-suspend-param", state);

	if (err) {
		pr_warn("%pOF missing arm,psci-suspend-param property\n", np);
		return err;
	}

	if (!psci_power_state_is_valid(*state)) {
		pr_warn("Invalid PSCI power state %#x\n", *state);
		return -EINVAL;
	}

	return 0;
}

static int __init psci_dt_cpu_init_idle(struct device_node *cpu_node, int cpu)
{
	int i, ret = 0, count = 0;
	u32 *psci_states;
	struct device_node *state_node;

	/* Count idle states */
	while ((state_node = of_parse_phandle(cpu_node, "cpu-idle-states",
					      count))) {
		count++;
		of_node_put(state_node);
	}

	if (!count)
		return -ENODEV;

	psci_states = kcalloc(count, sizeof(*psci_states), GFP_KERNEL);
	if (!psci_states)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		state_node = of_parse_phandle(cpu_node, "cpu-idle-states", i);
		ret = psci_dt_parse_state_node(state_node, &psci_states[i]);
		of_node_put(state_node);

		if (ret)
			goto free_mem;

		pr_debug("psci-power-state %#x index %d\n", psci_states[i], i);
	}

	/* Idle states parsed correctly, initialize per-cpu pointer */
	per_cpu(psci_power_state, cpu) = psci_states;

	for (i = 0; i < count; i++)
		if (PSCI_STATEID_AFF_LEVEL(psci_states[i]))
			break;
	if (i < count) {
		int id = topology_physical_package_id(cpu);

		if (id >= 0 && id < PSCI_MAX_CLUSTERS)
			per_cpu(psci_cpu_cluster, cpu) = &psci_clusters[id];
		else
			pr_warn("CPU%d: no cluster id, cluster states disabled\n",
				cpu);
	}
	return 0;

free_mem:
	kfree(psci_states);
	return ret;
}

static __init int psci_cpu_init_idle(unsigned int cpu)
{
	struct device_node *cpu_node;
	int ret;

	/*
	 * If the PSCI cpu_suspend function hook has not been initialized
	 * idle states must not be enabled, so bail out
	 */
	if (!psci_ops.cpu_suspend)
		return -EOPNOTSUPP;

	cpu_node = of_cpu_device_node_get(cpu);
	if (!cpu_node)
		return -ENODEV;

	ret = psci_dt_cpu_init_idle(cpu_node, cpu);

	of_node_put(cpu_node);

	return ret;
}

static int __init psci_idle_init_cpu(int cpu)
{
	struct cpuidle_driver *drv;
	struct device_node *cpu_node;
	const char *enable_method;
	int ret = 0;

	cpu_node = of_cpu_device_node_get(cpu);
	if (!cpu_node)
		return -ENODEV;

	/*
	 * Check whether the enable-method for the cpu is PSCI, fail
	 * if it is not.
	 */
	enable_method = of_get_property(cpu_node, "enable-method", NULL);
	if (!enable_method || (strcmp(enable_method, "psci")))
		ret = -ENODEV;

	of_node_put(cpu_node);
	if (ret)
		return ret;

	drv = kmemdup(&psci_idle_driver, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->cpumask = (struct cpumask *)cpumask_of(cpu);

	/*
	 * Initialize idle states data, starting at index 1, since
	 * by default idle state 0 is the quiescent state reached
	 * by the cpu by executing the wfi instruction.
	 *
	 * If no DT idle states are detected (ret == 0) let the driver
	 * initialization fail accordingly since there is no reason to
	 * initialize the idle driver if only wfi is supported, the
	 * default archictectural back-end already executes wfi
	 * on idle entry.
	 */
	ret = dt_init_idle_driver(drv, psci_idle_state_match, 1);
	if (ret <= 0) {
		ret = ret ? : -ENODEV;
		goto out_kfree_drv;
	}

	/*
	 * Initialize PSCI idle states.
	 */
	ret = psci_cpu_init_idle(cpu);
	if (ret) {
		pr_err("CPU %d failed to PSCI idle\n", cpu);
		goto out_kfree_drv;
	}

	ret = cpuidle_register(drv, NULL);
	if (ret)
		goto out_kfree_drv;

	return 0;

out_kfree_drv:
	kfree(drv);
	return ret;
}

/*
 * psci_idle_init - Initializes PSCI cpuidle driver
 *
 * Initializes PSCI cpuidle driver for all CPUs, if any CPU fails
 * to register cpuidle driver then rollback to cancel all CPUs
 * registration.
 */
static int __init psci_idle_init(void)
{
	int cpu, ret;
	struct cpuidle_driver *drv;
	struct cpuidle_device *dev;

	for_each_possible_cpu(cpu) {
		ret = psci_idle_init_cpu(cpu);
		if (ret)
			goto out_fail;
	}

	return 0;

out_fail:
	while (--cpu >= 0) {
		dev = per_cpu(cpuidle_devices, cpu);
		drv = cpuidle_get_cpu_driver(dev);
		cpuidle_unregister(drv);
		kfree(drv);
	}

	return ret;
}
device_initcall(psci_idle_init);
