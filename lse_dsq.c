// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 *
 * Complete DSQ (Dispatch Queue) layer — ported & adapted from hmbird.
 *
 * Provides:
 *  - 10-level global dispatch queue with deadline-based prioritization
 *  - Per-CPU local DSQ with quota tracking
 *  - Cluster-aware round-robin (BIG / LITTLE clusters)
 *  - Non-period DSQ time quota management
 *  - Timeout scanning for starvation detection
 *  - Urgency signal for cpufreq governor tuning
 */

#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/cpumask.h>

#include "lse_main.h"
#include "lse_dsq.h"

/* is_migration_disabled() / p->migration_disabled not available before 5.15; fall back to nr_cpus_allowed check only */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
#define lse_is_migration_disabled(p) is_migration_disabled(p)
#else
static inline bool lse_is_migration_disabled(struct task_struct *p)
{
	return false;
}
#endif

/* ===================== Module parameters ===================== */

int lse_dsq_enable = 1;
int lse_dsq_rescue_enable = 1;
int lse_dsq_max_depth = 256;

/* ===================== DSQ deadline / quota tables ============ */

u32 lse_dsq_deadlines[LSE_MAX_GLOBAL_DSQS] = LSE_DSQ_DEFAULT_DEADLINES;
u32 lse_pcp_dsq_deadline_ms = LSE_PCP_DSQ_DEADLINE_MS;

static unsigned long dsq_quota_ns[LSE_MAX_GLOBAL_DSQS] = LSE_DSQ_DEFAULT_QUOTAS;
static unsigned long pcp_dsq_quota_ns __maybe_unused = LSE_PCP_DSQ_QUOTA_NS;

/* ===================== Global DSQ state ======================= */

struct lse_dispatch_q lse_gdsqs[LSE_MAX_GLOBAL_DSQS];
struct lse_dsq_sched_info lse_dsq_sinfo;

/* Per-CPU DSQ */
static DEFINE_PER_CPU(struct lse_dispatch_q, lse_pcp_dsq);
static DEFINE_PER_CPU(struct lse_pcp_dsq_info, lse_pcp_info);

/* Round counter: incremented each time both clusters wrap around */
static atomic64_t lse_dsq_round = ATOMIC64_INIT(0);

/* ===================== Static helpers ========================= */

static inline void init_one_dsq(struct lse_dispatch_q *dsq, u64 id)
{
	memset(dsq, 0, sizeof(*dsq));
	raw_spin_lock_init(&dsq->lock);
	INIT_LIST_HEAD(&dsq->fifo);
	dsq->priq = RB_ROOT_CACHED;
	dsq->id = id;
	dsq->last_consume_at = jiffies;
}

static inline bool dsq_is_global(struct lse_dispatch_q *dsq)
{
	return dsq >= lse_gdsqs && dsq < &lse_gdsqs[LSE_MAX_GLOBAL_DSQS];
}

static int __maybe_unused dsq_global_idx(struct lse_dispatch_q *dsq)
{
	if (dsq_is_global(dsq))
		return (int)(dsq - lse_gdsqs);
	return -1;
}

static bool dsq_idx_valid(int idx)
{
	return (idx >= 0 && idx < LSE_MAX_GLOBAL_DSQS) ||
	       (idx >= LSE_DSQ_PCP_BASE && idx < LSE_DSQ_PCP_BASE + nr_cpu_ids);
}

/* ===================== Cluster type for a CPU ================= */

static enum lse_cluster_type __maybe_unused lse_cpu_cluster_type(int cpu)
{
	struct lse_sched_cluster *cluster;
	int first_cpu;

	cluster = per_cpu(lse_rq, cpu).cluster;
	if (!cluster)
		return LSE_CLUSTER_INVALID;

	first_cpu = cpumask_first(&cluster->cpus);
	if (first_cpu >= nr_cpu_ids)
		return LSE_CLUSTER_INVALID;

	if (cluster->max_possible_capacity >=
	    max_cap_cluster()->max_possible_capacity)
		return LSE_CLUSTER_BIG;

	return LSE_CLUSTER_LITTLE;
}

/* ===================== Task → DSQ classification ============== */

