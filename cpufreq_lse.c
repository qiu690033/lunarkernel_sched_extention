// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 *
 * This code is part of LunarKernel.
 * cpufreq governor.
 *
 * File name: cpufreq_lse.c
 * Author: Cloud_Yun <1770669041@qq.com>
 * Version: v251003_Dev
 * Date: 2025/10/3 Friday
 */

#include <linux/kmemleak.h>
#include <linux/cpufreq.h>
#include <linux/sched.h>
#include <linux/sched/cputime.h>
#include <uapi/linux/sched/types.h>
#include <trace/hooks/sched.h>

#include "lse_main.h"

/* ── governor-level parms ── */
unsigned int sysctl_lse_gov_debug;
static int cpufreq_gov_debug(void) {return sysctl_lse_gov_debug;}

/* Rate-limit: min interval between tick-driven freq updates per policy (ns) */
#define LSE_GOV_RATE_LIMIT_NS  400000UL  /* 400us ≈ 2-3 ticks at 250Hz */

struct lse_gov_rq {      /* per-rq gov tracking */
    u64  last_tick_ns;   /* last tick-update timestamp (monotonic ns) */
    u64  curr_util;      /* cached util from last tick */
    u64  cached_this_sum;/* cached curr_runnable_sum of this CPU */
    unsigned int cached_agg_util; /* cached cluster agg util */
};
static DEFINE_PER_CPU(struct lse_gov_rq, lse_gov_rq_data);

/*debug level for lse_gov*/
#define DEBUG_SYSTRACE (1 << 0)
#define DEBUG_FTRACE   (1 << 1)
#define DEBUG_KMSG     (1 << 2)

