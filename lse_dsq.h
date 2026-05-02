// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 *
 * Complete DSQ (Dispatch Queue) layer ported from hmbird.
 *
 * Architecture:
 *   10-Level Global DSQ with deadline-based priority
 *   Per-CPU local DSQ with quota tracking
 *   Cluster-aware round-robin (BIG / LITTLE)
 *   Timeout detection and priority boosting
 *
 * DSQ Layout (in priority order):
 *   [0] deadline=0ms   — critical system tasks, period allowed
 *   [1] deadline=1ms   — critical app top tasks
 *   [2] deadline=2ms   — mid-priority period tasks
 *   [3] deadline=4ms   — RT tasks
 *   [4] deadline=6ms   — UX compatible
 *   [5] deadline=8ms   — non-period start, quota=32ms
 *   [6] deadline=16ms  — non-period, quota=20ms
 *   [7] deadline=32ms  — non-period, quota=14ms
 *   [8] deadline=64ms  — CLUSTER_SEPARATE_IDX, quota=8ms
 *   [9] deadline=128ms — non-period, quota=6ms
 */

#ifndef _LSE_DSQ_H_
#define _LSE_DSQ_H_

#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/cpumask.h>

/*
 * Global DSQ count and layout
 */
#define LSE_MAX_GLOBAL_DSQS        10

/* Period DSQs: always checked even when nonperiod is disallowed */
#define LSE_DSQ_PERIOD_START       0
#define LSE_DSQ_PERIOD_END         4  /* [0, 4) */

/* UX compatible DSQ */
#define LSE_DSQ_UX_IDX             4

/* Non-period DSQs: share round-robin among themselves */
#define LSE_DSQ_NON_PERIOD_START   5
#define LSE_DSQ_NON_PERIOD_END     10 /* [5, 10) */

/* Cluster separation boundary: BIG cluster uses [5,8), LITTLE uses [8,10) */
#define LSE_DSQ_CLUSTER_SEPARATE_IDX  8

/* Index range for BIG cluster round-robin */
#define LSE_DSQ_BIG_LOWER    LSE_DSQ_NON_PERIOD_START
#define LSE_DSQ_BIG_UPPER    LSE_DSQ_CLUSTER_SEPARATE_IDX  /* [5, 8) */

/* Index range for LITTLE cluster round-robin */
#define LSE_DSQ_LITTLE_LOWER LSE_DSQ_CLUSTER_SEPARATE_IDX
#define LSE_DSQ_LITTLE_UPPER LSE_DSQ_NON_PERIOD_END        /* [8, 10) */

/* Cluster round-robin indices */
#define LSE_DSQ_CLUSTER_TIDX_BIG    0
#define LSE_DSQ_CLUSTER_TIDX_LITTLE 1
#define LSE_DSQ_MAX_CLUSTER_TIDX    2

/* Per-CPU DSQ base index (offset from 0) */
#define LSE_DSQ_PCP_BASE           LSE_MAX_GLOBAL_DSQS

/* Default deadlines (ms) for each global DSQ */
#define LSE_DSQ_DEFAULT_DEADLINES \
    { 0, 1, 2, 4, 6, 8, 16, 32, 64, 128 }

/* Default time quotas for non-period DSQs (ns) — indices 0-4 unused */
#define LSE_DSQ_DEFAULT_QUOTAS \
    { 0, 0, 0, 0, 0, \
      32UL * NSEC_PER_MSEC,    /* [5] 32ms */ \
      20UL * NSEC_PER_MSEC,    /* [6] 20ms */ \
      14UL * NSEC_PER_MSEC,    /* [7] 14ms */ \
       8UL * NSEC_PER_MSEC,    /* [8]  8ms */ \
       6UL * NSEC_PER_MSEC }   /* [9]  6ms */

/* Per-CPU DSQ default deadline (ms) and quota (ns) */
#define LSE_PCP_DSQ_DEADLINE_MS  20
#define LSE_PCP_DSQ_QUOTA_NS     (3UL * NSEC_PER_MSEC)

/* DSQ dispatch flags */
#define LSE_DSQ_FLAG_BUILTIN  (1ULL << 63)
#define LSE_DSQ_LOCAL_ID      0xFFFFFFFFFFFFFFFEULL
#define LSE_DSQ_INVALID_ID    0xFFFFFFFFFFFFFFFFULL

/*
 * Task classification for DSQ mapping
 */
