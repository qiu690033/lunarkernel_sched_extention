// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 *
 * This code is part of LunarKernel.
 * sysctl interface.
 *
 * File name: lse_sysctl.c
 * Author: Cloud_Yun <1770669041@qq.com>
 * Version: v251003_Dev
 * Date: 2025/10/3 Friday
 */

#include <linux/kernel.h>
#include <linux/sysctl.h>

#include "lse_main.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define LSE_CTL_TABLE_ARG const struct ctl_table
#else
#define LSE_CTL_TABLE_ARG struct ctl_table
#endif

int slim_walt_ctrl = 1;
int frame_per_sec = 120;
unsigned int highres_tick_ctrl;
unsigned int highres_tick_ctrl_dbg;

static int window_stats_policy_minval = WINDOW_STATS_RECENT;
static int window_stats_policy_maxval = WINDOW_STATS_INVALID_POLICY;
static int lse_prio_minval = 0;
static int lse_prio_maxval = 139;
static int lse_boost_minval = 1;
static int lse_boost_maxval = 4096;
static int lse_dsq_depth_minval = 1;
static int lse_dsq_depth_maxval = 4096;

static int lse_proc_shadow_tick_update(LSE_CTL_TABLE_ARG *table,
				int write, void __user *buffer, size_t *lenp,
				loff_t *ppos)
{
	int ret;
	unsigned int val;
	static DEFINE_MUTEX(mutex);

	struct ctl_table tmp = {
		.data	= &val,
		.maxlen	= sizeof(val),
		.mode	= table->mode,
	};

	mutex_lock(&mutex);

	val = highres_tick_ctrl;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	ret = proc_dobool(&tmp, write, buffer, lenp, ppos);
#else
	ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);
#endif
	if (!ret && write && val != highres_tick_ctrl) {
		highres_tick_ctrl = val;
		lse_shadow_tick_sync_all();
	}

	mutex_unlock(&mutex);
	return ret;
}

static int lse_proc_sched_ravg_window_update(LSE_CTL_TABLE_ARG *table,
				int write, void __user *buffer, size_t *lenp,
				loff_t *ppos)
{
	int ret = -EPERM;
	int val;
	static DEFINE_MUTEX(mutex);

	struct ctl_table tmp = {
		.data	= &val,
		.maxlen	= sizeof(val),
		.mode	= table->mode,
	};

	mutex_lock(&mutex);

	val = frame_per_sec;
	ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);
	if (ret || !write || (val == frame_per_sec))
		goto unlock;
	frame_per_sec = val;

	sched_ravg_window_change(frame_per_sec);

unlock:
	mutex_unlock(&mutex);
	return ret;
}

static int lse_proc_monitor_toggle_update(LSE_CTL_TABLE_ARG *table,
				int write, void __user *buffer, size_t *lenp,
				loff_t *ppos)
{
	int ret;
	int val;
	static DEFINE_MUTEX(mutex);

	struct ctl_table tmp = {
		.data	= &val,
		.maxlen	= sizeof(val),
		.mode	= table->mode,
	};

	mutex_lock(&mutex);

	val = READ_ONCE(*(int *)table->data);
	ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);
	if (!ret && write && val != READ_ONCE(*(int *)table->data)) {
		WRITE_ONCE(*(int *)table->data, val);
		lse_monitor_sync();
	}

	mutex_unlock(&mutex);
	return ret;
}

static int lse_proc_dsq_toggle_update(LSE_CTL_TABLE_ARG *table,
				int write, void __user *buffer, size_t *lenp,
				loff_t *ppos)
{
	int ret;
	int val;
	static DEFINE_MUTEX(mutex);

	struct ctl_table tmp = {
		.data	= &val,
		.maxlen	= sizeof(val),
		.mode	= table->mode,
	};

	mutex_lock(&mutex);

	val = READ_ONCE(*(int *)table->data);
	ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);
	if (!ret && write && val != READ_ONCE(*(int *)table->data)) {
		WRITE_ONCE(*(int *)table->data, val);
		lse_dsq_sync();
	}

	mutex_unlock(&mutex);
	return ret;
}

