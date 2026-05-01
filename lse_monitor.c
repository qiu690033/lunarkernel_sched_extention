// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 *
 * Minimal monitor layer inspired by hmbird:
 * heartbeat, watchdog and lightweight stats.
 */

#include <linux/jiffies.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/uaccess.h>

#include "lse_main.h"

#define LSE_HEARTBEAT_PERIOD_MS	1000
#define LSE_WATCHDOG_TIMEOUT_MS	30000

int slim_stats;
int heartbeat;
int heartbeat_enable;
int watchdog_enable;

static unsigned long lse_watchdog_timeout;
static unsigned long lse_watchdog_timestamp = INITIAL_JIFFIES;
static unsigned long lse_heartbeat_last_touch;

static struct delayed_work lse_watchdog_work;
static struct timer_list lse_heartbeat_timer;
static struct proc_dir_entry *lse_proc_root;
static bool lse_monitor_inited;

static atomic64_t lse_stats_update_cnt = ATOMIC64_INIT(0);
static atomic64_t lse_stats_rollover_cnt = ATOMIC64_INIT(0);
static atomic64_t lse_stats_tick_cnt = ATOMIC64_INIT(0);

static inline void lse_stats_inc(atomic64_t *cnt)
{
	if (READ_ONCE(slim_stats))
		atomic64_inc(cnt);
}

void lse_stats_record_update(void)
{
	lse_stats_inc(&lse_stats_update_cnt);
}

void lse_stats_record_rollover(void)
{
	lse_stats_inc(&lse_stats_rollover_cnt);
}

static void lse_stats_record_tick(void)
{
	lse_stats_inc(&lse_stats_tick_cnt);
}

static void lse_monitor_reset_stats(void)
{
	atomic64_set(&lse_stats_update_cnt, 0);
	atomic64_set(&lse_stats_rollover_cnt, 0);
	atomic64_set(&lse_stats_tick_cnt, 0);
	WRITE_ONCE(heartbeat, 0);
	WRITE_ONCE(lse_heartbeat_last_touch, jiffies);
	WRITE_ONCE(lse_watchdog_timestamp, jiffies);
}

static void lse_monitor_dump_state(const char *reason)
{
	pr_warn_ratelimited(
		"lse: %s | walt=%d stats=%d hb=%d hb_en=%d wd_en=%d shadow=%u updates=%lld rollover=%lld ticks=%lld hb_age=%lu wd_age=%lu\n",
		reason,
		READ_ONCE(slim_walt_ctrl),
		READ_ONCE(slim_stats),
		READ_ONCE(heartbeat),
		READ_ONCE(heartbeat_enable),
		READ_ONCE(watchdog_enable),
		READ_ONCE(highres_tick_ctrl),
		atomic64_read(&lse_stats_update_cnt),
		atomic64_read(&lse_stats_rollover_cnt),
		atomic64_read(&lse_stats_tick_cnt),
		jiffies - READ_ONCE(lse_heartbeat_last_touch),
		jiffies - READ_ONCE(lse_watchdog_timestamp));
}

static const char *lse_task_class_name(u8 cls)
{
	switch (cls) {
	case LSE_TASK_CLASS_IDLE:
		return "idle";
	case LSE_TASK_CLASS_BACKGROUND:
		return "background";
	case LSE_TASK_CLASS_NORMAL:
		return "normal";
	case LSE_TASK_CLASS_FOREGROUND:
		return "foreground";
	case LSE_TASK_CLASS_RT:
		return "rt";
	case LSE_TASK_CLASS_DEADLINE:
		return "deadline";
	default:
		return "unknown";
	}
}

void lse_monitor_touch(void)
{
	unsigned long now = jiffies;

	WRITE_ONCE(heartbeat, 1);
	WRITE_ONCE(lse_watchdog_timestamp, now);

	if (now != READ_ONCE(lse_heartbeat_last_touch)) {
		lse_stats_record_tick();
		WRITE_ONCE(lse_heartbeat_last_touch, now);
	}
}

static void lse_heartbeat_timer_fn(struct timer_list *timer)
{
	if (!READ_ONCE(heartbeat_enable))
		return;

	if (READ_ONCE(heartbeat))
		WRITE_ONCE(heartbeat, 0);
	else
		lse_monitor_dump_state("heartbeat stalled");

	mod_timer(timer, jiffies + msecs_to_jiffies(LSE_HEARTBEAT_PERIOD_MS));
}

static void lse_watchdog_workfn(struct work_struct *work)
{
	unsigned long timeout = READ_ONCE(lse_watchdog_timeout);
	(void)work;

	if (!READ_ONCE(watchdog_enable))
		return;

	if (time_after(jiffies, READ_ONCE(lse_watchdog_timestamp) + timeout))
		lse_monitor_dump_state("watchdog timeout");

	queue_delayed_work(system_unbound_wq, &lse_watchdog_work,
			   max(1UL, timeout / 2));
}

