/*
 *  drivers/cpufreq/cpufreq_stats.c
 *
 *  Copyright (C) 2003-2004 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *  (C) 2004 Zou Nan hai <nanhai.zou@intel.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/sysfs.h>
#include <linux/cpufreq.h>
#include <linux/hashtable.h>
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/percpu.h>
#include <linux/kobject.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>
#include <linux/sort.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/proc_fs.h>
#include <linux/profile.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/cputime.h>
#ifdef CONFIG_BL_SWITCHER
#include <asm/bL_switcher.h>
#endif

#define UID_HASH_BITS 10

DECLARE_HASHTABLE(uid_hash_table, UID_HASH_BITS);

static spinlock_t cpufreq_stats_lock;

static DEFINE_SPINLOCK(cpufreq_stats_table_lock);
static DEFINE_SPINLOCK(task_time_in_state_lock); /* task->time_in_state */
static DEFINE_RT_MUTEX(uid_lock); /* uid_hash_table */

struct uid_entry {
	uid_t uid;
	unsigned int dead_max_states;
	unsigned int alive_max_states;
	u64 *dead_time_in_state;
	u64 *alive_time_in_state;
	struct hlist_node hash;
};

struct cpufreq_stats {
	unsigned int cpu;
	unsigned int total_trans;
	unsigned long long last_time;
	unsigned int max_state;
	unsigned int state_num;
	atomic_t cpu_freq_i;
	atomic_t all_freq_i;
	u64 *time_in_state;
	unsigned int *freq_table;
#ifdef CONFIG_CPU_FREQ_STAT_DETAILS
	unsigned int *trans_table;
#endif
};

struct cpufreq_power_stats {
	unsigned int state_num;
	unsigned int *curr;
	unsigned int *freq_table;
};

struct all_cpufreq_stats {
	unsigned int state_num;
	cputime64_t *time_in_state;
	unsigned int *freq_table;
};

struct all_freq_table {
	unsigned int *freq_table;
	unsigned int table_size;
};

static struct all_freq_table *all_freq_table;
static bool cpufreq_all_freq_init;

static DEFINE_PER_CPU(struct all_cpufreq_stats *, all_cpufreq_stats);
static DEFINE_PER_CPU(struct cpufreq_stats *, cpufreq_stats_table);
static DEFINE_PER_CPU(struct cpufreq_stats *, prev_cpufreq_stats_table);
static DEFINE_PER_CPU(struct cpufreq_power_stats *, cpufreq_power_stats);

struct cpufreq_stats_attribute {
	struct attribute attr;
	ssize_t(*show) (struct cpufreq_stats *, char *);
};

/* Caller must hold uid lock */
static struct uid_entry *find_uid_entry(uid_t uid)
{
	struct uid_entry *uid_entry;

	hash_for_each_possible(uid_hash_table, uid_entry, hash, uid) {
		if (uid_entry->uid == uid)
			return uid_entry;
	}
	return NULL;
}

/* Caller must hold uid lock */
static struct uid_entry *find_or_register_uid(uid_t uid)
{
	struct uid_entry *uid_entry;

	uid_entry = find_uid_entry(uid);
	if (uid_entry)
		return uid_entry;

	uid_entry = kzalloc(sizeof(struct uid_entry), GFP_ATOMIC);
	if (!uid_entry)
		return NULL;

	uid_entry->uid = uid;

	hash_add(uid_hash_table, &uid_entry->hash, uid);

	return uid_entry;
}


static int uid_time_in_state_show(struct seq_file *m, void *v)
{
	struct uid_entry *uid_entry;
	struct task_struct *task, *temp;
	unsigned long bkt, flags;
	int i;

	if (!all_freq_table || !cpufreq_all_freq_init)
		return 0;

	seq_puts(m, "uid:");
	for (i = 0; i < all_freq_table->table_size; ++i)
		seq_printf(m, " %d", all_freq_table->freq_table[i]);
	seq_putc(m, '\n');

	rt_mutex_lock(&uid_lock);

	rcu_read_lock();
	do_each_thread(temp, task) {

		uid_entry = find_or_register_uid(from_kuid_munged(
			current_user_ns(), task_uid(task)));
		if (!uid_entry)
			continue;

		if (uid_entry->alive_max_states < task->max_states) {
			uid_entry->alive_time_in_state = krealloc(
				uid_entry->alive_time_in_state,
				task->max_states *
				sizeof(uid_entry->alive_time_in_state[0]),
				GFP_ATOMIC);
			memset(uid_entry->alive_time_in_state +
				uid_entry->alive_max_states,
				0, (task->max_states -
				uid_entry->alive_max_states) *
				sizeof(uid_entry->alive_time_in_state[0]));
			uid_entry->alive_max_states = task->max_states;
		}

		spin_lock_irqsave(&task_time_in_state_lock, flags);
		if (task->time_in_state) {
			for (i = 0; i < task->max_states; ++i) {
				uid_entry->alive_time_in_state[i] +=
					atomic_read(&task->time_in_state[i]);
			}
		}
		spin_unlock_irqrestore(&task_time_in_state_lock, flags);

	} while_each_thread(temp, task);
	rcu_read_unlock();

	hash_for_each(uid_hash_table, bkt, uid_entry, hash) {
		int max_states = uid_entry->dead_max_states;

		if (uid_entry->alive_max_states > max_states)
			max_states = uid_entry->alive_max_states;
		if (max_states)
			seq_printf(m, "%d:", uid_entry->uid);
		for (i = 0; i < max_states; ++i) {
			u64 total_time_in_state = 0;

			if (uid_entry->dead_time_in_state &&
				i < uid_entry->dead_max_states) {
				total_time_in_state =
					uid_entry->dead_time_in_state[i];
			}
			if (uid_entry->alive_time_in_state &&
				i < uid_entry->alive_max_states) {
				total_time_in_state +=
					uid_entry->alive_time_in_state[i];
			}
			seq_printf(m, " %lu", (unsigned long)
				cputime_to_clock_t(total_time_in_state));
		}
		if (max_states)
			seq_putc(m, '\n');

		kfree(uid_entry->alive_time_in_state);
		uid_entry->alive_time_in_state = NULL;
		uid_entry->alive_max_states = 0;
	}

	rt_mutex_unlock(&uid_lock);
	return 0;
}