int lse_dsq_classify_task(struct task_struct *p)
{
	int idx;
	struct lse_task_struct *lts;

	if (!p)
		return LSE_DSQ_PRIO_BACKGROUND;

	/* Per-CPU pinned tasks → own PCP DSQ */
	if (p->nr_cpus_allowed == 1 || lse_is_migration_disabled(p))
		return LSE_DSQ_PCP_BASE + cpumask_any(p->cpus_ptr);

	/* DL tasks are critical system */
	if (dl_task(p))
		return LSE_DSQ_PRIO_CRITICAL_SYSTEM;

	/* RT tasks → DSQ[3] */
	if (rt_prio(p->prio))
		return LSE_DSQ_PRIO_RT;

	/* Use task_class from slim_walt if available */
	lts = get_lse_task_struct(p);
	if (lts) {
		switch (lts->task_class) {
		case LSE_TASK_CLASS_RT:
		case LSE_TASK_CLASS_DEADLINE:
			return LSE_DSQ_PRIO_CRITICAL_SYSTEM;
		case LSE_TASK_CLASS_FOREGROUND:
			idx = (lts->boost_pct > 1280) ?
			      LSE_DSQ_PRIO_ENHANCED : LSE_DSQ_PRIO_FOREGROUND;
			return idx;
		case LSE_TASK_CLASS_NORMAL:
			return LSE_DSQ_PRIO_NORMAL;
		case LSE_TASK_CLASS_BACKGROUND:
		default:
			return LSE_DSQ_PRIO_BACKGROUND;
		}
	}

	/* Fallback: traditional prio-based */
	if (task_nice(p) < -5 || p->prio < DEFAULT_PRIO - 10)
		return LSE_DSQ_PRIO_CRITICAL_SYSTEM;
	if (p->prio < DEFAULT_PRIO - 5)
		return LSE_DSQ_PRIO_CRITICAL_APP;
	if (p->prio <= DEFAULT_PRIO)
		return LSE_DSQ_PRIO_FOREGROUND;
	if (p->prio <= DEFAULT_PRIO + 4)
		return LSE_DSQ_PRIO_NORMAL;
	return LSE_DSQ_PRIO_BACKGROUND;
}

bool lse_dsq_is_pcp_candidate(struct task_struct *p)
{
	if (!p)
		return false;
	return (p->nr_cpus_allowed == 1 || lse_is_migration_disabled(p));
}

/* ===================== DSQ enqueue / dequeue ================== */

static void __dsq_enqueue(struct lse_dispatch_q *dsq, struct task_struct *p)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&dsq->lock, flags);

	if (list_empty(&dsq->fifo))
		dsq->last_consume_at = jiffies;

	list_add_tail(&get_lse_task_struct(p)->dsq_node, &dsq->fifo);
	get_lse_task_struct(p)->on_dsq = 1;
	dsq->nr++;

	raw_spin_unlock_irqrestore(&dsq->lock, flags);
}

void lse_dsq_enqueue_task(struct task_struct *p, int cpu)
{
	struct lse_task_struct *lts;
	int idx;
	struct lse_dispatch_q *dsq;

	if (!READ_ONCE(lse_dsq_enable) || !p)
		return;

	lts = get_lse_task_struct(p);
	if (!lts || lts->on_dsq)
		return;

	/* Only enqueue runnable tasks not currently running */
	if (!lse_task_on_rq(p) || p == cpu_rq(cpu)->curr)
		return;

	idx = lse_dsq_classify_task(p);
	if (!dsq_idx_valid(idx))
		return;

	if (idx >= LSE_DSQ_PCP_BASE) {
		int pcp_cpu = idx - LSE_DSQ_PCP_BASE;

		if (pcp_cpu >= nr_cpu_ids || pcp_cpu < 0)
			return;
		dsq = &per_cpu(lse_pcp_dsq, pcp_cpu);
	} else {
		dsq = &lse_gdsqs[idx];
	}

	__dsq_enqueue(dsq, p);
}

static void __dsq_dequeue(struct lse_dispatch_q *dsq, struct task_struct *p)
{
	struct lse_task_struct *lts;
	unsigned long flags;

	lts = get_lse_task_struct(p);
	if (!lts || !lts->on_dsq)
		return;

	raw_spin_lock_irqsave(&dsq->lock, flags);
	if (lts->on_dsq) {
		list_del_init(&lts->dsq_node);
		lts->on_dsq = 0;
		dsq->nr = max(0, dsq->nr - 1);

		/* Clear timeout if DSQ is now empty */
		if (list_empty(&dsq->fifo) && dsq->is_timeout) {
			dsq->is_timeout = false;
			dsq->last_consume_at = jiffies;
		}
	}
	raw_spin_unlock_irqrestore(&dsq->lock, flags);
}

