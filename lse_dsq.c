// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 *
 * Lightweight DSQ layer:
 * - per-cpu dispatch queue shadow
 * - enqueue/dequeue around schedule path
 * - stale entry cleanup
 * - optional rescue hint via resched_curr()
 */

#include <linux/smp.h>

#include "lse_main.h"

int lse_dsq_enable = 1;
int lse_dsq_rescue_enable = 1;
int lse_dsq_max_depth = 256;

struct lse_dsq_cpu {
	raw_spinlock_t lock;
	struct list_head q;
	int depth;
};

static DEFINE_PER_CPU(struct lse_dsq_cpu, lse_dsq_cpu);

static inline struct lse_dsq_cpu *lse_this_dsq(int cpu)
{
	return &per_cpu(lse_dsq_cpu, cpu);
}

static inline bool lse_dsq_task_valid(struct task_struct *p)
{
	if (!p)
		return false;
	if (is_idle_task(p))
		return false;
	return true;
}

static void lse_dsq_detach_locked(struct lse_task_struct *lts)
{
	if (!lts->on_dsq)
		return;

	list_del_init(&lts->dsq_node);
	lts->on_dsq = 0;
}

static void lse_dsq_compact_locked(struct lse_dsq_cpu *dsq, int cpu)
{
	struct list_head *pos, *n;

	list_for_each_safe(pos, n, &dsq->q) {
		struct lse_task_struct *lts;
		struct task_struct *p;

		lts = list_entry(pos, struct lse_task_struct, dsq_node);
		p = lts_to_ts(lts);
		if (!p || !lse_task_on_rq(p) || task_cpu(p) != cpu) {
			lse_dsq_detach_locked(lts);
			dsq->depth = max(0, dsq->depth - 1);
		}
	}
}

static void lse_dsq_enqueue_locked(struct lse_dsq_cpu *dsq, struct task_struct *p)
{
	struct lse_task_struct *lts = get_lse_task_struct(p);
	int limit = READ_ONCE(lse_dsq_max_depth);

	if (!lts)
		return;
	if (lts->on_dsq)
		return;
	if (limit <= 0)
		limit = 1;
	if (dsq->depth >= limit)
		return;

	INIT_LIST_HEAD(&lts->dsq_node);
	list_add_tail(&lts->dsq_node, &dsq->q);
	lts->on_dsq = 1;
	dsq->depth++;
}

static void lse_dsq_dequeue_locked(struct lse_dsq_cpu *dsq, struct task_struct *p)
{
	struct lse_task_struct *lts = get_lse_task_struct(p);

	if (!lts || !lts->on_dsq)
		return;

	lse_dsq_detach_locked(lts);
	dsq->depth = max(0, dsq->depth - 1);
}

int lse_dsq_depth_cpu(int cpu)
{
	struct lse_dsq_cpu *dsq;
	unsigned long flags;
	int depth;

	if (cpu < 0 || cpu >= nr_cpu_ids)
		return 0;

	dsq = lse_this_dsq(cpu);
	raw_spin_lock_irqsave(&dsq->lock, flags);
	depth = dsq->depth;
	raw_spin_unlock_irqrestore(&dsq->lock, flags);
	return depth;
}

static void lse_dsq_maybe_rescue(struct rq *rq)
{
	int self_depth;
	int cpu;

	if (!READ_ONCE(lse_dsq_rescue_enable))
		return;
	if (!rq || is_idle_task(rq->curr))
		return;

	self_depth = lse_dsq_depth_cpu(cpu_of(rq));
	if (self_depth > 1)
		return;

	for_each_online_cpu(cpu) {
		if (cpu == cpu_of(rq))
			continue;
		if (lse_dsq_depth_cpu(cpu) > self_depth + 2) {
			resched_curr(rq);
			return;
		}
	}
}

void lse_dsq_on_schedule(struct rq *rq, struct task_struct *prev,
			 struct task_struct *next)
{
	struct lse_dsq_cpu *dsq;
	unsigned long flags;
	int cpu;

	if (!READ_ONCE(lse_dsq_enable))
		return;
	if (!rq)
		return;

	cpu = cpu_of(rq);
	dsq = lse_this_dsq(cpu);

	raw_spin_lock_irqsave(&dsq->lock, flags);
	lse_dsq_compact_locked(dsq, cpu);

	if (lse_dsq_task_valid(next))
		lse_dsq_dequeue_locked(dsq, next);

	if (lse_dsq_task_valid(prev) && lse_task_on_rq(prev) &&
	    task_cpu(prev) == cpu)
		lse_dsq_enqueue_locked(dsq, prev);

	raw_spin_unlock_irqrestore(&dsq->lock, flags);

	lse_dsq_maybe_rescue(rq);
}

void lse_dsq_sync(void)
{
	int cpu;

	if (!READ_ONCE(lse_dsq_enable)) {
		for_each_online_cpu(cpu) {
			struct lse_dsq_cpu *dsq = lse_this_dsq(cpu);
			unsigned long flags;
			struct list_head *pos, *n;

			raw_spin_lock_irqsave(&dsq->lock, flags);
			list_for_each_safe(pos, n, &dsq->q) {
				struct lse_task_struct *lts;

				lts = list_entry(pos, struct lse_task_struct, dsq_node);
				lse_dsq_detach_locked(lts);
			}
			dsq->depth = 0;
			raw_spin_unlock_irqrestore(&dsq->lock, flags);
		}
	}
}

void lse_dsq_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct lse_dsq_cpu *dsq = lse_this_dsq(cpu);

		raw_spin_lock_init(&dsq->lock);
		INIT_LIST_HEAD(&dsq->q);
		dsq->depth = 0;
	}
}