static int cpufreq_stats_update(unsigned int cpu)
{
	struct cpufreq_stats *stat;
	struct all_cpufreq_stats *all_stat;
	unsigned long long cur_time;

	cur_time = get_jiffies_64();
	spin_lock(&cpufreq_stats_lock);
	stat = per_cpu(cpufreq_stats_table, cpu);
	all_stat = per_cpu(all_cpufreq_stats, cpu);
	if (!stat) {
		spin_unlock(&cpufreq_stats_lock);
		return 0;
	}
	if (stat->time_in_state) {
		int cpu_freq_i = atomic_read(&stat->cpu_freq_i);

		stat->time_in_state[cpu_freq_i] += cur_time - stat->last_time;
		if (all_stat)
			all_stat->time_in_state[cpu_freq_i] +=
					cur_time - stat->last_time;
	}
	stat->last_time = cur_time;
	spin_unlock(&cpufreq_stats_lock);
	return 0;
}

void cpufreq_task_stats_init(struct task_struct *p)
{
	size_t alloc_size;
	void *temp;
	unsigned long flags;

	spin_lock_irqsave(&task_time_in_state_lock, flags);
	p->time_in_state = NULL;
	spin_unlock_irqrestore(&task_time_in_state_lock, flags);
	WRITE_ONCE(p->max_states, 0);

	if (!all_freq_table || !cpufreq_all_freq_init)
		return;

	WRITE_ONCE(p->max_states, all_freq_table->table_size);

	/* Create all_freq_table for clockticks in all possible freqs in all
	 * cpus
	 */
	alloc_size = p->max_states * sizeof(p->time_in_state[0]);
	temp = kzalloc(alloc_size, GFP_KERNEL);

	spin_lock_irqsave(&task_time_in_state_lock, flags);
	p->time_in_state = temp;
	spin_unlock_irqrestore(&task_time_in_state_lock, flags);
}

void cpufreq_task_stats_exit(struct task_struct *p)
{
	unsigned long flags;
	void *temp;

	spin_lock_irqsave(&task_time_in_state_lock, flags);
	temp = p->time_in_state;
	p->time_in_state = NULL;
	spin_unlock_irqrestore(&task_time_in_state_lock, flags);
	kfree(temp);
}

int proc_time_in_state_show(struct seq_file *m, struct pid_namespace *ns,
			    struct pid *pid, struct task_struct *p)
{
	int i;
	cputime_t cputime;
	unsigned long flags;

	if (!all_freq_table || !cpufreq_all_freq_init || !p->time_in_state)
		return 0;

	spin_lock(&cpufreq_stats_lock);
	for (i = 0; i < p->max_states; ++i) {
		cputime = 0;
		spin_lock_irqsave(&task_time_in_state_lock, flags);
		if (p->time_in_state)
			cputime = atomic_read(&p->time_in_state[i]);
		spin_unlock_irqrestore(&task_time_in_state_lock, flags);

		seq_printf(m, "%d %lu\n", all_freq_table->freq_table[i],
			(unsigned long)cputime_to_clock_t(cputime));
	}
	spin_unlock(&cpufreq_stats_lock);

	return 0;
}

static ssize_t show_total_trans(struct cpufreq_policy *policy, char *buf)
{
	struct cpufreq_stats *stat = per_cpu(cpufreq_stats_table, policy->cpu);
	if (!stat)
		return 0;
	return sprintf(buf, "%d\n",
			per_cpu(cpufreq_stats_table, stat->cpu)->total_trans);
}

static ssize_t show_time_in_state(struct cpufreq_policy *policy, char *buf)
{
	ssize_t len = 0;
	int i;
	struct cpufreq_stats *stat = per_cpu(cpufreq_stats_table, policy->cpu);
	if (!stat)
		return 0;
	cpufreq_stats_update(stat->cpu);
	for (i = 0; i < stat->state_num; i++) {
		len += sprintf(buf + len, "%u %llu\n", stat->freq_table[i],
			(unsigned long long)
			jiffies_64_to_clock_t(stat->time_in_state[i]));
	}
	return len;
}