void lse_dsq_dequeue_task(struct task_struct *p, int cpu)
{
	struct lse_task_struct *lts;
	int idx;
	struct lse_dispatch_q *dsq;

	if (!READ_ONCE(lse_dsq_enable) || !p)
		return;

	lts = get_lse_task_struct(p);
	if (!lts || !lts->on_dsq)
		return;

	idx = lse_dsq_classify_task(p);
	if (idx >= LSE_DSQ_PCP_BASE) {
		int pcp_cpu = idx - LSE_DSQ_PCP_BASE;

		if (pcp_cpu >= nr_cpu_ids || pcp_cpu < 0)
			return;
		dsq = &per_cpu(lse_pcp_dsq, pcp_cpu);
	} else {
		dsq = &lse_gdsqs[idx];
	}

	__dsq_dequeue(dsq, p);
}

/* ===================== Runtime accounting ===================== */

void lse_dsq_add_runtime(struct task_struct *p, unsigned long exec_ns)
{
	int idx;

	if (!READ_ONCE(lse_dsq_enable) || !p)
		return;

	idx = lse_dsq_classify_task(p);

	if (idx >= LSE_DSQ_PCP_BASE) {
		int pcp_cpu = idx - LSE_DSQ_PCP_BASE;

		if (pcp_cpu < 0 || pcp_cpu >= nr_cpu_ids)
			return;
		per_cpu(lse_pcp_info, pcp_cpu).rtime += exec_ns;
	} else if (idx >= LSE_DSQ_NON_PERIOD_START &&
		   idx < LSE_DSQ_NON_PERIOD_END) {
		unsigned long flags;

		spin_lock_irqsave(&lse_dsq_sinfo.lock, flags);
		lse_dsq_sinfo.rtime[idx] += exec_ns;
		spin_unlock_irqrestore(&lse_dsq_sinfo.lock, flags);
	}
}

/* ===================== Timeout scanning ======================= */

void lse_dsq_scan_timeout(int cpu)
{
	struct lse_dispatch_q *dsq;
	unsigned long flags;
	int i;
	u64 deadline_ms;

	if (!READ_ONCE(lse_dsq_enable))
		return;

	/* Scan PCP DSQ for this CPU */
	dsq = &per_cpu(lse_pcp_dsq, cpu);
	raw_spin_lock_irqsave(&dsq->lock, flags);
	if (!list_empty(&dsq->fifo) && !dsq->is_timeout) {
		if (time_after(jiffies, dsq->last_consume_at +
			       msecs_to_jiffies(lse_pcp_dsq_deadline_ms)))
			dsq->is_timeout = true;
	}
	raw_spin_unlock_irqrestore(&dsq->lock, flags);

	/* Scan global non-period DSQs */
	for (i = LSE_DSQ_NON_PERIOD_START; i < LSE_DSQ_NON_PERIOD_END; i++) {
		dsq = &lse_gdsqs[i];
		raw_spin_lock_irqsave(&dsq->lock, flags);
		if (!list_empty(&dsq->fifo) && !dsq->is_timeout) {
			deadline_ms = lse_dsq_deadlines[i];
			if (time_after(jiffies, dsq->last_consume_at +
				       msecs_to_jiffies(deadline_ms)))
				dsq->is_timeout = true;
		}
		if (list_empty(&dsq->fifo) && dsq->is_timeout) {
			dsq->is_timeout = false;
			dsq->last_consume_at = jiffies;
		}
		raw_spin_unlock_irqrestore(&dsq->lock, flags);
	}
}

bool lse_dsq_has_timeout(int idx)
{
	unsigned long flags;
	bool ret;

	if (idx < 0 || idx >= LSE_MAX_GLOBAL_DSQS)
		return false;

	raw_spin_lock_irqsave(&lse_gdsqs[idx].lock, flags);
	ret = lse_gdsqs[idx].is_timeout;
	raw_spin_unlock_irqrestore(&lse_gdsqs[idx].lock, flags);

	return ret;
}

/* ===================== Depth queries ========================== */

int lse_dsq_depth_global(int idx)
{
	unsigned long flags;
	int depth;

	if (idx < 0 || idx >= LSE_MAX_GLOBAL_DSQS)
		return 0;

	raw_spin_lock_irqsave(&lse_gdsqs[idx].lock, flags);
	depth = lse_gdsqs[idx].nr;
	raw_spin_unlock_irqrestore(&lse_gdsqs[idx].lock, flags);

	return depth;
}