#define lse_gov_debug(fmt, ...) \
	pr_info("[lse_gov][%s] "fmt, __func__, ##__VA_ARGS__)

#define lse_gov_err(fmt, ...) \
	pr_err("[lse_gov][%s] "fmt, __func__, ##__VA_ARGS__)

#define gov_trace_printk(fmt, args...)	\
do {										\
		trace_printk("[lse_gov] "fmt, args);	\
} while (0)

#define DEFAULT_TARGET_LOAD 90

static int gov_flag[MAX_LSE_CLUSTERS] = {0};
int util_norm_enable;
int cluster_window_enable = 1;
int cluster_tl_dyn_enable = 1;
int cluster_freq_cap_enable = 1;
int cluster_agg_mode = 1; /* 0: max, 1: top2_avg */
int gov_legacy_formula_enable;

#define LSE_AGG_MAX 0
#define LSE_AGG_TOP2_AVG 1
#define LSE_DEFAULT_CLUSTER_WINDOW_NS DEFAULT_SCHED_RAVG_WINDOW

struct lse_cluster_gov_cfg {
	unsigned int target_load_base;
	unsigned int target_load_min;
	unsigned int target_load_max;
	unsigned int target_load_step;
	unsigned int window_ns;
	unsigned int freq_cap_max;
	unsigned int freq_cap_min;
};

static struct lse_cluster_gov_cfg lse_cluster_cfg[MAX_LSE_CLUSTERS];

struct lse_gov_tunables {
	struct gov_attr_set		attr_set;
	unsigned int			target_loads;
	int				soft_freq_max;
	int				soft_freq_min;
	bool				apply_freq_immediately;
};

struct lse_gov_policy {
	struct cpufreq_policy	*policy;

	struct lse_gov_tunables	*tunables;
	struct list_head	tunables_hook;

	raw_spinlock_t		update_lock;	/* For shared policies */
	unsigned int		next_freq;
	unsigned int		freq_cached;
	/* The next fields are only needed if fast switch cannot be used: */
	struct kthread_work	work;
	struct mutex		work_lock;
	struct kthread_worker	worker;
	struct task_struct	*thread;
	bool			work_in_progress;
	unsigned int	target_load;
    bool            backup_efficiencies_available;
	int		cluster_id;
	unsigned int	target_load_base;
	unsigned int	target_load_min;
	unsigned int	target_load_max;
	unsigned int	target_load_step;
	unsigned int	cluster_window_ns;
	unsigned int	cluster_freq_cap_max;
	unsigned int	cluster_freq_cap_min;
	unsigned int	last_agg_util;
	unsigned int	last_power_pressure;
	unsigned int	last_target_load_dyn;
	unsigned int	last_freq_cap_applied;
};

struct lse_gov_cpu {
	struct update_util_data update_util;
	unsigned int		reasons;
	struct lse_gov_policy	*lg_policy;
	unsigned int		cpu;

	unsigned long		util;
	unsigned int		flags;
};

static DEFINE_PER_CPU(struct lse_gov_cpu, lse_gov_cpu);
static DEFINE_PER_CPU(struct lse_gov_tunables *, cached_tunables);
static DEFINE_MUTEX(global_tunables_lock);
static struct lse_gov_tunables *global_tunables;

static unsigned int lse_get_policy_window_ns(struct lse_gov_policy *lg_policy)
{
	if (!cluster_window_enable)
		return lse_sched_ravg_window;
	if (!lg_policy->cluster_window_ns)
		return lse_sched_ravg_window;
	return lg_policy->cluster_window_ns;
}

static unsigned int lse_top2_avg_util(struct cpumask *mask)
{
	unsigned int top1 = 0, top2 = 0, util;
	unsigned int cnt = 0;
	int cpu;
	struct lse_rq *lrq;
	unsigned int win_ns;

	for_each_cpu(cpu, mask) {
		lrq = &per_cpu(lse_rq, cpu);
		win_ns = lrq->prev_window_size ? lrq->prev_window_size : lse_sched_ravg_window;
		if (!win_ns)
			continue;
		util = (unsigned int)min_t(u64,
			div64_u64(lrq->prev_runnable_sum << SCHED_CAPACITY_SHIFT, win_ns),
			SCHED_CAPACITY_SCALE);
		/*
		 * lrq->prev_runnable_sum is already frequency/capacity scaled by
		 * update_task_rq_cpu_cycles(), so avoid double normalization here.
		 */
		if (util >= top1) {
			top2 = top1;
			top1 = util;
		} else if (util > top2) {
			top2 = util;
		}
		cnt++;
	}

	if (!cnt)
		return 0;
	if (cnt == 1)
		return top1;
	return (top1 + top2) / 2;
}

static unsigned int lse_compute_power_pressure(struct lse_gov_policy *lg_policy,
					       unsigned int agg_util)
{
	struct cpufreq_policy *policy = lg_policy->policy;
	unsigned int cur = policy->cur;
	unsigned int max = policy->cpuinfo.max_freq;
	unsigned int pressure = 0;

	if (max)
		pressure = mult_frac(cur, 1024, max);
	pressure = (pressure + agg_util) / 2;
	return min(pressure, 1024U);
}

static unsigned int lse_dynamic_target_load(struct lse_gov_policy *lg_policy,
					    unsigned int agg_util)
{
	unsigned int tl;
	unsigned int pressure;

	tl = lg_policy->target_load_base ? lg_policy->target_load_base : DEFAULT_TARGET_LOAD;
	tl = clamp_t(unsigned int, tl, lg_policy->target_load_min, lg_policy->target_load_max);
	if (!cluster_tl_dyn_enable)
		return tl;

	pressure = lse_compute_power_pressure(lg_policy, agg_util);
	if (pressure > 850 && tl + lg_policy->target_load_step <= lg_policy->target_load_max)
		tl += lg_policy->target_load_step;
	else if (pressure < 450 && tl > lg_policy->target_load_min + lg_policy->target_load_step)
		tl -= lg_policy->target_load_step;

	lg_policy->last_power_pressure = pressure;
	return clamp_t(unsigned int, tl, lg_policy->target_load_min, lg_policy->target_load_max);
}

/* ──── tick-level cluster util (cached-incremental, avoids full traversal) ──── */

/*
 * Compute cluster utility from prev_runnable_sum (completed-window aggregate).
 * Using prev (not curr) eliminates intra-window volatility that causes
 * frequency overshoot → undershoot cycles — a major energy waste pattern.
 *
 * Standard WALT + schedutil convention: prev_runnable_sum = stable average,
 * curr_runnable_sum = in-progress partial (used only for rate-of-change hints).
 */
#define UTIL_CACHE_STALE_NS  16000000ULL  /* 16ms — two windows before forced recal */

static unsigned int lse_gov_cluster_util(struct cpufreq_policy *policy, int this_cpu)
{
	struct lse_gov_rq *grq = &per_cpu(lse_gov_rq_data, this_cpu);
	struct lse_rq *lrq = &per_cpu(lse_rq, this_cpu);
	u64 this_sum = lrq->prev_runnable_sum;
	u64 now = local_clock();

	/*
	 * Incremental path: if this CPU's sum barely changed since last tick
	 * and the cache isn't stale, reuse cached cluster util.
	 */
	if (grq->cached_agg_util && grq->cached_this_sum &&
	    (now - grq->last_tick_ns < UTIL_CACHE_STALE_NS)) {
		u64 diff = this_sum > grq->cached_this_sum
			   ? this_sum - grq->cached_this_sum
			   : grq->cached_this_sum - this_sum;

		/* less than 5% change — ignore, reuse cache */
		if (diff * 20 < grq->cached_this_sum) {
			grq->cached_this_sum = this_sum;
			return grq->cached_agg_util;
		}
	}

	/* Full traversal: this CPU changed significantly or cache is stale */
	{
		u64 max_sum = 0, sum;
		int cpu;

		if (cluster_agg_mode == LSE_AGG_TOP2_AVG) {
			u64 top1 = 0, top2 = 0;
			for_each_cpu(cpu, policy->cpus) {
				lrq = &per_cpu(lse_rq, cpu);
				sum = lrq->prev_runnable_sum;
				if (sum >= top1)  { top2 = top1; top1 = sum; }
				else if (sum > top2) top2 = sum;
			}
			max_sum = (top1 + top2) / 2;
		} else {
			for_each_cpu(cpu, policy->cpus) {
				lrq = &per_cpu(lse_rq, cpu);
				sum = lrq->prev_runnable_sum;
				if (sum > max_sum) max_sum = sum;
			}
		}

		grq->cached_this_sum = this_sum;
		grq->cached_agg_util = (unsigned int)min_t(u64,
			div64_u64(max_sum << SCHED_CAPACITY_SHIFT,
				  max_t(unsigned int, lse_sched_ravg_window, 1U)),
			SCHED_CAPACITY_SCALE);
		return grq->cached_agg_util;
	}
}

static unsigned int soft_freq_clamp(struct lse_gov_policy *lg_policy, unsigned int target_freq);

/**
 * lse_gov_tick_update — tick-level frequency update entry (called from scheduler tick)
 * @rq:  runqueue of the current CPU
 *
 * Computes cluster util from curr_runnable_sum (real-time, not prev-window),
 * integrates DSQ urgency as a boost factor, and applies rate-limiting to
 * avoid excessive updates.
 */
void lse_gov_tick_update(struct rq *rq)
{
	struct cpufreq_policy *policy;
	struct lse_gov_policy *lg_policy;
	struct lse_gov_rq *grq;
	unsigned int agg_util, cluster_tl, next_f, dsq_boost;
	u64 now_ns, scaled_freq;
	unsigned long irq_flags;
	int cpu = cpu_of(rq);

	policy = cpufreq_cpu_get(cpu);
	if (!policy || !policy->governor_data) {
		if (policy) cpufreq_cpu_put(policy);
		return;
	}
	lg_policy = policy->governor_data;

	/* ── rate-limit ── */
	grq = &per_cpu(lse_gov_rq_data, cpu);
	now_ns = local_clock();
	if (now_ns - grq->last_tick_ns < LSE_GOV_RATE_LIMIT_NS) {
		cpufreq_cpu_put(policy);
		return;
	}
	grq->last_tick_ns = now_ns;

	/* ── compute util from prev_runnable_sum (stable, completed-window) ── */
	agg_util = lse_gov_cluster_util(policy, cpu);
	grq->curr_util = agg_util;

	/* ── DSQ urgency boost (0-1024 → 1.0×-2.0×) ── */
	dsq_boost = lse_dsq_urgency_signal(cpu);
	agg_util = min_t(unsigned int, agg_util * (1024 + dsq_boost) / 1024,
			 SCHED_CAPACITY_SCALE);

	/* ── dynamic target_load ── */
	cluster_tl = lg_policy->target_load_base ? lg_policy->target_load_base
						 : DEFAULT_TARGET_LOAD;
	cluster_tl = clamp_t(unsigned int, cluster_tl,
			     lg_policy->target_load_min,
			     lg_policy->target_load_max);
	if (cluster_tl_dyn_enable)
		cluster_tl = lse_dynamic_target_load(lg_policy, agg_util);

	/* ── next_freq = cur × agg_util / target_load ── */
	scaled_freq = div64_u64((u64)policy->cur * agg_util,
				max_t(unsigned int, cluster_tl, 1U));
	next_f = (unsigned int)scaled_freq;
	if (!next_f) next_f = policy->cpuinfo.min_freq;

	next_f = soft_freq_clamp(lg_policy, next_f);
	next_f = cpufreq_driver_resolve_freq(policy, next_f);

	raw_spin_lock_irqsave(&lg_policy->update_lock, irq_flags);
	lg_policy->last_agg_util = agg_util;
	lg_policy->last_target_load_dyn = cluster_tl;
	if (lg_policy->next_freq == next_f) {
		raw_spin_unlock_irqrestore(&lg_policy->update_lock, irq_flags);
		cpufreq_cpu_put(policy);
		return;
	}
	lg_policy->next_freq = next_f;
	if (policy->fast_switch_enabled)
		cpufreq_driver_fast_switch(policy, next_f);
	else
		kthread_queue_work(&lg_policy->worker, &lg_policy->work);
	raw_spin_unlock_irqrestore(&lg_policy->update_lock, irq_flags);

	cpufreq_cpu_put(policy);
}

static void lse_gov_work(struct kthread_work *work)
{
	struct lse_gov_policy *lg_policy = container_of(work, struct lse_gov_policy, work);
	unsigned int freq;
	unsigned long flags;

	/*
	 * Hold lg_policy->update_lock shortly to handle the case where:
	 * incase lg_policy->next_freq is read here, and then updated by
	 * lse_gov_deferred_update() just before work_in_progress is set to false
	 * here, we may miss queueing the new update.
	 *
	 * Note: If a work was queued after the update_lock is released,
	 * lse_gov_work() will just be called again by kthread_work code; and the
	 * request will be proceed before the lse_gov thread sleeps.
	 */
	raw_spin_lock_irqsave(&lg_policy->update_lock, flags);
	freq = lg_policy->next_freq;
	raw_spin_unlock_irqrestore(&lg_policy->update_lock, flags);

	mutex_lock(&lg_policy->work_lock);
	__cpufreq_driver_target(lg_policy->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&lg_policy->work_lock);
}

/* next_freq = cur × agg_util / target_load — uses current freq as base for smooth scaling */
static unsigned int get_next_freq(struct lse_gov_policy *lg_policy, u64 prev_runnable_sum)
{
	struct cpufreq_policy *policy = lg_policy->policy;
	unsigned int freq = policy->cur ? policy->cur : policy->cpuinfo.min_freq;
	unsigned int next_f;
	unsigned int cluster_tl, window_ns, agg_util;
	u64 scaled_freq;
	int cpu = cpumask_first(policy->cpus);
	unsigned long cap = arch_scale_cpu_capacity(cpu);

	if (!cap)
		cap = 1;
	cluster_tl = DEFAULT_TARGET_LOAD;
	if (lg_policy->tunables) {
		cluster_tl = lg_policy->tunables->target_loads;
	}
	if (lg_policy->target_load_base)
		cluster_tl = lg_policy->target_load_base;
	window_ns = lse_get_policy_window_ns(lg_policy);
	agg_util = (unsigned int)min_t(u64,
		div64_u64(prev_runnable_sum << SCHED_CAPACITY_SHIFT, max_t(unsigned int, window_ns, 1U)),
		SCHED_CAPACITY_SCALE);
	/*
	 * prev_runnable_sum has already been scaled in WALT accounting path.
	 * Re-normalizing by cpu capacity over-amplifies little cores at low load.
	 */
	cluster_tl = lse_dynamic_target_load(lg_policy, agg_util);
	lg_policy->last_target_load_dyn = cluster_tl;
	lg_policy->last_agg_util = agg_util;

	if (gov_legacy_formula_enable) {
		unsigned int window_size_tl;
		u64 divisor;

		window_size_tl = mult_frac(lse_sched_ravg_window, cluster_tl, 100);
		divisor = DIV64_U64_ROUNDUP(window_size_tl * cap, freq);
		next_f = DIV64_U64_ROUNDUP(prev_runnable_sum << SCHED_CAPACITY_SHIFT, divisor);
		return next_f;
	}

	scaled_freq = div64_u64((u64)freq * agg_util, max_t(unsigned int, cluster_tl, 1U));
	next_f = (unsigned int)scaled_freq;
	if (!next_f)
		next_f = policy->cpuinfo.min_freq;
	if (cpufreq_gov_debug() & DEBUG_FTRACE)
		gov_trace_printk("cluster[%d] cur[%d] max[%d] win_ns[%u] tl[%u] util[%u] next_f[%d]\n",
			cpu, freq, policy->cpuinfo.max_freq,
			window_ns, cluster_tl, agg_util, next_f);
	return next_f;
}

static unsigned int soft_freq_clamp(struct lse_gov_policy *lg_policy, unsigned int target_freq)
{
	struct cpufreq_policy *policy = lg_policy->policy;
	int soft_freq_max = lg_policy->tunables->soft_freq_max;
	int soft_freq_min = lg_policy->tunables->soft_freq_min;

	if (soft_freq_min >= 0 && soft_freq_min > target_freq) {
		target_freq = soft_freq_min;
	}
	if (soft_freq_max >= 0 && soft_freq_max < target_freq) {
		target_freq = soft_freq_max;
	}
	if (cluster_freq_cap_enable) {
		if (lg_policy->cluster_freq_cap_max &&
		    target_freq > lg_policy->cluster_freq_cap_max)
			target_freq = lg_policy->cluster_freq_cap_max;
		if (lg_policy->cluster_freq_cap_min &&
		    target_freq < lg_policy->cluster_freq_cap_min)
			target_freq = lg_policy->cluster_freq_cap_min;
	}
	lg_policy->last_freq_cap_applied = target_freq;

	if (cpufreq_gov_debug() & DEBUG_FTRACE)
		gov_trace_printk("cluster[%d] max_freq[%d] min_freq[%d] freq[%d]\n",
			policy->cpu, soft_freq_max, soft_freq_min, target_freq);

	return target_freq;
}

void lse_gov_update_cpufreq(struct cpufreq_policy *policy, u64 prev_runnable_sum)
{
	unsigned int next_f;
	struct lse_gov_policy *lg_policy = policy->governor_data;
	unsigned long irq_flags;

	raw_spin_lock_irqsave(&lg_policy->update_lock, irq_flags);

	next_f = get_next_freq(lg_policy, prev_runnable_sum);
	next_f = soft_freq_clamp(lg_policy, next_f);
	next_f = cpufreq_driver_resolve_freq(policy, next_f);
	lg_policy->freq_cached = lg_policy->next_freq ? lg_policy->next_freq : next_f;
	if (lg_policy->next_freq == next_f)
		goto unlock;
	lg_policy->next_freq = next_f;
	if (cpufreq_gov_debug() & DEBUG_FTRACE)
		gov_trace_printk("cluster[%d] freq[%d] fast[%d]\n", policy->cpu, next_f, policy->fast_switch_enabled);
	if (policy->fast_switch_enabled)
		cpufreq_driver_fast_switch(policy, next_f);
	else
		kthread_queue_work(&lg_policy->worker, &lg_policy->work);

unlock:
	raw_spin_unlock_irqrestore(&lg_policy->update_lock, irq_flags);
}

void lse_gov_update_soft_limit_cpufreq(struct lse_gov_policy *lg_policy)
{
	unsigned int next_f;
	struct cpufreq_policy *policy = lg_policy->policy;
	unsigned long irq_flags;

	raw_spin_lock_irqsave(&lg_policy->update_lock, irq_flags);

	next_f = soft_freq_clamp(lg_policy, lg_policy->next_freq);
	next_f = cpufreq_driver_resolve_freq(policy, next_f);
	if (lg_policy->next_freq == next_f)
		goto unlock;
	lg_policy->next_freq = next_f;
	if (cpufreq_gov_debug() & DEBUG_FTRACE)
		gov_trace_printk("cluster[%d] freq[%d] fast[%d]\n",
			policy->cpu, next_f, policy->fast_switch_enabled);
	if (policy->fast_switch_enabled)
		cpufreq_driver_fast_switch(policy, next_f);
	else
		kthread_queue_work(&lg_policy->worker, &lg_policy->work);

unlock:
	raw_spin_unlock_irqrestore(&lg_policy->update_lock, irq_flags);
}

/************************** sysfs interface ************************/
static inline struct lse_gov_tunables *to_lse_gov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct lse_gov_tunables, attr_set);
}