static int get_index_all_cpufreq_stat(struct all_cpufreq_stats *all_stat,
		unsigned int freq)
{
	int i;
	if (!all_stat)
		return -1;
	for (i = 0; i < all_stat->state_num; i++) {
		if (all_stat->freq_table[i] == freq)
			return i;
	}
	return -1;
}

/* Called without cpufreq_stats_lock held */
void acct_update_power(struct task_struct *task, cputime_t cputime) {
	struct cpufreq_power_stats *powerstats;
	struct cpufreq_stats *stats;
	unsigned int cpu_num, curr;
	int cpu_freq_i;
	int all_freq_i;
	unsigned long flags;
	unsigned long stl_flags;

	if (!task)
		return;

	cpu_num = task_cpu(task);
	spin_lock_irqsave(&cpufreq_stats_table_lock, stl_flags);
	stats = per_cpu(cpufreq_stats_table, cpu_num);
	if (!stats)
		goto out;

	all_freq_i = atomic_read(&stats->all_freq_i);

	/* This function is called from a different context
	 * Interruptions in between reads/assignements are ok
	 */
	if (all_freq_table && cpufreq_all_freq_init &&
		!(task->flags & PF_EXITING) &&
		all_freq_i != -1 && all_freq_i < READ_ONCE(task->max_states)) {

		spin_lock_irqsave(&task_time_in_state_lock, flags);
		if (task->time_in_state) {
			atomic64_add(cputime,
				&task->time_in_state[all_freq_i]);
		}
		spin_unlock_irqrestore(&task_time_in_state_lock, flags);
	}

	powerstats = per_cpu(cpufreq_power_stats, cpu_num);
	if (!powerstats)
		goto out;

	cpu_freq_i = atomic_read(&stats->cpu_freq_i);
	if (cpu_freq_i == -1)
		goto out;

	curr = powerstats->curr[cpu_freq_i];
	if (task->cpu_power != ULLONG_MAX)
		task->cpu_power += curr * cputime_to_usecs(cputime);

out:
	spin_unlock_irqrestore(&cpufreq_stats_table_lock, stl_flags);
}
EXPORT_SYMBOL_GPL(acct_update_power);

static ssize_t show_current_in_state(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	ssize_t len = 0;
	unsigned int i, cpu;
	struct cpufreq_power_stats *powerstats;

	spin_lock(&cpufreq_stats_lock);
	for_each_possible_cpu(cpu) {
		powerstats = per_cpu(cpufreq_power_stats, cpu);
		if (!powerstats)
			continue;
		len += scnprintf(buf + len, PAGE_SIZE - len, "CPU%d:", cpu);
		for (i = 0; i < powerstats->state_num; i++)
			len += scnprintf(buf + len, PAGE_SIZE - len,
					"%d=%d ", powerstats->freq_table[i],
					powerstats->curr[i]);
		len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	}
	spin_unlock(&cpufreq_stats_lock);
	return len;
}

static ssize_t show_all_time_in_state(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	ssize_t len = 0;
	unsigned int i, cpu, freq, index;
	struct all_cpufreq_stats *all_stat;
	struct cpufreq_policy *policy;

	len += scnprintf(buf + len, PAGE_SIZE - len, "freq\t\t");
	for_each_possible_cpu(cpu) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "cpu%d\t\t", cpu);
		if (cpu_online(cpu))
			cpufreq_stats_update(cpu);
	}

	if (!all_freq_table)
		goto out;
	for (i = 0; i < all_freq_table->table_size; i++) {
		freq = all_freq_table->freq_table[i];
		len += scnprintf(buf + len, PAGE_SIZE - len, "\n%u\t\t", freq);
		for_each_possible_cpu(cpu) {
			policy = cpufreq_cpu_get(cpu);
			if (policy == NULL)
				continue;
			all_stat = per_cpu(all_cpufreq_stats, policy->cpu);
			index = get_index_all_cpufreq_stat(all_stat, freq);
			if (index != -1) {
				len += scnprintf(buf + len, PAGE_SIZE - len,
					"%lu\t\t", (unsigned long)
					cputime64_to_clock_t(all_stat->time_in_state[index]));
			} else {
				len += scnprintf(buf + len, PAGE_SIZE - len,
						"N/A\t\t");
			}
			cpufreq_cpu_put(policy);
		}
	}

out:
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	return len;
}