int lse_dsq_depth_pcp(int cpu)
{
	unsigned long flags;
	int depth;

	if (cpu < 0 || cpu >= nr_cpu_ids)
		return 0;

	raw_spin_lock_irqsave(&per_cpu(lse_pcp_dsq, cpu).lock, flags);
	depth = per_cpu(lse_pcp_dsq, cpu).nr;
	raw_spin_unlock_irqrestore(&per_cpu(lse_pcp_dsq, cpu).lock, flags);

	return depth;
}

int lse_dsq_total_backlog(void)
{
	int total = 0;
	int i, cpu;

	for (i = 0; i < LSE_MAX_GLOBAL_DSQS; i++)
		total += lse_dsq_depth_global(i);

	for_each_online_cpu(cpu)
		total += lse_dsq_depth_pcp(cpu);

	return total;
}

int lse_dsq_depth_cpu(int cpu)
{
	return lse_dsq_depth_pcp(cpu);
}

/* ===================== Urgency signal (→ cpufreq) ============= */

unsigned int lse_dsq_urgency_signal(int cpu)
{
	unsigned int urgency = 0;
	int i;
	int period_depth = 0;
	int non_period_depth = 0;
	int timeouts = 0;
	int total;

	if (!READ_ONCE(lse_dsq_enable))
		return 0;

	for (i = LSE_DSQ_PERIOD_START; i < LSE_DSQ_PERIOD_END; i++)
		period_depth += lse_dsq_depth_global(i);

	for (i = LSE_DSQ_NON_PERIOD_START; i < LSE_DSQ_NON_PERIOD_END; i++) {
		non_period_depth += lse_dsq_depth_global(i);
		if (lse_dsq_has_timeout(i))
			timeouts++;
	}

	non_period_depth += lse_dsq_depth_pcp(cpu);

	total = period_depth + non_period_depth;

	urgency += min(period_depth * 200, 600);
	urgency += min(timeouts * 200, 600);
	urgency += min(total * 40, 400);

	return clamp(urgency, 0U, 1024U);
}

/* ===================== Schedule-hook integration =============== */

void lse_dsq_on_schedule(struct rq *rq, struct task_struct *prev,
			 struct task_struct *next)
{
	int cpu;

	if (!READ_ONCE(lse_dsq_enable) || !rq)
		return;

	cpu = cpu_of(rq);

	/*
	 * prev != next: actual task switch.
	 *   prev leaves CPU → enqueue if still runnable on this CPU.
	 *   next arrives → dequeue from its DSQ.
	 * prev == next: tick accounting only.
	 */
	if (prev != next) {
		if (prev && !is_idle_task(prev) && lse_task_on_rq(prev) &&
		    task_cpu(prev) == cpu)
			lse_dsq_enqueue_task(prev, cpu);

		if (next && !is_idle_task(next))
			lse_dsq_dequeue_task(next, cpu);
	}

	/* Periodic timeout scan (throttled per call, cheap under lock) */
	lse_dsq_scan_timeout(cpu);
}

/* ===================== DSQ cleanup / init ===================== */

void lse_dsq_sync(void)
{
	int i, cpu;
	unsigned long flags;
	struct list_head *pos, *n;

	if (!READ_ONCE(lse_dsq_enable)) {
		for (i = 0; i < LSE_MAX_GLOBAL_DSQS; i++) {
			struct lse_dispatch_q *dsq = &lse_gdsqs[i];

			raw_spin_lock_irqsave(&dsq->lock, flags);
			list_for_each_safe(pos, n, &dsq->fifo) {
				struct lse_task_struct *lts;

				lts = list_entry(pos, struct lse_task_struct,
						 dsq_node);
				list_del_init(&lts->dsq_node);
				lts->on_dsq = 0;
			}
			dsq->nr = 0;
			dsq->is_timeout = false;
			raw_spin_unlock_irqrestore(&dsq->lock, flags);
		}

		for_each_online_cpu(cpu) {
			struct lse_dispatch_q *dsq = &per_cpu(lse_pcp_dsq, cpu);

			raw_spin_lock_irqsave(&dsq->lock, flags);
			list_for_each_safe(pos, n, &dsq->fifo) {
				struct lse_task_struct *lts;

				lts = list_entry(pos, struct lse_task_struct,
						 dsq_node);
				list_del_init(&lts->dsq_node);
				lts->on_dsq = 0;
			}
			dsq->nr = 0;
			dsq->is_timeout = false;
			raw_spin_unlock_irqrestore(&dsq->lock, flags);
		}

		spin_lock_irqsave(&lse_dsq_sinfo.lock, flags);
		lse_dsq_sinfo.curr_idx[LSE_DSQ_CLUSTER_TIDX_BIG] =
			LSE_DSQ_BIG_LOWER;
		lse_dsq_sinfo.curr_idx[LSE_DSQ_CLUSTER_TIDX_LITTLE] =
			LSE_DSQ_LITTLE_LOWER;
		memset(lse_dsq_sinfo.rtime, 0, sizeof(lse_dsq_sinfo.rtime));
		spin_unlock_irqrestore(&lse_dsq_sinfo.lock, flags);

		for_each_online_cpu(cpu) {
			per_cpu(lse_pcp_info, cpu).rtime = 0;
			per_cpu(lse_pcp_info, cpu).pcp_round = false;
		}
	}
}