static DEFINE_MUTEX(min_rate_lock);


static ssize_t target_loads_show(struct gov_attr_set *attr_set, char *buf)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	return sprintf(buf, "%d\n", tunables->target_loads);
}

static ssize_t target_loads_store(struct gov_attr_set *attr_set, const char *buf,
					size_t count)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	unsigned int new_target_loads = DEFAULT_TARGET_LOAD;

	if (kstrtouint(buf, 10, &new_target_loads))
		return -EINVAL;

	tunables->target_loads = new_target_loads;
	return count;
}

static ssize_t soft_freq_max_show(struct gov_attr_set *attr_set, char *buf)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	int soft_freq_max = tunables->soft_freq_max;

	if (soft_freq_max < 0) {
		return sprintf(buf, "max\n");
	} else {
		return sprintf(buf, "%d\n", soft_freq_max);
	}
}

static ssize_t soft_freq_max_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	struct lse_gov_policy *lg_policy = list_first_entry(&attr_set->policy_list, struct lse_gov_policy, tunables_hook);
	int new_soft_freq_max = -1;

	if (kstrtoint(buf, 10, &new_soft_freq_max))
		return -EINVAL;

	if (tunables->soft_freq_max == new_soft_freq_max) {
		return count;
	}

	tunables->soft_freq_max = new_soft_freq_max;
	if (tunables->apply_freq_immediately) {
		lse_gov_update_soft_limit_cpufreq(lg_policy);
	}

	return count;
}