#ifdef CONFIG_CPU_FREQ_STAT_DETAILS
static ssize_t show_trans_table(struct cpufreq_policy *policy, char *buf)
{
	ssize_t len = 0;
	int i, j;

	struct cpufreq_stats *stat = per_cpu(cpufreq_stats_table, policy->cpu);
	if (!stat)
		return 0;
	cpufreq_stats_update(stat->cpu);
	len += snprintf(buf + len, PAGE_SIZE - len, "   From  :    To\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "         : ");
	for (i = 0; i < stat->state_num; i++) {
		if (len >= PAGE_SIZE)
			break;
		len += snprintf(buf + len, PAGE_SIZE - len, "%9u ",
				stat->freq_table[i]);
	}
	if (len >= PAGE_SIZE)
		return PAGE_SIZE;

	len += snprintf(buf + len, PAGE_SIZE - len, "\n");

	for (i = 0; i < stat->state_num; i++) {
		if (len >= PAGE_SIZE)
			break;

		len += snprintf(buf + len, PAGE_SIZE - len, "%9u: ",
				stat->freq_table[i]);

		for (j = 0; j < stat->state_num; j++) {
			if (len >= PAGE_SIZE)
				break;
			len += snprintf(buf + len, PAGE_SIZE - len, "%9u ",
					stat->trans_table[i*stat->max_state+j]);
		}
		if (len >= PAGE_SIZE)
			break;
		len += snprintf(buf + len, PAGE_SIZE - len, "\n");
	}
	if (len >= PAGE_SIZE)
		return PAGE_SIZE;
	return len;
}
cpufreq_freq_attr_ro(trans_table);
#endif

cpufreq_freq_attr_ro(total_trans);
cpufreq_freq_attr_ro(time_in_state);

static struct attribute *default_attrs[] = {
	&total_trans.attr,
	&time_in_state.attr,
#ifdef CONFIG_CPU_FREQ_STAT_DETAILS
	&trans_table.attr,
#endif
	NULL
};
static struct attribute_group stats_attr_group = {
	.attrs = default_attrs,
	.name = "stats"
};

static struct kobj_attribute _attr_all_time_in_state = __ATTR(all_time_in_state,
		0444, show_all_time_in_state, NULL);

static struct kobj_attribute _attr_current_in_state = __ATTR(current_in_state,
		0444, show_current_in_state, NULL);

static int freq_table_get_index(struct cpufreq_stats *stat, unsigned int freq)
{
	int index;
	for (index = 0; index < stat->max_state; index++)
		if (stat->freq_table[index] == freq)
			return index;
	return -1;
}

/* should be called late in the CPU removal sequence so that the stats
 * memory is still available in case someone tries to use it.
 */
static void cpufreq_stats_free_table(unsigned int cpu)
{
	struct cpufreq_stats *stat = per_cpu(cpufreq_stats_table, cpu);
	struct cpufreq_stats *prev_stat = per_cpu(prev_cpufreq_stats_table, cpu);
	unsigned int alloc_size;

	if (stat) {
		prev_stat = kzalloc(sizeof(*stat), GFP_KERNEL);
		if (!prev_stat) {
			pr_err("%s: prev_stat kzalloc failed\n", __func__);
			return;
		}

		memcpy(prev_stat, stat, sizeof(*stat));
		per_cpu(prev_cpufreq_stats_table, cpu) = prev_stat;

		alloc_size = stat->max_state * sizeof(int) + stat->max_state * sizeof(u64);
#ifdef CONFIG_CPU_FREQ_STAT_DETAILS
		alloc_size = stat->max_state * stat->max_state * sizeof(int);
#endif
		prev_stat->time_in_state = kzalloc(alloc_size, GFP_KERNEL);
		if (!prev_stat->time_in_state) {
			pr_err("%s: prev_stat time_in_state kzalloc failed\n", __func__);
			kfree(prev_stat);
			return;
		}
		memcpy(prev_stat->time_in_state, stat->time_in_state, alloc_size);

		pr_debug("%s: Free stat table\n", __func__);
		kfree(stat->time_in_state);
		kfree(stat);
		per_cpu(cpufreq_stats_table, cpu) = NULL;
	}
}

/* must be called early in the CPU removal sequence (before
 * cpufreq_remove_dev) so that policy is still valid.
 */
static void cpufreq_stats_free_sysfs(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

	if (!policy)
		return;

	if (!cpufreq_frequency_get_table(cpu))
		goto put_ref;

	if (!policy_is_shared(policy)) {
		pr_debug("%s: Free sysfs stat\n", __func__);
		sysfs_remove_group(&policy->kobj, &stats_attr_group);
	}

put_ref:
	cpufreq_cpu_put(policy);
}

static void cpufreq_allstats_free(void)
{
	int cpu;
	struct all_cpufreq_stats *all_stat;

	sysfs_remove_file(cpufreq_global_kobject,
						&_attr_all_time_in_state.attr);

	for_each_possible_cpu(cpu) {
		all_stat = per_cpu(all_cpufreq_stats, cpu);
		if (!all_stat)
			continue;
		kfree(all_stat->time_in_state);
		kfree(all_stat);
		per_cpu(all_cpufreq_stats, cpu) = NULL;
	}
	if (all_freq_table) {
		kfree(all_freq_table->freq_table);
		kfree(all_freq_table);
		all_freq_table = NULL;
	}
}

static void cpufreq_powerstats_free(void)
{
	int cpu;
	struct cpufreq_power_stats *powerstats;

	sysfs_remove_file(cpufreq_global_kobject, &_attr_current_in_state.attr);

	for_each_possible_cpu(cpu) {
		powerstats = per_cpu(cpufreq_power_stats, cpu);
		if (!powerstats)
			continue;
		kfree(powerstats->curr);
		kfree(powerstats);
		per_cpu(cpufreq_power_stats, cpu) = NULL;
	}
}

static int cpufreq_stats_create_table(struct cpufreq_policy *policy,
	int cpu, struct cpufreq_frequency_table *table, int count)
{
	unsigned int i, j, ret = 0;
	struct cpufreq_stats *stat;
	struct cpufreq_policy *data;
	unsigned int alloc_size;
	struct cpufreq_stats *prev_stat = per_cpu(prev_cpufreq_stats_table, cpu);

	if (per_cpu(cpufreq_stats_table, cpu))
		return -EBUSY;
	stat = kzalloc(sizeof(struct cpufreq_stats), GFP_KERNEL);
	if ((stat) == NULL)
		return -ENOMEM;

	if (prev_stat)
		memcpy(stat, prev_stat, sizeof(*prev_stat));

	data = cpufreq_cpu_get(cpu);
	if (data == NULL) {
		ret = -EINVAL;
		goto error_get_fail;
	}

	ret = sysfs_create_group(&data->kobj, &stats_attr_group);

	stat->cpu = cpu;
	per_cpu(cpufreq_stats_table, cpu) = stat;

	alloc_size = count * sizeof(int) + count * sizeof(u64);

#ifdef CONFIG_CPU_FREQ_STAT_DETAILS
	alloc_size += count * count * sizeof(int);
#endif
	stat->max_state = count;
	stat->time_in_state = kzalloc(alloc_size, GFP_KERNEL);
	if (!stat->time_in_state) {
		ret = -ENOMEM;
		goto error_out;
	}
	stat->freq_table = (unsigned int *)(stat->time_in_state + count);

#ifdef CONFIG_CPU_FREQ_STAT_DETAILS
	stat->trans_table = stat->freq_table + count;
#endif
	j = 0;
	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++) {
		unsigned int freq = table[i].frequency;
		if (freq == CPUFREQ_ENTRY_INVALID)
			continue;
		if (freq_table_get_index(stat, freq) == -1)
			stat->freq_table[j++] = freq;
	}
	stat->state_num = j;

	if (prev_stat) {
		memcpy(stat->time_in_state, prev_stat->time_in_state, alloc_size);
		kfree(prev_stat->time_in_state);
		kfree(prev_stat);
		per_cpu(prev_cpufreq_stats_table, cpu) = NULL;
	}

	spin_lock(&cpufreq_stats_lock);
	stat->last_time = get_jiffies_64();
	atomic_set(&stat->cpu_freq_i, freq_table_get_index(stat, policy->cur));
	spin_unlock(&cpufreq_stats_lock);
	cpufreq_cpu_put(data);
	return 0;
error_out:
	cpufreq_cpu_put(data);
error_get_fail:
	kfree(stat);
	per_cpu(cpufreq_stats_table, cpu) = NULL;
	return ret;
}