enum lse_dsq_task_prio {
    LSE_DSQ_PRIO_CRITICAL_SYSTEM = 0,   /* DSQ [0] */
    LSE_DSQ_PRIO_CRITICAL_APP    = 1,   /* DSQ [1] */
    LSE_DSQ_PRIO_RT              = 2,   /* DSQ [3] */
    LSE_DSQ_PRIO_ENHANCED        = 3,   /* DSQ [4] UX */
    LSE_DSQ_PRIO_FOREGROUND      = 6,   /* DSQ [6] default FG */
    LSE_DSQ_PRIO_NORMAL          = 8,   /* DSQ [8] */
    LSE_DSQ_PRIO_BACKGROUND      = 9,   /* DSQ [9] lowest */
};

/**
 * struct lse_dispatch_q — A single dispatch queue
 *
 * Each DSQ holds a FIFO of tasks awaiting dispatch plus an optional
 * priority-ordered rbtree (priq). Tasks in priq are preferred over fifo
 * (they have explicit vtrace ordering from hmbird-style scheduling).
 *
 * @lock:           Protects fifo, priq, nr, is_timeout, last_consume_at
 * @fifo:           FIFO-ordered task list
 * @priq:           Virtual-time-sorted priority queue (rb tree)
 * @nr:             Total number of tasks in this DSQ
 * @id:             Unique DSQ identifier
 * @last_consume_at: Jiffies when a task was last consumed from this DSQ
 * @is_timeout:     Set when the head task's runnable age exceeds deadline
 */
struct lse_dispatch_q {
    raw_spinlock_t      lock;
    struct list_head    fifo;
    struct rb_root_cached priq;
    int                 nr;
    u64                 id;
    unsigned long       last_consume_at;
    bool                is_timeout;
};

/**
 * struct lse_dsq_sched_info — Global DSQ round-robin state
 *
 * Tracks which non-period DSQ is the current active one for each cluster.
 *
 * @lock:       Protects curr_idx[] and rtime[]
 * @curr_idx:   Current DSQ index for BIG cluster [0] and LITTLE cluster [1]
 * @rtime:      Accumulated running time for each global DSQ (ns)
 */
struct lse_dsq_sched_info {
    spinlock_t  lock;
    int         curr_idx[LSE_DSQ_MAX_CLUSTER_TIDX];
    int         rtime[LSE_MAX_GLOBAL_DSQS];
};

/**
 * struct lse_pcp_dsq_info — Per-CPU DSQ runtime tracking
 *
 * @pcp_seq:   Monotonic sequence number (unused, reserved)
 * @rtime:     Accumulated running time for this CPU's PCP DSQ (ns)
 * @pcp_round: Whether this CPU's PCP DSQ is in round (has quota remaining)
 */
struct lse_pcp_dsq_info {
    s64     pcp_seq;
    int     rtime;
    bool    pcp_round;
};

/*
 * DSQ type classification
 */
enum lse_dsq_type {
    LSE_DSQ_TYPE_GLOBAL,
    LSE_DSQ_TYPE_PCP,
    LSE_DSQ_TYPE_OTHER,
};

/*
 * Cluster types (mirrors the sched_cluster model)
 */
enum lse_cluster_type {
    LSE_CLUSTER_BIG    = 0,
    LSE_CLUSTER_LITTLE = 1,
    LSE_CLUSTER_INVALID = -1,
};

/* ──── Global DSQ state (declared in lse_dsq.c) ──── */

extern u32                    lse_dsq_deadlines[LSE_MAX_GLOBAL_DSQS];
extern u32                    lse_pcp_dsq_deadline_ms;
extern struct lse_dispatch_q  lse_gdsqs[LSE_MAX_GLOBAL_DSQS];
extern struct lse_dsq_sched_info lse_dsq_sinfo;

/* ──── API ──── */

/* Init / teardown */
void lse_dsq_init_core(void);
void lse_dsq_sync(void);

/* Task classification → DSQ index */
int  lse_dsq_classify_task(struct task_struct *p);
bool lse_dsq_is_pcp_candidate(struct task_struct *p);

/* Enqueue / dequeue (called from schedule hooks) */
void lse_dsq_enqueue_task(struct task_struct *p, int cpu);
void lse_dsq_dequeue_task(struct task_struct *p, int cpu);

/* Monitoring / observability */
int  lse_dsq_depth_global(int idx);
int  lse_dsq_depth_pcp(int cpu);
int  lse_dsq_total_backlog(void);
bool lse_dsq_has_timeout(int idx);
void lse_dsq_scan_timeout(int cpu);

/* DSQ → util signal for cpufreq governor */
unsigned int lse_dsq_urgency_signal(int cpu);

/* DSQ diagnostic dump */
void lse_dsq_dump_state(void);

#endif /* _LSE_DSQ_H_ */