static ssize_t soft_freq_min_show(struct gov_attr_set *attr_set, char *buf)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	int soft_freq_min = tunables->soft_freq_min;

	if (soft_freq_min < 0) {
		return sprintf(buf, "0\n");
	} else {
		return sprintf(buf, "%d\n", soft_freq_min);
	}
}

static ssize_t soft_freq_min_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	struct lse_gov_policy *lg_policy = list_first_entry(&attr_set->policy_list, struct lse_gov_policy, tunables_hook);
	int new_soft_freq_min = -1;

	if (kstrtoint(buf, 10, &new_soft_freq_min))
		return -EINVAL;

	if (tunables->soft_freq_min == new_soft_freq_min) {
		return count;
	}

	tunables->soft_freq_min = new_soft_freq_min;
	if (tunables->apply_freq_immediately) {
		lse_gov_update_soft_limit_cpufreq(lg_policy);
	}

	return count;
}

static ssize_t soft_freq_cur_show(struct gov_attr_set *attr_set __maybe_unused, char *buf)
{
	return sprintf(buf, "none\n");
}

static ssize_t soft_freq_cur_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	struct lse_gov_policy *lg_policy = list_first_entry(&attr_set->policy_list, struct lse_gov_policy, tunables_hook);
	int new_soft_freq_cur = -1;

	if (kstrtoint(buf, 10, &new_soft_freq_cur))
		return -EINVAL;

	if (tunables->soft_freq_max == new_soft_freq_cur && tunables->soft_freq_min == new_soft_freq_cur) {
		return count;
	}

	tunables->soft_freq_max = new_soft_freq_cur;
	tunables->soft_freq_min = new_soft_freq_cur;
	if (tunables->apply_freq_immediately) {
		lse_gov_update_soft_limit_cpufreq(lg_policy);
	}

	return count;
}