static void cpufreq_stats_update_policy_cpu(struct cpufreq_policy *policy)
{
	struct cpufreq_stats *old;
	struct cpufreq_stats *stat;
	unsigned long flags;

	spin_lock_irqsave(&cpufreq_stats_table_lock, flags);
	old = per_cpu(cpufreq_stats_table, policy->cpu);
	stat = per_cpu(cpufreq_stats_table, policy->last_cpu);

	if (old) {
		kfree(old->time_in_state);
		kfree(old);
	}

	pr_debug("Updating stats_table for new_cpu %u from last_cpu %u\n",
			policy->cpu, policy->last_cpu);
	per_cpu(cpufreq_stats_table, policy->cpu) = per_cpu(cpufreq_stats_table,
			policy->last_cpu);
	per_cpu(cpufreq_stats_table, policy->last_cpu) = NULL;
	stat->cpu = policy->cpu;
	spin_unlock_irqrestore(&cpufreq_stats_table_lock, flags);
}

static void cpufreq_powerstats_create(unsigned int cpu,
		struct cpufreq_frequency_table *table, int count) {
	unsigned int alloc_size, i = 0, j = 0, ret = 0;
	struct cpufreq_power_stats *powerstats;
	struct device_node *cpu_node;
	char device_path[16];

	powerstats = kzalloc(sizeof(struct cpufreq_power_stats),
			GFP_KERNEL);
	if (!powerstats)
		return;

	/* Allocate memory for freq table per cpu as well as clockticks per
	 * freq*/
	alloc_size = count * sizeof(unsigned int) +
		count * sizeof(unsigned int);
	powerstats->curr = kzalloc(alloc_size, GFP_KERNEL);
	if (!powerstats->curr) {
		kfree(powerstats);
		return;
	}
	powerstats->freq_table = powerstats->curr + count;

	spin_lock(&cpufreq_stats_lock);
	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END && j < count; i++) {
		unsigned int freq = table[i].frequency;

		if (freq == CPUFREQ_ENTRY_INVALID)
			continue;
		powerstats->freq_table[j++] = freq;
	}
	powerstats->state_num = j;

	snprintf(device_path, sizeof(device_path), "/cpus/cpu@%d", cpu);
	cpu_node = of_find_node_by_path(device_path);
	if (cpu_node) {
		ret = of_property_read_u32_array(cpu_node, "current",
				powerstats->curr, count);
		if (ret) {
			kfree(powerstats->curr);
			kfree(powerstats);
			powerstats = NULL;
		}
	}
	per_cpu(cpufreq_power_stats, cpu) = powerstats;
	spin_unlock(&cpufreq_stats_lock);
}

