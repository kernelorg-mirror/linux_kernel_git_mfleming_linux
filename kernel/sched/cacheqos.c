#include <linux/cgroup.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/seq_file.h>
#include <linux/rcupdate.h>
#include <linux/kernel_stat.h>
#include <linux/err.h>

#include "cacheqos.h"
#include "sched.h"

struct cacheqos root_cacheqos_group;
static DEFINE_MUTEX(cacheqos_mutex);

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
	/*
	 * Assumption is that this thread is running on the logical processor
	 * from which the task is being scheduled out.
	 *
	 * As the task is scheduled out mapping goes back to default map.
	 */
	if (cq->monitor_cache)
		cacheqos_map_schedule_out();
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
}

static void cacheqos_adjust_children_rmid(struct cacheqos *cq)
{
	struct cgroup_subsys_state *css, *pos;
	struct cacheqos *p_cq, *pos_cq;

	css = &cq->css;
	rcu_read_lock();

	css_for_each_descendant_pre(pos, css) {
		pos_cq = css_cacheqos(pos);
		if (!pos_cq->monitor_cache) {
			/* monitoring is disabled, so use the parent's RMID */
			p_cq = parent_cacheqos(pos_cq);
			spin_lock_irq(&pos_cq->lock);
			pos_cq->rmid = p_cq->rmid;
			spin_unlock_irq(&pos_cq->lock);
		}
	}
	rcu_read_unlock();
}

static int cacheqos_move_rmid_to_unused_list(struct cacheqos *cq)
{
	struct rmid_list_element *elem;

	/*
	 * Assumes only called when cq->rmid is valid (ie, it is on the
	 * inuse list) and cacheqos_mutex is held.
	 */
	lockdep_assert_held(&cacheqos_mutex);
	list_for_each_entry(elem, &cq->subsys_info->rmid_inuse_list, list) {
		if (cq->rmid == elem->rmid) {
			/* Move rmid from inuse to unused list */
			list_del_init(&elem->list);
			list_add_tail(&elem->list,
				      &cq->subsys_info->rmid_unused_fifo);
			goto quick_exit;
		}
	}
	return -ELIBBAD;

quick_exit:
	return 0;
}

static int cacheqos_deallocate_rmid(struct cacheqos *cq)
{
	struct cacheqos *cq_parent = parent_cacheqos(cq);
	int err;

	mutex_lock(&cacheqos_mutex);
	err = cacheqos_move_rmid_to_unused_list(cq);
	if (err)
		return err;
	/* assign parent's rmid to cgroup */
	cq->monitor_cache = false;
	cq->rmid = cq_parent->rmid;

	/* Check for children using this cgroup's rmid, iterate */
	cacheqos_adjust_children_rmid(cq);

	mutex_unlock(&cacheqos_mutex);
	return 0;
}

static int cacheqos_allocate_rmid(struct cacheqos *cq)
{
	struct rmid_list_element *elem;
	struct list_head *item;

	mutex_lock(&cacheqos_mutex);

	if (list_empty(&cq->subsys_info->rmid_unused_fifo)) {
		mutex_unlock(&cacheqos_mutex);
		return -EAGAIN;
	}

	/* Move rmid from unused to inuse list */
	item = cq->subsys_info->rmid_unused_fifo.next;
	list_del_init(item);
	list_add_tail(item, &cq->subsys_info->rmid_inuse_list);

	/* assign rmid to cgroup */
	elem = list_entry(item, struct rmid_list_element, list);
	cq->rmid = elem->rmid;
	cq->monitor_cache = true;

	/* Check for children using this cgroup's rmid, iterate */
	cacheqos_adjust_children_rmid(cq);

	mutex_unlock(&cacheqos_mutex);

	return 0;
}

/* create a new cacheqos cgroup */
static struct cgroup_subsys_state *
cacheqos_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct cacheqos *parent = css_cacheqos(parent_css);
	struct cacheqos *cq;

	if (!parent) {
		/* cacheqos_late_init() will enable monitoring on the root */
		root_cacheqos_group.rmid = 0;
		return &root_cacheqos_group.css;
	}

	cq = kzalloc(sizeof(struct cacheqos), GFP_KERNEL);
	if (!cq)
		goto out;

	cq->cgrp = parent_css->cgroup;
	cq->monitor_cache = false;	/* disabled i.e., use parent's RMID */
	cq->rmid = parent->rmid;	/* Start by using parent's RMID*/
	cq->subsys_info = root_cacheqos_group.subsys_info;
	return &cq->css;