static ssize_t apply_freq_immediately_show(struct gov_attr_set *attr_set, char *buf)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	return sprintf(buf, "%d\n", (int)tunables->apply_freq_immediately);
}

static ssize_t apply_freq_immediately_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	int new_apply_freq_immediately = 0;

	if (kstrtoint(buf, 10, &new_apply_freq_immediately))
		return -EINVAL;

	tunables->apply_freq_immediately = new_apply_freq_immediately > 0;
	return count;
}

static struct governor_attr target_loads =
	__ATTR(target_loads, 0664, target_loads_show, target_loads_store);

static struct governor_attr soft_freq_max =
	__ATTR(soft_freq_max, 0664, soft_freq_max_show, soft_freq_max_store);

static struct governor_attr soft_freq_min =
	__ATTR(soft_freq_min, 0664, soft_freq_min_show, soft_freq_min_store);

static struct governor_attr soft_freq_cur =
	__ATTR(soft_freq_cur, 0664, soft_freq_cur_show, soft_freq_cur_store);

static struct governor_attr apply_freq_immediately =
	__ATTR(apply_freq_immediately, 0664, apply_freq_immediately_show, apply_freq_immediately_store);

static struct attribute *lse_gov_attrs[] = {
	&target_loads.attr,
	&soft_freq_max.attr,
	&soft_freq_min.attr,
	&soft_freq_cur.attr,
	&apply_freq_immediately.attr,
	NULL
};
ATTRIBUTE_GROUPS(lse_gov);