static int compare_for_sort(const void *lhs_ptr, const void *rhs_ptr)
{
	unsigned int lhs = *(const unsigned int *)(lhs_ptr);
	unsigned int rhs = *(const unsigned int *)(rhs_ptr);
	if (lhs < rhs)
		return -1;
	if (lhs > rhs)
		return 1;
	return 0;
}

static bool check_all_freq_table(unsigned int freq)
{
	int i;
	for (i = 0; i < all_freq_table->table_size; i++) {
		if (freq == all_freq_table->freq_table[i])
			return true;
	}
	return false;
}

static void create_all_freq_table(void)
{
	all_freq_table = kzalloc(sizeof(struct all_freq_table),
			GFP_KERNEL);
	if (!all_freq_table)
		pr_warn("could not allocate memory for all_freq_table\n");
	return;
}

static void free_all_freq_table(void)
{
	if (all_freq_table) {
		if (all_freq_table->freq_table) {
			kfree(all_freq_table->freq_table);
			all_freq_table->freq_table = NULL;
		}
		kfree(all_freq_table);
		all_freq_table = NULL;
	}
}

static void add_all_freq_table(unsigned int freq)
{
	unsigned int size;
	size = sizeof(all_freq_table->freq_table[0]) *
		(all_freq_table->table_size + 1);
	all_freq_table->freq_table = krealloc(all_freq_table->freq_table,
			size, GFP_ATOMIC);
	if (IS_ERR(all_freq_table->freq_table)) {
		pr_warn("Could not reallocate memory for freq_table\n");
		all_freq_table->freq_table = NULL;
		return;
	}
	all_freq_table->freq_table[all_freq_table->table_size++] = freq;
}

static void cpufreq_allstats_create(unsigned int cpu,
		struct cpufreq_frequency_table *table, int count)
{
	int i , j = 0;
	unsigned int alloc_size;
	struct all_cpufreq_stats *all_stat;
	bool sort_needed = false;

	all_stat = kzalloc(sizeof(struct all_cpufreq_stats),
			GFP_KERNEL);
	if (!all_stat) {
		pr_warn("Cannot allocate memory for cpufreq stats\n");
		return;
	}

	/*Allocate memory for freq table per cpu as well as clockticks per freq*/
	alloc_size = count * sizeof(int) + count * sizeof(cputime64_t);
	all_stat->time_in_state = kzalloc(alloc_size, GFP_KERNEL);
	if (!all_stat->time_in_state) {
		pr_warn("Cannot allocate memory for cpufreq time_in_state\n");
		kfree(all_stat);
		all_stat = NULL;
		return;
	}
	all_stat->freq_table = (unsigned int *)
		(all_stat->time_in_state + count);

	spin_lock(&cpufreq_stats_lock);
	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++) {
		unsigned int freq = table[i].frequency;
		if (freq == CPUFREQ_ENTRY_INVALID)
			continue;
		all_stat->freq_table[j++] = freq;
		if (all_freq_table && !check_all_freq_table(freq)) {
			add_all_freq_table(freq);
			sort_needed = true;
		}
	}
	if (sort_needed)
		sort(all_freq_table->freq_table, all_freq_table->table_size,
				sizeof(unsigned int), &compare_for_sort, NULL);
	all_stat->state_num = j;
	per_cpu(all_cpufreq_stats, cpu) = all_stat;
	spin_unlock(&cpufreq_stats_lock);
}

static int cpufreq_stat_notifier_policy(struct notifier_block *nb,
		unsigned long val, void *data)
{
	int ret, count = 0, i;
	struct cpufreq_policy *policy = data;
	struct cpufreq_frequency_table *table;
	unsigned int cpu = policy->cpu;

	if (val == CPUFREQ_UPDATE_POLICY_CPU) {
		cpufreq_stats_update_policy_cpu(policy);
		return 0;
	}

	if (val != CPUFREQ_NOTIFY)
		return 0;
	table = cpufreq_frequency_get_table(cpu);
	if (!table)
		return 0;

	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++) {
		unsigned int freq = table[i].frequency;

		if (freq == CPUFREQ_ENTRY_INVALID)
			continue;
		count++;
	}

	if (!per_cpu(all_cpufreq_stats, cpu))
		cpufreq_allstats_create(cpu, table, count);

	if (!per_cpu(cpufreq_power_stats, cpu))
		cpufreq_powerstats_create(cpu, table, count);

	ret = cpufreq_stats_create_table(policy, cpu, table, count);
	if (ret)
		return ret;
	return 0;
}