out:
	return ERR_PTR(-ENOMEM);
}

/* destroy an existing cacheqos task group */
static void cacheqos_css_free(struct cgroup_subsys_state *css)
{
	struct cacheqos *cq = css_cacheqos(css);

	if (cq->monitor_cache) {
		mutex_lock(&cacheqos_mutex);
		cacheqos_move_rmid_to_unused_list(cq);
		mutex_unlock(&cacheqos_mutex);
	}
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
	int err = 0;

	if (enable != 0 && enable != 1) {
		err = -EINVAL;
		goto monitor_out;
	}

	if (enable && cq->monitor_cache)
		goto monitor_out;

	if (cq->monitor_cache)
		err = cacheqos_deallocate_rmid(cq);
	else
		err = cacheqos_allocate_rmid(cq);

monitor_out:
	return err;
}

static int cacheqos_get_occupancy_data(struct cacheqos *cq)
{
	unsigned int cpu;
	unsigned int node;
	const struct cpumask *node_cpus;
	int err = 0;

	/* Assumes cacheqos_mutex is held */
	lockdep_assert_held(&cacheqos_mutex);
	for_each_node_with_cpus(node) {
		node_cpus = cpumask_of_node(node);
		cpu = any_online_cpu(*node_cpus);
		err = smp_call_function_single(cpu, cacheqos_read, cq, 1);

		if (err) {
			break;
		} else if (cq->subsys_info->node_results[node] == -1) {
			err = -EPROTO;
			break;
		}
	}
	return err;
}

/* return total system LLC occupancy in bytes of a task group */
static int cacheqos_occupancy_read(struct seq_file *m, void *v)
{
	struct cacheqos *cq = css_cacheqos(seq_css(m));
	u64 total_occupancy = 0;
	int err, node;

	mutex_lock(&cacheqos_mutex);
	err = cacheqos_get_occupancy_data(cq);
	if (err) {
		mutex_unlock(&cacheqos_mutex);
		return err;
	}

	for_each_node_with_cpus(node)
		total_occupancy += cq->subsys_info->node_results[node];

	mutex_unlock(&cacheqos_mutex);

	seq_printf(m, "%llu\n", total_occupancy);
	return 0;
}

/* return display each LLC's occupancy in bytes of a task group */
static int
cacheqos_occupancy_persocket_seq_read(struct seq_file *m, void *v)
{
	struct cacheqos *cq = css_cacheqos(seq_css(m));
	int err, node;

	mutex_lock(&cacheqos_mutex);
	err = cacheqos_get_occupancy_data(cq);
	if (err) {
		mutex_unlock(&cacheqos_mutex);
		return err;
	}

	for_each_node_with_cpus(node) {
		seq_printf(m, "%llu\n",
			   cq->subsys_info->node_results[node]);
	}

	mutex_unlock(&cacheqos_mutex);

	return 0;
}

/* return total system LLC occupancy as a %of system LLC for the task group */
static int cacheqos_occupancy_percent_read(struct seq_file *m, void *v)
{
	struct cacheqos *cq = css_cacheqos(seq_css(m));
	u64 total_occupancy = 0;
	int err, node;
	int node_cnt = 0;
	int parts_of_100, parts_of_10000;
	int cache_size;

	mutex_lock(&cacheqos_mutex);
	err = cacheqos_get_occupancy_data(cq);
	if (err) {
		mutex_unlock(&cacheqos_mutex);
		return err;
	}

	for_each_node_with_cpus(node) {
		++node_cnt;
		total_occupancy += cq->subsys_info->node_results[node];
	}

	mutex_unlock(&cacheqos_mutex);

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
	int err, node;
	int cache_size;
	int parts_of_100, parts_of_10000;

	mutex_lock(&cacheqos_mutex);
	err = cacheqos_get_occupancy_data(cq);
	if (err) {
		mutex_unlock(&cacheqos_mutex);
		return err;
	}

	cache_size = cq->subsys_info->cache_size;
	for_each_node_with_cpus(node) {
		total_occupancy = cq->subsys_info->node_results[node];
		parts_of_100 = (total_occupancy * 100) / (cache_size * 1024);
		parts_of_10000 = (total_occupancy * 10000) /
				 (cache_size * 1024) - parts_of_100 * 100;

		seq_printf(m, "%d.%02d\n", parts_of_100, parts_of_10000);
	}

	mutex_unlock(&cacheqos_mutex);

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