static struct kobj_type lse_gov_tunables_ktype = {
	.default_groups = lse_gov_groups,
	.sysfs_ops = &governor_sysfs_ops,
};

/********************** cpufreq governor interface *********************/

struct cpufreq_governor cpufreq_lse_gov;

static struct lse_gov_policy *lse_gov_policy_alloc(struct cpufreq_policy *policy)
{
	struct lse_gov_policy *lg_policy;

	lg_policy = kzalloc(sizeof(*lg_policy), GFP_KERNEL);
	if (!lg_policy)
		return NULL;

	lg_policy->policy = policy;
	raw_spin_lock_init(&lg_policy->update_lock);
	return lg_policy;
}

static inline void lse_gov_cpu_reset(struct lse_gov_policy *lg_policy)
{
	unsigned int cpu;

	for_each_cpu(cpu, lg_policy->policy->cpus) {
		struct lse_gov_cpu *lg_cpu = &per_cpu(lse_gov_cpu, cpu);

		lg_cpu->lg_policy = NULL;
	}
}

static void lse_gov_policy_free(struct lse_gov_policy *lg_policy)
{
	kfree(lg_policy);
}

static void lsegov_update_freq(struct update_util_data *cb, u64 time, unsigned int flags)
{
	struct lse_sched_cluster *cluster;
	struct cpufreq_policy *policy;
	struct lse_rq *lrq;
	int cpu;

	if (flags & LSE_CPUFREQ_WINDOW_ROLLOVER) {
		for_each_lse_cluster(cluster) {
			cpumask_t cluster_online_cpus;
			u64 prev_runnable_sum = 0;
			if (gov_flag[cluster->id] == 0)
				continue;
			cpumask_and(&cluster_online_cpus, &cluster->cpus, cpu_online_mask);
			if (cluster_agg_mode == LSE_AGG_TOP2_AVG) {
				unsigned int agg_util = lse_top2_avg_util(&cluster_online_cpus);
				struct cpufreq_policy *tmp_policy;
				unsigned int window_ns = lse_sched_ravg_window;
				struct lse_gov_policy *tmp_lg;

				tmp_policy = cpufreq_cpu_get_raw(cpumask_first(&cluster_online_cpus));
				if (tmp_policy && tmp_policy->governor_data) {
					tmp_lg = tmp_policy->governor_data;
					window_ns = lse_get_policy_window_ns(tmp_lg);
				}
				prev_runnable_sum = div64_u64((u64)agg_util * window_ns,
							      SCHED_CAPACITY_SCALE);
			} else {
				for_each_cpu(cpu, &cluster_online_cpus) {
					lrq = &per_cpu(lse_rq, cpu);
					if (cpufreq_gov_debug() & DEBUG_FTRACE)
						gov_trace_printk("cpu[%d] prev_runnable_sum[%llu]\n",
								 cpu, lrq->prev_runnable_sum);
					prev_runnable_sum = max(prev_runnable_sum, lrq->prev_runnable_sum);
				}
			}

			policy = cpufreq_cpu_get_raw(cpumask_first(&cluster_online_cpus));
			if (policy == NULL)
				lse_gov_err("NULL policy [%d]\n", cpumask_first(&cluster_online_cpus));
			lse_gov_update_cpufreq(policy, prev_runnable_sum);
		}
	}
}

static int lse_gov_kthread_create(struct lse_gov_policy *lg_policy)
{
	struct task_struct *thread;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };
	struct cpufreq_policy *policy = lg_policy->policy;
	int ret;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&lg_policy->work, lse_gov_work);
	kthread_init_worker(&lg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &lg_policy->worker,
				"lse_gov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create lse_gov thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_FIFO\n", __func__);
		return ret;
	}

	lg_policy->thread = thread;
	kthread_bind_mask(thread, policy->related_cpus);
	mutex_init(&lg_policy->work_lock);

	wake_up_process(thread);

	return 0;
}