static int cpufreq_stat_notifier_trans(struct notifier_block *nb,
		unsigned long val, void *data)
{
	int i;
	struct cpufreq_freqs *freq = data;
	struct cpufreq_stats *stat;
	int cpu_freq_old_i, cpu_freq_new_i;
	int all_freq_old_i, all_freq_new_i;

	if (val != CPUFREQ_POSTCHANGE)
		return 0;

	stat = per_cpu(cpufreq_stats_table, freq->cpu);
	if (!stat)
		return 0;

	cpu_freq_old_i = atomic_read(&stat->cpu_freq_i);
	cpu_freq_new_i = freq_table_get_index(stat, freq->new);

	all_freq_old_i = atomic_read(&stat->all_freq_i);
	for (i = 0; i < all_freq_table->table_size; ++i) {
		if (all_freq_table->freq_table[i] == freq->new)
			break;
	}
	if (i != all_freq_table->table_size)
		all_freq_new_i = i;
	else
		all_freq_new_i = -1;

	/* We can't do stat->time_in_state[-1]= .. */
	if (cpu_freq_old_i == -1 || cpu_freq_new_i == -1)
		return 0;

	if (all_freq_old_i == -1 || all_freq_new_i == -1)
		return 0;

	cpufreq_stats_update(freq->cpu);

	if (cpu_freq_old_i == cpu_freq_new_i)
		return 0;

	if (all_freq_old_i == all_freq_new_i)
		return 0;

	spin_lock(&cpufreq_stats_lock);
	atomic_set(&stat->cpu_freq_i, cpu_freq_new_i);
	atomic_set(&stat->all_freq_i, all_freq_new_i);
#ifdef CONFIG_CPU_FREQ_STAT_DETAILS
	stat->trans_table[cpu_freq_old_i * stat->max_state + cpu_freq_new_i]++;
#endif
	stat->total_trans++;
	spin_unlock(&cpufreq_stats_lock);
	return 0;
}

static int process_notifier(struct notifier_block *self,
			unsigned long cmd, void *v)
{
	struct task_struct *task = v;
	struct uid_entry *uid_entry;
	unsigned long flags;
	uid_t uid;
	int i;

	if (!task)
		return NOTIFY_OK;

	rt_mutex_lock(&uid_lock);

	uid = from_kuid_munged(current_user_ns(), task_uid(task));
	uid_entry = find_or_register_uid(uid);
	if (!uid_entry) {
		rt_mutex_unlock(&uid_lock);
		pr_err("%s: failed to find uid %d\n", __func__, uid);
		return NOTIFY_OK;
	}

	if (uid_entry->dead_max_states < task->max_states) {
		uid_entry->dead_time_in_state = krealloc(
			uid_entry->dead_time_in_state,
			task->max_states *
			sizeof(uid_entry->dead_time_in_state[0]),
			GFP_ATOMIC);
		memset(uid_entry->dead_time_in_state +
			uid_entry->dead_max_states,
			0, (task->max_states - uid_entry->dead_max_states) *
			sizeof(uid_entry->dead_time_in_state[0]));
		uid_entry->dead_max_states = task->max_states;
	}

	spin_lock_irqsave(&task_time_in_state_lock, flags);
	if (task->time_in_state) {
		for (i = 0; i < task->max_states; ++i) {
			uid_entry->dead_time_in_state[i] +=
				atomic_read(&task->time_in_state[i]);
		}
	}
	spin_unlock_irqrestore(&task_time_in_state_lock, flags);

	rt_mutex_unlock(&uid_lock);
	return NOTIFY_OK;
}

static int uid_time_in_state_open(struct inode *inode, struct file *file)
{
	return single_open(file, uid_time_in_state_show, PDE_DATA(inode));
}

static const struct file_operations uid_time_in_state_fops = {
	.open		= uid_time_in_state_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int cpufreq_stats_create_table_cpu(unsigned int cpu)
{
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *table;
	int ret = -ENODEV, i, count = 0;

	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return -ENODEV;

	table = cpufreq_frequency_get_table(cpu);
	if (!table)
		goto out;

	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++) {
		unsigned int freq = table[i].frequency;

		if (freq == CPUFREQ_ENTRY_INVALID)
			continue;
		count++;
	}

	if (!per_cpu(all_cpufreq_stats, cpu))
		cpufreq_allstats_create(cpu, table, count);

	if (!per_cpu(cpufreq_power_stats, cpu))
		cpufreq_powerstats_create(cpu, table, count);

	ret = cpufreq_stats_create_table(policy, cpu, table, count);

out:
	cpufreq_cpu_put(policy);
	return ret;
}

