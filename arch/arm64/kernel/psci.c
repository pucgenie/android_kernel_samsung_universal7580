/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Copyright (C) 2013 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#define pr_fmt(fmt) "psci: " fmt

#include <linux/cpuidle.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/slab.h>

#include <asm/compiler.h>
#include <asm/cpu_ops.h>
#include <asm/errno.h>
#include <asm/psci.h>
#include <asm/smp_plat.h>
#include <asm/suspend.h>

#define PSCI_POWER_STATE_TYPE_STANDBY		0
#define PSCI_POWER_STATE_TYPE_POWER_DOWN	1

#define PSCI_INDEX_SLEEP	0x8

struct psci_power_state {
	u16	id;
	u8	type;
	u8	affinity_level;
};

struct psci_operations {
	int (*cpu_suspend)(struct psci_power_state state,
			   unsigned long entry_point);
	int (*cpu_off)(struct psci_power_state state);
	int (*cpu_on)(unsigned long cpuid, unsigned long entry_point);
	int (*migrate)(unsigned long cpuid);
};

static struct psci_operations psci_ops;

static int (*invoke_psci_fn)(u64, u64, u64, u64);

enum psci_function {
	PSCI_FN_CPU_SUSPEND,
	PSCI_FN_CPU_ON,
	PSCI_FN_CPU_OFF,
	PSCI_FN_MIGRATE,
	PSCI_FN_MAX,
};

static DEFINE_PER_CPU_READ_MOSTLY(struct psci_power_state *, psci_power_state);

static u32 psci_function_id[PSCI_FN_MAX];

#define PSCI_RET_SUCCESS		0
#define PSCI_RET_EOPNOTSUPP		-1
#define PSCI_RET_EINVAL			-2
#define PSCI_RET_EPERM			-3

static int psci_to_linux_errno(int errno)
{
	switch (errno) {
	case PSCI_RET_SUCCESS:
		return 0;
	case PSCI_RET_EOPNOTSUPP:
		return -EOPNOTSUPP;
	case PSCI_RET_EINVAL:
		return -EINVAL;
	case PSCI_RET_EPERM:
		return -EPERM;
	};

	return -EINVAL;
}

#define PSCI_POWER_STATE_ID_MASK	0xffff
#define PSCI_POWER_STATE_ID_SHIFT	0
#define PSCI_POWER_STATE_TYPE_MASK	0x1
#define PSCI_POWER_STATE_TYPE_SHIFT	16
#define PSCI_POWER_STATE_AFFL_MASK	0x3
#define PSCI_POWER_STATE_AFFL_SHIFT	24

static u32 psci_power_state_pack(struct psci_power_state state)
{
	return	((state.id & PSCI_POWER_STATE_ID_MASK)
			<< PSCI_POWER_STATE_ID_SHIFT)	|
		((state.type & PSCI_POWER_STATE_TYPE_MASK)
			<< PSCI_POWER_STATE_TYPE_SHIFT)	|
		((state.affinity_level & PSCI_POWER_STATE_AFFL_MASK)
			<< PSCI_POWER_STATE_AFFL_SHIFT);
}

static void psci_power_state_unpack(u32 power_state,
				    struct psci_power_state *state)
{
	state->id = (power_state >> PSCI_POWER_STATE_ID_SHIFT)
			& PSCI_POWER_STATE_ID_MASK;
	state->type = (power_state >> PSCI_POWER_STATE_TYPE_SHIFT)
			& PSCI_POWER_STATE_TYPE_MASK;
	state->affinity_level = (power_state >> PSCI_POWER_STATE_AFFL_SHIFT)
			& PSCI_POWER_STATE_AFFL_MASK;
}

/*
 * The following two functions are invoked via the invoke_psci_fn pointer
 * and will not be inlined, allowing us to piggyback on the AAPCS.
 */
static noinline int __invoke_psci_fn_hvc(u64 function_id, u64 arg0, u64 arg1,
					 u64 arg2)
{
	asm volatile(
			__asmeq("%0", "x0")
			__asmeq("%1", "x1")
			__asmeq("%2", "x2")
			__asmeq("%3", "x3")
			"hvc	#0\n"
		: "+r" (function_id)
		: "r" (arg0), "r" (arg1), "r" (arg2));

	return function_id;
}