static void lse_gov_kthread_stop(struct lse_gov_policy *lg_policy)
{
	/* kthread only required for slow path */
	if (lg_policy->policy->fast_switch_enabled)
		return;

	kthread_flush_worker(&lg_policy->worker);
	kthread_stop(lg_policy->thread);
	mutex_destroy(&lg_policy->work_lock);
}

static struct lse_gov_tunables *lse_gov_tunables_alloc(struct lse_gov_policy *lg_policy)
{
	struct lse_gov_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &lg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

static void lse_gov_tunables_free(struct lse_gov_tunables *tunables)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;

	kfree(tunables);
}

#define DEFAULT_HISPEED_LOAD 90
static void lse_gov_tunables_save(struct cpufreq_policy *policy,
		struct lse_gov_tunables *tunables)
{
	int cpu;
	struct lse_gov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!cached) {
		cached = kzalloc(sizeof(*tunables), GFP_KERNEL);
		if (!cached)
			return;

		for_each_cpu(cpu, policy->related_cpus)
			per_cpu(cached_tunables, cpu) = cached;
	}
}

static int lse_gov_init(struct cpufreq_policy *policy)
{
	struct lse_gov_policy *lg_policy;
	struct lse_gov_tunables *tunables;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	lg_policy = lse_gov_policy_alloc(policy);
	if (!lg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = lse_gov_kthread_create(lg_policy);
	if (ret)
		goto free_lg_policy;

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = lg_policy;
		lg_policy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set, &lg_policy->tunables_hook);
		goto out;
	}

	tunables = lse_gov_tunables_alloc(lg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	tunables->target_loads = DEFAULT_TARGET_LOAD;
	tunables->soft_freq_max = -1;
	tunables->soft_freq_min = -1;
	tunables->apply_freq_immediately = true;

	policy->governor_data = lg_policy;
	lg_policy->tunables = tunables;

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &lse_gov_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   cpufreq_lse_gov.name);
	if (ret)
		goto fail;

	policy->dvfs_possible_from_any_cpu = 1;

out:
	mutex_unlock(&global_tunables_lock);
	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	lse_gov_tunables_free(tunables);

stop_kthread:
	lse_gov_kthread_stop(lg_policy);
	mutex_unlock(&global_tunables_lock);

free_lg_policy:
	lse_gov_policy_free(lg_policy);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void lse_gov_exit(struct cpufreq_policy *policy)
{
	struct lse_gov_policy *lg_policy = policy->governor_data;
	struct lse_gov_tunables *tunables = lg_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &lg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count) {
		lse_gov_tunables_save(policy, tunables);
		lse_gov_tunables_free(tunables);
	}

	mutex_unlock(&global_tunables_lock);

	lse_gov_kthread_stop(lg_policy);
	lse_gov_cpu_reset(lg_policy);
	lse_gov_policy_free(lg_policy);
	cpufreq_disable_fast_switch(policy);
}

static int lse_gov_start(struct cpufreq_policy *policy)
{
	struct lse_gov_policy *lg_policy = policy->governor_data;
	unsigned int cpu, cluster_id;

	lg_policy->next_freq = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct lse_gov_cpu *lg_cpu = &per_cpu(lse_gov_cpu, cpu);

		memset(lg_cpu, 0, sizeof(*lg_cpu));
		lg_cpu->cpu			= cpu;
		lg_cpu->lg_policy		= lg_policy;
		cpufreq_add_update_util_hook(cpu, &lg_cpu->update_util, lsegov_update_freq);
	}
	cpu = cpumask_first(policy->related_cpus);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	cluster_id = topology_cluster_id(cpu);
#else
    cluster_id = topology_physical_package_id(cpu);
#endif
	lse_gov_debug("start cluster[%d] cluster_id[%d] gov\n", cpu, cluster_id);
	lg_policy->cluster_id = cluster_id;
	lg_policy->target_load_base = DEFAULT_TARGET_LOAD;
	lg_policy->target_load_min = 70;
	lg_policy->target_load_max = 95;
	lg_policy->target_load_step = 2;
	lg_policy->cluster_window_ns = lse_sched_ravg_window;
	lg_policy->cluster_freq_cap_max = policy->cpuinfo.max_freq;
	lg_policy->cluster_freq_cap_min = policy->cpuinfo.min_freq;
	if (cluster_id >= 0 && cluster_id < MAX_LSE_CLUSTERS) {
		lse_cluster_cfg[cluster_id].target_load_base = lg_policy->target_load_base;
		lse_cluster_cfg[cluster_id].target_load_min = lg_policy->target_load_min;
		lse_cluster_cfg[cluster_id].target_load_max = lg_policy->target_load_max;
		lse_cluster_cfg[cluster_id].target_load_step = lg_policy->target_load_step;
		lse_cluster_cfg[cluster_id].window_ns = lg_policy->cluster_window_ns;
		lse_cluster_cfg[cluster_id].freq_cap_max = lg_policy->cluster_freq_cap_max;
		lse_cluster_cfg[cluster_id].freq_cap_min = lg_policy->cluster_freq_cap_min;
	}

	/*
	 * Some kernel branches (e.g. android12-5.10) don't expose
	 * cpufreq_policy::efficiencies_available. Keep governor start path
	 * branch-agnostic by skipping direct access to that field.
	 */

	if (cluster_id < MAX_LSE_CLUSTERS)
		gov_flag[cluster_id] = 1;

	return 0;
}

