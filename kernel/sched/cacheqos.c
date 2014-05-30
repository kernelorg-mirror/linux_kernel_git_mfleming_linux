#include <linux/cgroup.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/seq_file.h>
#include <linux/rcupdate.h>
#include <linux/kernel_stat.h>
#include <linux/err.h>
#include <linux/trace_clock.h>

#include "cacheqos.h"
#include "sched.h"

struct cacheqos root_cacheqos_group = { .monitor_cache = false };
static DEFINE_SPINLOCK(cacheqos_lock);

#if !defined(CONFIG_X86_64) || !defined(CONFIG_X86)
static int __init cacheqos_late_init(void)
{
	/* No Cache QoS support on this architecture, disable the subsystem */
	root_cacheqos_group.monitor_cache = false;
	root_cacheqos_group.css.ss->disabled = 1;
	return -ENODEV;
}
late_initcall(cacheqos_late_init);
#endif

inline void cacheqos_sched_out(struct task_struct *task)
{
	struct cacheqos *cq = task_cacheqos(task);
	u64 val;

	/*
	 * Assumption is that this thread is running on the logical processor
	 * from which the task is being scheduled out.
	 *
	 * As the task is scheduled out mapping goes back to default map.
	 */
	if (cq->monitor_cache)
		cacheqos_map_schedule_out(cq);

	/*
	 * Beyond this point, we should be using RMID 0.
	 */
	rdmsrl(0xc8f, val);
	WARN_ON(val & 0x3ff);
}

inline void cacheqos_sched_in(struct task_struct *task)
{
	struct cacheqos *cq = task_cacheqos(task);
	/*
	 * Assumption is that this thread is running on the logical processor
	 * of which this task is being scheduled onto.
	 *
	 * As the task is scheduled in, the cgroup's rmid is loaded
	 */
	if (cq->monitor_cache)
		cacheqos_map_schedule_in(cq);
	else {
		u64 val;

		rdmsrl(0xc8f, val);
		WARN_ON(val & 0x3ff);
	}
}

static int cacheqos_move_rmid_to_unused_list(struct cacheqos *cq, int rmid)
{
	struct rmid_list_element *elem;

	/*
	 * Assumes only called when cq->rmid is valid (ie, it is on the
	 * inuse list) and cacheqos_lock is held.
	 */
	lockdep_assert_held(&cacheqos_lock);
	list_for_each_entry(elem, &cq->subsys_info->rmid_inuse_list, list) {
		int cpu;

		if (rmid == elem->rmid) {
			/* Move rmid from inuse to unused list */
			list_del_init(&elem->list);
			list_add_tail(&elem->list,
				      &cq->subsys_info->rmid_unused_fifo);

			/* Update statistics */
			cpu = get_cpu();
			elem->free_clock = trace_clock_global();
			elem->free_val = __cacheqos_read(elem->rmid);
			elem->phys_id = topology_physical_package_id(cpu);
			put_cpu();

			goto quick_exit;
		}
	}
	return -ELIBBAD;

quick_exit:
	return 0;
}

int cacheqos_deallocate_rmid(struct cacheqos *cq, int rmid)
{
	unsigned long flags;
	int err = 0;

	/*
	 * We must never deallocate RMID 0.
	 *
	 * This actually occurs because of a race condition with
	 * monitor_cache. Because userland can update monitor_cache in
	 * the middle of a task's execution it may end up calling
	 * cacheqos_dellocate_rmid() despite having never allocated an
	 * RMID.
	 */
	if (!rmid)
		return 0;

	spin_lock_irqsave(&cacheqos_lock, flags);

	err = cacheqos_move_rmid_to_unused_list(cq, rmid);
	WARN_ON(err);

	spin_unlock_irqrestore(&cacheqos_lock, flags);
	return err;
}

/*
 * This function returns the "coldest" RMID. By coldest, we mean the
 * RMID with the smallest associated value in IA32_QM_CTR. This is
 * likely to be the least recently used RMID, but not necessarily.
 * get_rmid() should be called before trying this slowpath.
 *
 * This function *WILL* return an RMID since the caller of this function
 * should have checked that there exists an RMID item on the free list.
 *
 * We expect to be called with cacheqos_lock held.
 */