static int __cpuinit cpufreq_stat_cpu_callback(struct notifier_block *nfb,
					       unsigned long action,
					       void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;

	switch (action) {
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		cpufreq_update_policy(cpu);
		break;
	case CPU_DOWN_PREPARE:
	case CPU_DOWN_PREPARE_FROZEN:
		cpufreq_stats_free_sysfs(cpu);
		break;
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
		cpufreq_stats_free_table(cpu);
		break;
	case CPU_DOWN_FAILED:
	case CPU_DOWN_FAILED_FROZEN:
		cpufreq_stats_create_table_cpu(cpu);
		break;
	}
	return NOTIFY_OK;
}

/* priority=1 so this will get called before cpufreq_remove_dev */
static struct notifier_block cpufreq_stat_cpu_notifier __refdata = {
	.notifier_call = cpufreq_stat_cpu_callback,
	.priority = 1,
};

static struct notifier_block notifier_policy_block = {
	.notifier_call = cpufreq_stat_notifier_policy
};

static struct notifier_block notifier_trans_block = {
	.notifier_call = cpufreq_stat_notifier_trans
};

static struct notifier_block process_notifier_block = {
	.notifier_call	= process_notifier,
};

static int cpufreq_stats_setup(void)
{
	int ret;
	unsigned int cpu;

	spin_lock_init(&cpufreq_stats_lock);
	ret = cpufreq_register_notifier(&notifier_policy_block,
				CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		return ret;

	create_all_freq_table();

	register_hotcpu_notifier(&cpufreq_stat_cpu_notifier);
	for_each_online_cpu(cpu)
		cpufreq_update_policy(cpu);

	/* XXX TODO task support for time_in_state doesn't update freq
	 * info for tasks already initialized, so tasks initialized early
	 * (before cpufreq_stat_init is done) do not get time_in_state data
	 * and CPUFREQ_TRANSITION_NOTIFIER does not update freq info for
	 * tasks already created
	 */
	ret = cpufreq_register_notifier(&notifier_trans_block,
				CPUFREQ_TRANSITION_NOTIFIER);
	if (ret) {
		cpufreq_unregister_notifier(&notifier_policy_block,
				CPUFREQ_POLICY_NOTIFIER);
		unregister_hotcpu_notifier(&cpufreq_stat_cpu_notifier);
		for_each_online_cpu(cpu)
			cpufreq_stats_free_table(cpu);
		free_all_freq_table();
		return ret;
	}

	ret = sysfs_create_file(cpufreq_global_kobject,
			&_attr_all_time_in_state.attr);
	if (ret)
		pr_warn("Cannot create sysfs file for cpufreq stats\n");

	ret = sysfs_create_file(cpufreq_global_kobject,
			&_attr_current_in_state.attr);
	if (ret)
		pr_warn("Cannot create sysfs file for cpufreq current stats\n");

	proc_create_data("uid_time_in_state", 0444, NULL,
		&uid_time_in_state_fops, NULL);

	profile_event_register(PROFILE_TASK_EXIT, &process_notifier_block);

	cpufreq_all_freq_init = true;

	return 0;
}

static void cpufreq_stats_cleanup(void)
{
	unsigned int cpu;

	cpufreq_unregister_notifier(&notifier_policy_block,
			CPUFREQ_POLICY_NOTIFIER);
	cpufreq_unregister_notifier(&notifier_trans_block,
			CPUFREQ_TRANSITION_NOTIFIER);
	unregister_hotcpu_notifier(&cpufreq_stat_cpu_notifier);
	for_each_online_cpu(cpu) {
		cpufreq_stats_free_table(cpu);
		cpufreq_stats_free_sysfs(cpu);
	}
	cpufreq_allstats_free();
}

#ifdef CONFIG_BL_SWITCHER
static int cpufreq_stats_switcher_notifier(struct notifier_block *nfb,
					unsigned long action, void *_arg)
{
	switch (action) {
	case BL_NOTIFY_PRE_ENABLE:
	case BL_NOTIFY_PRE_DISABLE:
		cpufreq_stats_cleanup();
		break;

	case BL_NOTIFY_POST_ENABLE:
	case BL_NOTIFY_POST_DISABLE:
		cpufreq_stats_setup();
		break;

	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

static struct notifier_block switcher_notifier = {
	.notifier_call = cpufreq_stats_switcher_notifier,
};
#endif

static int __init cpufreq_stats_init(void)
{
	int ret;
	spin_lock_init(&cpufreq_stats_lock);

	ret = cpufreq_stats_setup();
#ifdef CONFIG_BL_SWITCHER
	if (!ret)
		bL_switcher_register_notifier(&switcher_notifier);
#endif
	return ret;
}

static void __exit cpufreq_stats_exit(void)
{
#ifdef CONFIG_BL_SWITCHER
	bL_switcher_unregister_notifier(&switcher_notifier);
#endif
	cpufreq_stats_cleanup();
	cpufreq_powerstats_free();
}

MODULE_AUTHOR("Zou Nan hai <nanhai.zou@intel.com>");
MODULE_DESCRIPTION("'cpufreq_stats' - A driver to export cpufreq stats "
				"through sysfs filesystem");
MODULE_LICENSE("GPL");

module_init(cpufreq_stats_init);
module_exit(cpufreq_stats_exit);