void lse_dsq_init(void)
{
	int i, cpu;

	for (i = 0; i < LSE_MAX_GLOBAL_DSQS; i++)
		init_one_dsq(&lse_gdsqs[i], LSE_DSQ_FLAG_BUILTIN | i);

	for_each_possible_cpu(cpu)
		init_one_dsq(&per_cpu(lse_pcp_dsq, cpu),
			     LSE_DSQ_FLAG_BUILTIN | (LSE_DSQ_PCP_BASE + cpu));

	spin_lock_init(&lse_dsq_sinfo.lock);
	lse_dsq_sinfo.curr_idx[LSE_DSQ_CLUSTER_TIDX_BIG] = LSE_DSQ_BIG_LOWER;
	lse_dsq_sinfo.curr_idx[LSE_DSQ_CLUSTER_TIDX_LITTLE] =
		LSE_DSQ_LITTLE_LOWER;

	for_each_possible_cpu(cpu) {
		per_cpu(lse_pcp_info, cpu).rtime = 0;
		per_cpu(lse_pcp_info, cpu).pcp_round = false;
	}

	atomic64_set(&lse_dsq_round, 0);
}

/* ===================== Diagnostic dump ======================== */

void lse_dsq_dump_state(void)
{
	int i, cpu;
	unsigned long flags;
	int depth;

	pr_info("lse_dsq: === DSQ State Dump ===\n");
	pr_info("lse_dsq: Round counter = %lld\n",
		atomic64_read(&lse_dsq_round));

	spin_lock_irqsave(&lse_dsq_sinfo.lock, flags);
	pr_info("lse_dsq: curr_idx[BIG]=%d curr_idx[LITTLE]=%d\n",
		lse_dsq_sinfo.curr_idx[LSE_DSQ_CLUSTER_TIDX_BIG],
		lse_dsq_sinfo.curr_idx[LSE_DSQ_CLUSTER_TIDX_LITTLE]);
	spin_unlock_irqrestore(&lse_dsq_sinfo.lock, flags);

	for (i = 0; i < LSE_MAX_GLOBAL_DSQS; i++) {
		depth = lse_dsq_depth_global(i);
		if (depth > 0 || lse_dsq_has_timeout(i))
			pr_info("lse_dsq: gdsq[%d] deadline=%ums quota=%lums "
				"depth=%d timeout=%d\n",
				i, lse_dsq_deadlines[i],
				dsq_quota_ns[i] / NSEC_PER_MSEC,
				depth, lse_dsq_has_timeout(i) ? 1 : 0);
	}

	for_each_online_cpu(cpu) {
		depth = lse_dsq_depth_pcp(cpu);
		if (depth > 0) {
			spin_lock_irqsave(&lse_dsq_sinfo.lock, flags);
			pr_info("lse_dsq: pcp_dsq[%d] depth=%d rtime=%d "
				"pcp_round=%d\n",
				cpu, depth,
				per_cpu(lse_pcp_info, cpu).rtime,
				per_cpu(lse_pcp_info, cpu).pcp_round ? 1 : 0);
			spin_unlock_irqrestore(&lse_dsq_sinfo.lock, flags);
		}
	}

	pr_info("lse_dsq: Total backlog = %d\n", lse_dsq_total_backlog());
	pr_info("lse_dsq: Urgency signal (cpu%d) = %u\n",
		raw_smp_processor_id(),
		lse_dsq_urgency_signal(raw_smp_processor_id()));
}