struct ctl_table lse_table[] = {
    {
        .procname     = "slim_walt_ctrl",
        .data         = &slim_walt_ctrl,
        .maxlen       = sizeof(int),
        .mode         = 0644,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
        .proc_handler = proc_dobool,
#else
        .proc_handler = proc_dointvec,
#endif
    },
	{
		.procname	= "slim_stats",
		.data		= &slim_stats,
		.maxlen		= sizeof(int),
		.mode		= 0666,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "heartbeat",
		.data		= &heartbeat,
		.maxlen		= sizeof(int),
		.mode		= 0666,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "fg_prio_threshold",
		.data		= &lse_fg_prio_threshold,
		.maxlen		= sizeof(int),
		.mode		= 0666,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &lse_prio_minval,
		.extra2		= &lse_prio_maxval,
	},
	{
		.procname	= "boost_bg_pct",
		.data		= &lse_boost_bg_pct,
		.maxlen		= sizeof(int),
		.mode		= 0666,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &lse_boost_minval,
		.extra2		= &lse_boost_maxval,
	},
	{
		.procname	= "boost_fg_pct",
		.data		= &lse_boost_fg_pct,
		.maxlen		= sizeof(int),
		.mode		= 0666,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &lse_boost_minval,
		.extra2		= &lse_boost_maxval,
	},
	{
		.procname	= "boost_rt_pct",
		.data		= &lse_boost_rt_pct,
		.maxlen		= sizeof(int),
		.mode		= 0666,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &lse_boost_minval,
		.extra2		= &lse_boost_maxval,
	},
    {
        .procname     = "slim_walt_policy",
        .data         = &sched_window_stats_policy,
        .maxlen       = sizeof(int),
        .mode         = 0644,
        .proc_handler = proc_dointvec_minmax,
        .extra1       = &window_stats_policy_minval,
        .extra2       = &window_stats_policy_maxval,
    },
    {
        .procname     = "sched_ravg_window_frame_per_sec",
        .data         = &frame_per_sec,
        .maxlen       = sizeof(int),
        .mode         = 0644,
        .proc_handler = lse_proc_sched_ravg_window_update,
        .extra1       = SYSCTL_ZERO,
        .extra2       = SYSCTL_INT_MAX,
    },
	{
		.procname	= "lse_gov_debug",
		.data		= &sysctl_lse_gov_debug,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0666,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "scx_shadow_tick_enable",
		.data		= &highres_tick_ctrl,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0666,
		.proc_handler	= lse_proc_shadow_tick_update,
	},
	{
		.procname	= "heartbeat_enable",
		.data		= &heartbeat_enable,
		.maxlen		= sizeof(int),
		.mode		= 0666,
		.proc_handler	= lse_proc_monitor_toggle_update,
	},
	{
		.procname	= "watchdog_enable",
		.data		= &watchdog_enable,
		.maxlen		= sizeof(int),
		.mode		= 0666,
		.proc_handler	= lse_proc_monitor_toggle_update,
	},
	{
		.procname	= "dsq_enable",
		.data		= &lse_dsq_enable,
		.maxlen		= sizeof(int),
		.mode		= 0666,
		.proc_handler	= lse_proc_dsq_toggle_update,
	},
	{
		.procname	= "dsq_rescue_enable",
		.data		= &lse_dsq_rescue_enable,
		.maxlen		= sizeof(int),
		.mode		= 0666,
		.proc_handler	= lse_proc_dsq_toggle_update,
	},
	{
		.procname	= "dsq_max_depth",
		.data		= &lse_dsq_max_depth,
		.maxlen		= sizeof(int),
		.mode		= 0666,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &lse_dsq_depth_minval,
		.extra2		= &lse_dsq_depth_maxval,
	},
	{
		.procname	= "highres_tick_ctrl_dbg",
		.data		= &highres_tick_ctrl_dbg,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0666,
		.proc_handler	= proc_dointvec,
	},
    { },
};

void lse_sysctl_init(void)
{
    struct ctl_table_header *hdr;
    hdr = register_sysctl("lunar_sched_ext", lse_table);
    kmemleak_not_leak(hdr);
	lse_dsq_sync();
	lse_monitor_sync();
	lse_shadow_tick_sync_all();
}