static struct rmid_list_element *get_rmid_slowpath(struct cacheqos *cq)
{
	struct rmid_list_element *elem;
	struct list_head *item;

	WARN_ON(1);

	/* XXX: we don't do anything special yet */
	item = cq->subsys_info->rmid_unused_fifo.next;
	elem = list_entry(item, struct rmid_list_element, list);

	list_del_init(&elem->list);
	list_add_tail(&elem->list, &cq->subsys_info->rmid_inuse_list);

	return elem;
}

/*
 * Grab the least-recently-used RMID.
 *
 * If the LRU RMID doesn't have a zero occupancy value, we return NULL
 * and the caller should execute get_rmid_slowpath().
 *
 * This function *WILL* return an RMID since the caller of this function
 * should have checked that there exists an RMID item on the free list.
 *
 * We expect to be called with cacheqos_lock held.
 */
static struct rmid_list_element *get_rmid(struct cacheqos *cq)
{
	struct rmid_list_element *elem;
	struct list_head *item;
	u64 result;
	int cpu;

	lockdep_assert_held(&cacheqos_lock);

	/* Move rmid from unused to inuse list */
	item = cq->subsys_info->rmid_unused_fifo.next;
	elem = list_entry(item, struct rmid_list_element, list);

	cpu = get_cpu();
	result = __cacheqos_read(elem->rmid);

	/*
	 * If the counter increased after we moved the RMID to the free
	 * list (and we're on the same package), we've hit a race
	 * condition.
	 */
	if (topology_physical_package_id(cpu) == elem->phys_id) {
		if (result != -1 && result > elem->free_val)
			WARN_ON(1);
	}

	put_cpu();

	/*
	 * This can be fine-tuned if perhaps we can stand a non-zero
	 * occupancy value as long as it's below some threshold.
	 */
	if (result)
		return NULL;

	list_del_init(&elem->list);
	list_add_tail(&elem->list, &cq->subsys_info->rmid_inuse_list);

	return elem;
}

/*
 * Returns a positive non-zero rmid on success. 0 on failure since 0 is
 * always "allocated" for the root cgroup.
 */
int cacheqos_allocate_rmid(struct cacheqos *cq)
{
	struct rmid_list_element *elem;
	unsigned long flags;

	spin_lock_irqsave(&cacheqos_lock, flags);

	/*
	 * Having absolutely no rmids is a hard fail.
	 */
	if (list_empty(&cq->subsys_info->rmid_unused_fifo)) {
		spin_unlock_irqrestore(&cacheqos_lock, flags);
		return 0;
	}

	elem = get_rmid(cq);
	if (!elem)
		elem = get_rmid_slowpath(cq);

	spin_unlock_irqrestore(&cacheqos_lock, flags);

	return elem->rmid;
}

/* create a new cacheqos cgroup */
static struct cgroup_subsys_state *
cacheqos_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct cacheqos *parent = css_cacheqos(parent_css);
	struct cacheqos *cq;

	if (!parent) {
		/* cacheqos_late_init() will enable monitoring on the root */
		return &root_cacheqos_group.css;
	}

	cq = kzalloc(sizeof(struct cacheqos), GFP_KERNEL);
	if (!cq)
		goto out;

	cq->cgrp = parent_css->cgroup;
	cq->monitor_cache = false;	/* disabled i.e., use parent's RMID */
	cq->subsys_info = root_cacheqos_group.subsys_info;
	return &cq->css;

out:
	return ERR_PTR(-ENOMEM);
}

/* destroy an existing cacheqos task group */
static void cacheqos_css_free(struct cgroup_subsys_state *css)
{
	struct cacheqos *cq = css_cacheqos(css);
	kfree(cq);
}

/* return task group's monitoring state */
static u64 cacheqos_monitor_read(struct cgroup_subsys_state *css,
				 struct cftype *cft)
{
	struct cacheqos *cq = css_cacheqos(css);

	return cq->monitor_cache;
}

/* set the task group's monitoring state */
static int cacheqos_monitor_write(struct cgroup_subsys_state *css,
				  struct cftype *cftype, u64 enable)
{
	struct cacheqos *cq = css_cacheqos(css);