static noinline int __invoke_psci_fn_smc(u64 function_id, u64 arg0, u64 arg1,
					 u64 arg2)
{
	asm volatile(
			__asmeq("%0", "x0")
			__asmeq("%1", "x1")
			__asmeq("%2", "x2")
			__asmeq("%3", "x3")
			"smc	#0\n"
		: "+r" (function_id)
		: "r" (arg0), "r" (arg1), "r" (arg2));

	return function_id;
}

static int psci_cpu_suspend(struct psci_power_state state,
			    unsigned long entry_point)
{
	int err;
	u32 fn, power_state;

	fn = psci_function_id[PSCI_FN_CPU_SUSPEND];
	power_state = psci_power_state_pack(state);
	err = invoke_psci_fn(fn, power_state, entry_point, 0);
	return psci_to_linux_errno(err);
}

static int psci_cpu_off(struct psci_power_state state)
{
	int err;
	u32 fn, power_state;

	fn = psci_function_id[PSCI_FN_CPU_OFF];
	power_state = psci_power_state_pack(state);
	err = invoke_psci_fn(fn, power_state, 0, 0);
	return psci_to_linux_errno(err);
}

static int psci_cpu_on(unsigned long cpuid, unsigned long entry_point)
{
	int err;
	u32 fn;

	fn = psci_function_id[PSCI_FN_CPU_ON];
	err = invoke_psci_fn(fn, cpuid, entry_point, 0);
	return psci_to_linux_errno(err);
}

static int psci_migrate(unsigned long cpuid)
{
	int err;
	u32 fn;

	fn = psci_function_id[PSCI_FN_MIGRATE];
	err = invoke_psci_fn(fn, cpuid, 0, 0);
	return psci_to_linux_errno(err);
}

static const struct of_device_id psci_of_match[] __initconst = {
	{ .compatible = "arm,psci",	},
	{},
};

int __init psci_dt_register_idle_states(struct cpuidle_driver *drv,
					struct device_node *state_nodes[])
{
	int cpu, i;
	struct psci_power_state *psci_states;
	const struct cpu_operations *cpu_ops_ptr;

	if (!state_nodes)
		return -EINVAL;
	/*
	 * This is belt-and-braces: make sure that if the idle
	 * specified protocol is psci, the cpu_ops have been
	 * initialized to psci operations. Anything else is
	 * a recipe for mayhem.
	 */
	for_each_cpu(cpu, drv->cpumask) {
		cpu_ops_ptr = cpu_ops[cpu];
		if (WARN_ON(!cpu_ops_ptr || strcmp(cpu_ops_ptr->name, "psci")))
			return -EOPNOTSUPP;
	}

	psci_states = kcalloc(drv->state_count, sizeof(*psci_states),
			      GFP_KERNEL);

	if (!psci_states) {
		pr_warn("psci idle state allocation failed\n");
		return -ENOMEM;
	}

	for_each_cpu(cpu, drv->cpumask) {
		if (per_cpu(psci_power_state, cpu)) {
			pr_warn("idle states already initialized on cpu %u\n",
				cpu);
			continue;
		}
		per_cpu(psci_power_state, cpu) = psci_states;
	}


	for (i = 0; i < drv->state_count; i++) {
		u32 psci_power_state;

		if (!state_nodes[i]) {
			/*
			 * An index with a missing node pointer falls back to
			 * simple STANDBYWFI
			 */
			psci_states[i].type = PSCI_POWER_STATE_TYPE_STANDBY;
			continue;
		}

		if (of_property_read_u32(state_nodes[i], "entry-method-param",
					 &psci_power_state)) {
			pr_warn(" * %s missing entry-method-param property\n",
				state_nodes[i]->full_name);
			/*
			 * If entry-method-param property is missing, fall
			 * back to STANDBYWFI state
			 */
			psci_states[i].type = PSCI_POWER_STATE_TYPE_STANDBY;
			continue;
		}

		pr_debug("psci-power-state %#x index %u\n",
			 psci_power_state, i);
		psci_power_state_unpack(psci_power_state, &psci_states[i]);
	}