static void lse_gov_stop(struct cpufreq_policy *policy)
{
	struct lse_gov_policy *lg_policy = policy->governor_data;
	unsigned int cpu, cluster_id;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	if (!policy->fast_switch_enabled)
		kthread_cancel_work_sync(&lg_policy->work);

	cpu = cpumask_first(policy->related_cpus);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	cluster_id = topology_cluster_id(cpu);
#else
    cluster_id = topology_physical_package_id(cpu);
#endif
	if (cluster_id < MAX_LSE_CLUSTERS)
		gov_flag[cluster_id] = 0;
	synchronize_rcu();
}

static void lse_gov_limits(struct cpufreq_policy *policy)
{
	struct lse_gov_policy *lg_policy = policy->governor_data;
	unsigned long flags, now;
	unsigned int freq, final_freq;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&lg_policy->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&lg_policy->work_lock);
	} else {
		raw_spin_lock_irqsave(&lg_policy->update_lock, flags);

		freq = lg_policy->next_freq;
		/*
		 * we have serval resources to update freq
		 * (1) scheduler to run callback
		 * (2) cpufreq_set_policy to call governor->limtis here
		 * so we have serveral times here and we must to keep them same
		 * here we using walt_sched_clock() to keep same with walt scheduler
		 */
		now = ktime_get_ns();

		/*
		 * cpufreq_driver_resolve_freq() has a clamp, so we do not need
		 * to do any sort of additional validation here.
		 */
		final_freq = cpufreq_driver_resolve_freq(policy, freq);
		cpufreq_driver_fast_switch(policy, final_freq);

		raw_spin_unlock_irqrestore(&lg_policy->update_lock, flags);
	}
}

struct cpufreq_governor cpufreq_lse_gov = {
	.name			= "lunar_ext_gov",
	.owner			= THIS_MODULE,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	.flags			= CPUFREQ_GOV_DYNAMIC_SWITCHING,
#else
    .dynamic_switching	= true,
#endif
	.init			= lse_gov_init,
	.exit			= lse_gov_exit,
	.start			= lse_gov_start,
	.stop			= lse_gov_stop,
	.limits			= lse_gov_limits,
};

int lse_cpufreq_init(void)
{
	int ret = 0;
	struct lse_sched_cluster *cluster = NULL;

	ret = cpufreq_register_governor(&cpufreq_lse_gov);
	if (ret)
		return ret;

	for_each_lse_cluster(cluster)
		lse_gov_debug("num_cluster=%d id=%d cpumask=%*pbl capacity=%lu num_cpus=%d\n",
			lse_num_sched_clusters, cluster->id, cpumask_pr_args(&cluster->cpus),
			arch_scale_cpu_capacity(cpumask_first(&cluster->cpus)),
			num_possible_cpus());

	return ret;
}

static struct lse_gov_policy *lse_gov_policy_from_cpu(int cpu)
{
	struct lse_gov_cpu *lg_cpu;

	if (cpu < 0 || cpu >= nr_cpu_ids)
		return NULL;
	lg_cpu = &per_cpu(lse_gov_cpu, cpu);
	return lg_cpu->lg_policy;
}

unsigned int lse_gov_cluster_window_ns(int cpu)
{
	struct lse_gov_policy *lg = lse_gov_policy_from_cpu(cpu);

	if (!lg)
		return lse_sched_ravg_window;
	return lg->cluster_window_ns;
}

unsigned int lse_gov_cluster_target_load_dyn(int cpu)
{
	struct lse_gov_policy *lg = lse_gov_policy_from_cpu(cpu);

	if (!lg)
		return DEFAULT_TARGET_LOAD;
	return lg->last_target_load_dyn ? lg->last_target_load_dyn : lg->target_load_base;
}

unsigned int lse_gov_cluster_agg_util(int cpu)
{
	struct lse_gov_policy *lg = lse_gov_policy_from_cpu(cpu);

	if (!lg)
		return 0;
	return lg->last_agg_util;
}

unsigned int lse_gov_cluster_freq_cap_applied(int cpu)
{
	struct lse_gov_policy *lg = lse_gov_policy_from_cpu(cpu);

	if (!lg)
		return 0;
	return lg->last_freq_cap_applied;
}

unsigned int lse_gov_cluster_power_pressure(int cpu)
{
	struct lse_gov_policy *lg = lse_gov_policy_from_cpu(cpu);

	if (!lg)
		return 0;
	return lg->last_power_pressure;
}