static ssize_t lse_proc_status_read(struct file *file, char __user *buf,
				    size_t count, loff_t *ppos)
{
	char tmp[512];
	int len;

	(void)file;

	len = scnprintf(tmp, sizeof(tmp),
		"slim_walt_ctrl=%d\n"
		"slim_stats=%d\n"
		"heartbeat=%d\n"
		"heartbeat_enable=%d\n"
		"watchdog_enable=%d\n"
		"fg_prio_threshold=%d\n"
		"boost_bg_pct=%d\n"
		"boost_fg_pct=%d\n"
		"boost_rt_pct=%d\n"
		"dsq_enable=%d\n"
		"dsq_rescue_enable=%d\n"
		"dsq_max_depth=%d\n"
		"shadow_tick_enable=%u\n"
		"heartbeat_age_jiffies=%lu\n"
		"watchdog_age_jiffies=%lu\n"
		"watchdog_timeout_jiffies=%lu\n",
		READ_ONCE(slim_walt_ctrl),
		READ_ONCE(slim_stats),
		READ_ONCE(heartbeat),
		READ_ONCE(heartbeat_enable),
		READ_ONCE(watchdog_enable),
		READ_ONCE(lse_fg_prio_threshold),
		READ_ONCE(lse_boost_bg_pct),
		READ_ONCE(lse_boost_fg_pct),
		READ_ONCE(lse_boost_rt_pct),
		READ_ONCE(lse_dsq_enable),
		READ_ONCE(lse_dsq_rescue_enable),
		READ_ONCE(lse_dsq_max_depth),
		READ_ONCE(highres_tick_ctrl),
		jiffies - READ_ONCE(lse_heartbeat_last_touch),
		jiffies - READ_ONCE(lse_watchdog_timestamp),
		READ_ONCE(lse_watchdog_timeout));

	return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static ssize_t lse_proc_stats_read(struct file *file, char __user *buf,
				   size_t count, loff_t *ppos)
{
	char tmp[512];
	int len;

	(void)file;

	len = scnprintf(tmp, sizeof(tmp),
		"update_cnt=%lld\n"
		"rollover_cnt=%lld\n"
		"tick_cnt=%lld\n",
		atomic64_read(&lse_stats_update_cnt),
		atomic64_read(&lse_stats_rollover_cnt),
		atomic64_read(&lse_stats_tick_cnt));

	return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static ssize_t lse_proc_tasks_read(struct file *file, char __user *buf,
				   size_t count, loff_t *ppos)
{
	char tmp[1024];
	int len = 0;
	int cpu;

	(void)file;

	len += scnprintf(tmp + len, sizeof(tmp) - len,
			 "cpu pid comm class boost prio demand scaled dsq_depth\n");
	for_each_online_cpu(cpu) {
		struct task_struct *p = cpu_rq(cpu)->curr;
		struct lse_task_struct *lts = get_lse_task_struct(p);

		if (!lts)
			continue;

		len += scnprintf(tmp + len, sizeof(tmp) - len,
				 "%d %d %s %s %u %u %u %u %d\n",
				 cpu, p->pid, p->comm,
				 lse_task_class_name(lts->task_class),
				 lts->boost_pct, lts->priority_hint,
				 lts->demand, lts->demand_scaled,
				 lse_dsq_depth_cpu(cpu));
		if (len >= sizeof(tmp) - 96)
			break;
	}

	return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static ssize_t lse_proc_stats_reset_write(struct file *file,
					  const char __user *buf,
					  size_t count, loff_t *ppos)
{
	(void)file;
	(void)buf;
	(void)ppos;

	if (count)
		lse_monitor_reset_stats();

	return count;
}

static const struct proc_ops lse_status_proc_ops = {
	.proc_read	= lse_proc_status_read,
	.proc_lseek	= default_llseek,
};

static const struct proc_ops lse_stats_proc_ops = {
	.proc_read	= lse_proc_stats_read,
	.proc_lseek	= default_llseek,
};

static const struct proc_ops lse_tasks_proc_ops = {
	.proc_read	= lse_proc_tasks_read,
	.proc_lseek	= default_llseek,
};

static const struct proc_ops lse_stats_reset_proc_ops = {
	.proc_write	= lse_proc_stats_reset_write,
	.proc_lseek	= default_llseek,
};

static void lse_monitor_proc_init(void)
{
	if (lse_proc_root)
		return;

	lse_proc_root = proc_mkdir("lunar_sched_ext", NULL);
	if (!lse_proc_root) {
		pr_warn("lse: failed to create /proc/lunar_sched_ext\n");
		return;
	}

	proc_create("status", 0444, lse_proc_root, &lse_status_proc_ops);
	proc_create("stats", 0444, lse_proc_root, &lse_stats_proc_ops);
	proc_create("tasks", 0444, lse_proc_root, &lse_tasks_proc_ops);
	proc_create("reset_stats", 0220, lse_proc_root, &lse_stats_reset_proc_ops);
}

void lse_monitor_sync(void)
{
	if (!lse_monitor_inited)
		return;

	if (READ_ONCE(heartbeat_enable))
		mod_timer(&lse_heartbeat_timer,
			  jiffies + msecs_to_jiffies(LSE_HEARTBEAT_PERIOD_MS));
	else
		del_timer_sync(&lse_heartbeat_timer);

	if (READ_ONCE(watchdog_enable))
		queue_delayed_work(system_unbound_wq, &lse_watchdog_work,
				   max(1UL, READ_ONCE(lse_watchdog_timeout) / 2));
	else
		cancel_delayed_work_sync(&lse_watchdog_work);
}

void lse_monitor_init(void)
{
	if (lse_monitor_inited)
		return;

	WRITE_ONCE(lse_watchdog_timeout,
		   msecs_to_jiffies(LSE_WATCHDOG_TIMEOUT_MS));
	timer_setup(&lse_heartbeat_timer, lse_heartbeat_timer_fn, 0);
	INIT_DELAYED_WORK(&lse_watchdog_work, lse_watchdog_workfn);
	WRITE_ONCE(lse_heartbeat_last_touch, jiffies);
	WRITE_ONCE(lse_watchdog_timestamp, jiffies);
	lse_monitor_proc_init();
	lse_monitor_inited = true;
	lse_monitor_sync();
}