	return 0;
}

void __init psci_init(void)
{
	struct device_node *np;
	const char *method;
	u32 id;

	np = of_find_matching_node(NULL, psci_of_match);
	if (!np)
		return;

	pr_info("probing function IDs from device-tree\n");

	if (of_property_read_string(np, "method", &method)) {
		pr_warning("missing \"method\" property\n");
		goto out_put_node;
	}

	if (!strcmp("hvc", method)) {
		invoke_psci_fn = __invoke_psci_fn_hvc;
	} else if (!strcmp("smc", method)) {
		invoke_psci_fn = __invoke_psci_fn_smc;
	} else {
		pr_warning("invalid \"method\" property: %s\n", method);
		goto out_put_node;
	}

	if (!of_property_read_u32(np, "cpu_suspend", &id)) {
		psci_function_id[PSCI_FN_CPU_SUSPEND] = id;
		psci_ops.cpu_suspend = psci_cpu_suspend;
	}

	if (!of_property_read_u32(np, "cpu_off", &id)) {
		psci_function_id[PSCI_FN_CPU_OFF] = id;
		psci_ops.cpu_off = psci_cpu_off;
	}

	if (!of_property_read_u32(np, "cpu_on", &id)) {
		psci_function_id[PSCI_FN_CPU_ON] = id;
		psci_ops.cpu_on = psci_cpu_on;
	}

	if (!of_property_read_u32(np, "migrate", &id)) {
		psci_function_id[PSCI_FN_MIGRATE] = id;
		psci_ops.migrate = psci_migrate;
	}

out_put_node:
	of_node_put(np);
	return;
}

#ifdef CONFIG_SMP

static int __init cpu_psci_cpu_init(struct device_node *dn, unsigned int cpu)
{
	return 0;
}

static int __init cpu_psci_cpu_prepare(unsigned int cpu)
{
	if (!psci_ops.cpu_on) {
		pr_err("no cpu_on method, not booting CPU%d\n", cpu);
		return -ENODEV;
	}

	return 0;
}

static int cpu_psci_cpu_boot(unsigned int cpu)
{
	int err = psci_ops.cpu_on(cpu_logical_map(cpu), __pa(secondary_entry));
	if (err)
		pr_err("failed to boot CPU%d (%d)\n", cpu, err);

	return err;
}

#ifdef CONFIG_HOTPLUG_CPU
static int cpu_psci_cpu_disable(unsigned int cpu)
{
	/* Fail early if we don't have CPU_OFF support */
	if (!psci_ops.cpu_off)
		return -EOPNOTSUPP;
	return 0;
}

static void cpu_psci_cpu_die(unsigned int cpu)
{
	/*
	 * There are no known implementations of PSCI actually using the
	 * power state field, pass a sensible default for now.
	 */
	struct psci_power_state state = {
		.type = PSCI_POWER_STATE_TYPE_POWER_DOWN,
	};

	psci_ops.cpu_off(state);
}
#endif

#ifdef CONFIG_ARM64_CPU_SUSPEND
static int cpu_psci_cpu_suspend(unsigned long index)
{
	struct psci_power_state *state = __get_cpu_var(psci_power_state);

	if (index == PSCI_INDEX_SLEEP) {
		struct psci_power_state s = {
			.affinity_level = 3,
		};

		return psci_ops.cpu_suspend(s, virt_to_phys(cpu_resume));
	}

	if (!state)
		return -EOPNOTSUPP;

	return psci_ops.cpu_suspend(state[index], virt_to_phys(cpu_resume));
}
#endif

const struct cpu_operations cpu_psci_ops = {
	.name		= "psci",
	.cpu_init	= cpu_psci_cpu_init,
	.cpu_prepare	= cpu_psci_cpu_prepare,
	.cpu_boot	= cpu_psci_cpu_boot,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_disable	= cpu_psci_cpu_disable,
	.cpu_die	= cpu_psci_cpu_die,
#endif
#ifdef CONFIG_ARM64_CPU_SUSPEND
	.cpu_suspend	= cpu_psci_cpu_suspend,
#endif
};

#endif
