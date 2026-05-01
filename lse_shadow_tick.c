// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 *
 * This code is part of LunarKernel.
 * shadow tick support inspired by hmbird.
 */

#include <linux/hrtimer.h>

#include "lse_main.h"

#define LSE_SHADOW_TICK_NS	1000000ULL

extern unsigned int highres_tick_ctrl;
extern unsigned int highres_tick_ctrl_dbg;

static bool lse_shadow_tick_inited;
static DEFINE_PER_CPU(struct hrtimer, lse_shadow_tick_timer);
static DEFINE_PER_CPU(bool, lse_shadow_tick_armed);

static inline bool lse_shadow_tick_enabled(void)
{
	return READ_ONCE(highres_tick_ctrl) != 0;
}

static inline struct hrtimer *lse_shadow_tick_timer_get(int cpu)
{
	return &per_cpu(lse_shadow_tick_timer, cpu);
}

static void lse_shadow_tick_stop_cpu(int cpu)
{
	struct hrtimer *timer = lse_shadow_tick_timer_get(cpu);

	if (!per_cpu(lse_shadow_tick_armed, cpu))
		return;

	hrtimer_cancel(timer);
	per_cpu(lse_shadow_tick_armed, cpu) = false;
}

static void lse_shadow_tick_start_cpu(int cpu)
{
	struct hrtimer *timer = lse_shadow_tick_timer_get(cpu);

	if (!lse_shadow_tick_enabled()) {
		lse_shadow_tick_stop_cpu(cpu);
		return;
	}

	if (per_cpu(lse_shadow_tick_armed, cpu))
		return;

	hrtimer_start(timer, ns_to_ktime(LSE_SHADOW_TICK_NS),
		      HRTIMER_MODE_REL_PINNED);
	per_cpu(lse_shadow_tick_armed, cpu) = true;
}

static enum hrtimer_restart lse_shadow_tick_cb(struct hrtimer *timer)
{
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);
	struct rq_flags rf;

	if (READ_ONCE(highres_tick_ctrl_dbg) && cpu == 0)
		trace_printk("lse_shadow_tick cpu=%d\n", cpu);

	if (!lse_shadow_tick_enabled() || !slim_walt_ctrl) {
		per_cpu(lse_shadow_tick_armed, cpu) = false;
		return HRTIMER_NORESTART;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	rq_lock(rq, &rf);
#else
	rq_lock(rq, &rf);
#endif
	update_rq_clock(rq);
	if (rq->curr != rq->idle)
		lse_tick_entry(NULL, rq);
	else
		lse_monitor_touch();
	rq_unlock(rq, &rf);

	hrtimer_forward_now(timer, ns_to_ktime(LSE_SHADOW_TICK_NS));
	return HRTIMER_RESTART;
}

void lse_shadow_tick_update_cpu(struct rq *rq)
{
	int cpu;

	if (!lse_shadow_tick_inited)
		return;

	cpu = cpu_of(rq);
	if (!lse_shadow_tick_enabled()) {
		lse_shadow_tick_stop_cpu(cpu);
		return;
	}

	if (rq->curr == rq->idle)
		lse_shadow_tick_stop_cpu(cpu);
	else
		lse_shadow_tick_start_cpu(cpu);
}

void lse_shadow_tick_sync_all(void)
{
	int cpu;

	if (!lse_shadow_tick_inited)
		return;

	for_each_online_cpu(cpu)
		lse_shadow_tick_update_cpu(cpu_rq(cpu));
}

void lse_shadow_tick_init(void)
{
	int cpu;

	if (lse_shadow_tick_inited)
		return;

	for_each_possible_cpu(cpu) {
		struct hrtimer *timer = lse_shadow_tick_timer_get(cpu);

		hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
		timer->function = lse_shadow_tick_cb;
		per_cpu(lse_shadow_tick_armed, cpu) = false;
	}

	lse_shadow_tick_inited = true;
}