	if (enable != 0 && enable != 1)
		return -EINVAL;

	cq->monitor_cache = !!enable;
	return 0;
}

/* return total system LLC occupancy in bytes of a task group */
static int cacheqos_occupancy_read(struct seq_file *m, void *v)
{
	struct cacheqos *cq = css_cacheqos(seq_css(m));
	u64 total_occupancy = 0;
	int node;

	spin_lock_irq(&cacheqos_lock);
	for_each_node_with_cpus(node)
		total_occupancy += cq->subsys_info->node_results[node];

	spin_unlock_irq(&cacheqos_lock);

	seq_printf(m, "%llu\n", total_occupancy);
	return 0;
}

/* return display each LLC's occupancy in bytes of a task group */
static int
cacheqos_occupancy_persocket_seq_read(struct seq_file *m, void *v)
{
	struct cacheqos *cq = css_cacheqos(seq_css(m));
	int node;

	spin_lock_irq(&cacheqos_lock);
	for_each_node_with_cpus(node) {
		seq_printf(m, "%llu\n",
			   cq->subsys_info->node_results[node]);
	}

	spin_unlock_irq(&cacheqos_lock);

	return 0;
}

/* return total system LLC occupancy as a %of system LLC for the task group */
static int cacheqos_occupancy_percent_read(struct seq_file *m, void *v)
{
	struct cacheqos *cq = css_cacheqos(seq_css(m));
	u64 total_occupancy = 0;
	int node;
	int node_cnt = 0;
	int parts_of_100, parts_of_10000;
	int cache_size;

	spin_lock_irq(&cacheqos_lock);
	for_each_node_with_cpus(node) {
		++node_cnt;
		total_occupancy += cq->subsys_info->node_results[node];
	}

	spin_unlock_irq(&cacheqos_lock);

	cache_size = cq->subsys_info->cache_size * node_cnt;
	parts_of_100 = (total_occupancy * 100) / (cache_size * 1024);
	parts_of_10000 = (total_occupancy * 10000) / (cache_size * 1024) -
				parts_of_100 * 100;
	seq_printf(m, "%d.%02d\n", parts_of_100, parts_of_10000);

	return 0;
}

/* return display each LLC's % occupancy of the socket's LLC for task group */
static int
cacheqos_occupancy_percent_persocket_seq_read(struct seq_file *m, void *v)
{
	struct cacheqos *cq = css_cacheqos(seq_css(m));
	u64 total_occupancy;
	int node;
	int cache_size;
	int parts_of_100, parts_of_10000;

	spin_lock_irq(&cacheqos_lock);
	cache_size = cq->subsys_info->cache_size;
	for_each_node_with_cpus(node) {
		total_occupancy = cq->subsys_info->node_results[node];
		parts_of_100 = (total_occupancy * 100) / (cache_size * 1024);
		parts_of_10000 = (total_occupancy * 10000) /
				 (cache_size * 1024) - parts_of_100 * 100;

		seq_printf(m, "%d.%02d\n", parts_of_100, parts_of_10000);
	}

	spin_unlock_irq(&cacheqos_lock);

	return 0;
}

static struct cftype cacheqos_files[] = {
	{
		.name = "monitor_cache",
		.read_u64 = cacheqos_monitor_read,
		.write_u64 = cacheqos_monitor_write,
		.mode = 0666,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "occupancy_persocket",
		.seq_show = cacheqos_occupancy_persocket_seq_read,
	},
	{
		.name = "occupancy",
		.seq_show = cacheqos_occupancy_read,
	},
	{
		.name = "occupancy_percent_persocket",
		.seq_show = cacheqos_occupancy_percent_persocket_seq_read,
	},
	{
		.name = "occupancy_percent",
		.seq_show = cacheqos_occupancy_percent_read,
	},
	{ }	/* terminate */
};

struct cgroup_subsys cacheqos_subsys = {
	.name			= "cacheqos",
	.css_alloc		= cacheqos_css_alloc,
	.css_free		= cacheqos_css_free,
	.subsys_id		= cacheqos_subsys_id,
	.base_cftypes		= cacheqos_files,
};
