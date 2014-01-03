#ifndef _CACHEQOS_H_
#define _CACHEQOS_H_
#ifdef CONFIG_CGROUP_CACHEQOS

#include <linux/cgroup.h>

struct rmid_list_element {
	int rmid;
	struct list_head list;
};

struct cacheqos_subsys_info {
	struct list_head rmid_unused_fifo;
	struct list_head rmid_inuse_list;
	int cache_max_rmid;
	int cache_occ_scale;
	int cache_size;
	u64 node_results[MAX_NUMNODES];
};

struct cacheqos {
	struct cgroup_subsys_state css;
	struct cacheqos_subsys_info *subsys_info;
	struct cgroup *cgrp;
	bool monitor_cache; /* false - use parent RMID / true - new RMID */

	/*
	 * Used for walking the task groups to update RMID's of the various
	 * sub-groups.  If monitor_cache is false, the sub-groups will inherit
	 * the parent's RMID.  If monitor_cache is true, then the group has its
	 * own RMID.
	 */
	spinlock_t lock;
	u32 rmid;
};

extern void cacheqos_map_schedule_out(void);
extern void cacheqos_map_schedule_in(struct cacheqos *);
extern void cacheqos_read(void *);

/* return cacheqos group corresponding to this container */
static inline struct cacheqos *css_cacheqos(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct cacheqos, css) : NULL;
}

/* return cacheqos group to which this task belongs */
static inline struct cacheqos *task_cacheqos(struct task_struct *task)
{
	return css_cacheqos(task_css(task, cacheqos_subsys_id));
}

static inline struct cacheqos *parent_cacheqos(struct cacheqos *cacheqos)
{
	return css_cacheqos(css_parent(&cacheqos->css));
}

#endif /* CONFIG_CGROUP_CACHEQOS */
#endif /* _CACHEQOS_H_ */
