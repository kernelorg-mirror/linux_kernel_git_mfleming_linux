/*
 * Platform Quality-of-Service (QoS) Monitoring.
 *
 * Based very, very heavily on work by Peter Zijlstra.
 */

#include <linux/perf_event.h>
#include <linux/slab.h>
#include "perf_event.h"

#define MSR_IA32_PQR_ASSOC	0x0c8f
#define MSR_IA32_QM_CTR		0x0c8e
#define MSR_IA32_QM_EVTSEL	0x0c8d

static unsigned int qos_max_rmid = -1;
static unsigned int qos_l3_scale; /* supposedly cacheline size */

struct intel_qos_state {
	raw_spinlock_t		lock;
	int			rmid;
	int 			cnt;
};

static DEFINE_PER_CPU(struct intel_qos_state, qos_state);

/*
 * Protects the global state, hold both for modification, hold either for
 * stability.
 *
 * XXX we modify RMID with only cache_mutex held, racy!
 */
static DEFINE_MUTEX(cache_mutex);
static DEFINE_RAW_SPINLOCK(cache_lock);

static unsigned long *qos_rmid_bitmap;

/*
 * All events
 */
static LIST_HEAD(cache_events);

/*
 * Groups of events that have the same target(s), one RMID per group.
 */
static LIST_HEAD(cache_groups);

/*
 * The new RMID we must not use until intel_qos_stable().
 * See intel_qos_rotate().
 */
static unsigned long *cache_limbo_bitmap;

/*
 * The spare RMID that make rotation possible; keep out of the
 * qos_rmid_bitmap to avoid it getting used for new events.
 */
static int cache_rotation_rmid;

/*
 * The freed RMIDs, see intel_qos_rotate().
 */
static int cache_freed_nr;
static int *cache_freed_rmid;

/*
 * One online cpu per package, for intel_qos_stable().
 */
static cpumask_t cache_cpus;

/*
 * Returns < 0 on fail.
 */
static int __get_rmid(void)
{
	return bitmap_find_free_region(qos_rmid_bitmap, qos_max_rmid, 0);
}

static void __put_rmid(int rmid)
{
	bitmap_release_region(qos_rmid_bitmap, rmid, 0);
}

/*
 * Needs a quesent state before __put, see intel_qos_stabilize().
 */
static void __free_rmid(int rmid)
{
	cache_freed_rmid[cache_freed_nr++] = rmid;
}

#define RMID_VAL_ERROR		(1ULL << 63)
#define RMID_VAL_UNAVAIL	(1ULL << 62)

#define QOS_L3_OCCUP_EVENT_ID	(1 << 0)

#define QOS_EVENT_MASK	QOS_L3_OCCUP_EVENT_ID

static u64 __rmid_read(unsigned long rmid)
{
	u64 val;

	/*
	 * Ignore the SDM, this thing is _NOTHING_ like a regular perfcnt,
	 * it just says that to increase confusion.
	 */
	wrmsr(MSR_IA32_QM_EVTSEL, QOS_L3_OCCUP_EVENT_ID, rmid);
	rdmsrl(MSR_IA32_QM_CTR, val);

	/*
	 * Aside from the ERROR and UNAVAIL bits, assume this thing returns
	 * the number of cachelines tagged with @rmid.
	 */
	return val;
}

/*
 * Check whether the corresponding value for the RMID is non-zero, which
 * indicates that this RMID is in use.
 */
static void smp_test_stable(void *info)
{
	bool *inuse = info;
	int i;

	for (i = 0; i < cache_freed_nr; i++) {
		if (__rmid_read(cache_freed_rmid[i]))
			*inuse = true;
	}
}

/*
 * Test if the rotation_rmid is unused; see the comment near
 * intel_qos_rotate().
 */
static bool intel_qos_is_stable(void)
{
	bool inuse = false;

	smp_call_function_many(&cache_cpus, smp_test_stable, &inuse, true);

	return !inuse;
}

/*
 * Quescent state; wait for all the 'freed' RMIDs to become unused.
 * After this we can can reuse them and know that the current set of
 * active RMIDs is stable.
 */
static void intel_qos_stabilize(void)
{
	int i = 0;

	if (!cache_freed_nr)
		return;

	/*
	 * Now wait until the old RMID drops back to 0 again, this means all
	 * cachelines have acquired a new tag and the new RMID is now stable.
	 */
	while (!intel_qos_is_stable()) {
		/*
		 * XXX adaptive timeout? Ideally the hardware would get us an
		 * interrupt :/
		 */
		schedule_timeout_uninterruptible(1);
	}

	bitmap_clear(cache_limbo_bitmap, 0, qos_max_rmid);

	if (cache_rotation_rmid <= 0) {
		cache_rotation_rmid = cache_freed_rmid[0];
		i++;
	}

	for (; i < cache_freed_nr; i++)
		__put_rmid(cache_freed_rmid[i]);

	cache_freed_nr = 0;
}

/*
 * Exchange the RMID of a group of events.
 */
static unsigned long
cache_group_xchg_rmid(struct perf_event *group, unsigned long rmid)
{
	struct perf_event *event;
	unsigned long old_rmid = group->hw.qos_rmid;
	struct hw_perf_event *hw = &group->hw;

	hw->qos_rmid = rmid;
	list_for_each_entry(event, &hw->qos_group_entry, hw.qos_group_entry)
		event->hw.qos_rmid = rmid;

	return old_rmid;
}

/*
 * Determine if @a and @b measure the same set of tasks.
 */
static bool __match_event(struct perf_event *a, struct perf_event *b)
{
	if ((a->attach_state & PERF_ATTACH_TASK) !=
	    (b->attach_state & PERF_ATTACH_TASK))
		return false;

	if (a->attach_state & PERF_ATTACH_TASK) {
		if (a->hw.qos_target != b->hw.qos_target)
			return false;

		return true;
	}

	/* not task */

#ifdef CONFIG_CGROUP_PERF
	if ((a->cgrp == b->cgrp) && a->cgrp)
		return true;
#endif

	return true; /* if not task or cgroup, we're machine wide */
}

#ifdef CONFIG_CGROUP_PERF
static struct perf_cgroup *event_to_cgroup(struct perf_event *event)
{
	if (event->cgrp)
		return event->cgrp;

	if (event->attach_state & PERF_ATTACH_TASK) /* XXX */
		return perf_cgroup_from_task(event->hw.qos_target);

	return NULL;
}
#endif

/*
 * Determine if @na's tasks intersect with @b's tasks
 */
static bool __conflict_event(struct perf_event *a, struct perf_event *b)
{
#ifdef CONFIG_CGROUP_PERF
	struct perf_cgroup *ac, *bc;

	ac = event_to_cgroup(a);
	bc = event_to_cgroup(b);

	if (!ac || !bc) {
		/*
		 * If either is NULL, its a system wide event and that
		 * always conflicts with a cgroup one.
		 *
		 * If both are system wide, __match_event() should've
		 * been true and we'll never get here, if we did fail.
		 */
		return true;
	}

	/*
	 * If one is a parent of the other, we've got an intersection.
	 */
	if (cgroup_is_descendant(ac->css.cgroup, bc->css.cgroup) ||
	    cgroup_is_descendant(bc->css.cgroup, ac->css.cgroup))
		return true;
#endif

	/*
	 * If one of them is not a task, same story as above with cgroups.
	 */
	if (!(a->attach_state & PERF_ATTACH_TASK) ||
	    !(b->attach_state & PERF_ATTACH_TASK))
		return true;

	/*
	 * Again, if they're the same __match_event() should've caught us, if not fail.
	 */
	if (a->hw.qos_target == b->hw.qos_target)
		return true;

	/*
	 * Must be non-overlapping.
	 */
	return false;
}

/*
 * Attempt to rotate the groups and assign new RMIDs, ought to run from
 * an delayed work or somesuch.
 *
 * Rotating RMIDs is complicated; firstly because the hardware doesn't
 * give us any clues; secondly because of cgroups.
 *
 * There's problems with the hardware interface; when you change the
 * task:RMID map cachelines retain their 'old' tags, giving a skewed
 * picture. In order to work around this, we must always keep one free
 * RMID.
 *
 * Rotation works by taking away an RMID from a group (the old RMID),
 * and assigning the free RMID to another group (the new RMID). We must
 * then wait for the old RMID to not be used (no cachelines tagged).
 * This ensure that all cachelines are tagged with 'active' RMIDs. At
 * this point we can start reading values for the new RMID and treat the
 * old RMID as the free RMID for the next rotation.
 *
 * Secondly, since cgroups can nest, we must make sure to not program
 * conflicting cgroups at the same time. A conflicting cgroup is one
 * that has a parent<->child relation. After all, a task of the child
 * cgroup will also be covered by the parent cgroup.
 *
 * Therefore, when selecting a new group, we must invalidate all
 * conflicting groups. Rotations allows us to measure all (conflicting)
 * groups sequentially.
 *
 * XXX there's a further problem in that because we do our own rotation
 * and cheat with schedulability the event {enabled,running} times are
 * incorrect.
 */
static bool intel_qos_rotate(void)
{
	struct perf_event *rotor, *group;
	int rmid;

	mutex_lock(&cache_mutex);

	if (list_empty(&cache_groups))
		goto unlock_mutex;

	rotor = list_first_entry(&cache_groups, struct perf_event, hw.qos_groups_entry);

	raw_spin_lock_irq(&cache_lock);
	list_del(&rotor->hw.qos_groups_entry);
	rmid = cache_group_xchg_rmid(rotor, -1);
	WARN_ON_ONCE(rmid <= 0); /* first entry must always have an RMID */
	__free_rmid(rmid);
	raw_spin_unlock_irq(&cache_lock);

	/*
	 * XXX O(n^2) schedulability
	 */

	list_for_each_entry(group, &cache_groups, hw.qos_groups_entry) {
		bool conflicts = false;
		struct perf_event *iter;

		list_for_each_entry(iter, &cache_groups, hw.qos_groups_entry) {
			if (iter == group)
				break;
			if (__conflict_event(group, iter)) {
				conflicts = true;
				break;
			}
		}

		if (conflicts && group->hw.qos_rmid > 0) {
			rmid = cache_group_xchg_rmid(group, -1);
			WARN_ON_ONCE(rmid <= 0);
			__free_rmid(rmid);
			continue;
		}

		if (!conflicts && group->hw.qos_rmid <= 0) {
			rmid = __get_rmid();
			if (rmid <= 0) {
				rmid = cache_rotation_rmid;
				cache_rotation_rmid = -1;
			}
			set_bit(rmid, cache_limbo_bitmap);
			if (rmid <= 0)
				break; /* we're out of RMIDs, more next time */

			rmid = cache_group_xchg_rmid(group, rmid);
			WARN_ON_ONCE(rmid > 0);
			continue;
		}

		/*
		 * either we conflict and do not have an RMID -> good,
		 * or we do not conflict and have an RMID -> also good.
		 */
	}

	raw_spin_lock_irq(&cache_lock);
	list_add_tail(&rotor->hw.qos_groups_entry, &cache_groups);
	raw_spin_unlock_irq(&cache_lock);

	/*
	 * XXX force a PMU reprogram here such that the new RMIDs are in
	 * effect.
	 */

	intel_qos_stabilize();

unlock_mutex:
	mutex_unlock(&cache_mutex);

	/*
	 * XXX reschedule work.
	 */
	return false;
}

/*
 * Find a group and setup RMID
 */
static struct perf_event *intel_qos_setup_event(struct perf_event *event)
{
	struct perf_event *iter;
	int rmid = 0; /* unset */

	list_for_each_entry(iter, &cache_groups, hw.qos_groups_entry) {
		if (__match_event(iter, event)) {
			event->hw.qos_rmid = iter->hw.qos_rmid;
			return iter;
		}
		if (__conflict_event(iter, event))
			rmid = -1; /* conflicting rmid */
	}

	if (!rmid) {
		/* XXX lacks stabilization */
		event->hw.qos_rmid = __get_rmid();
	}

	return NULL;
}

static void intel_qos_event_read(struct perf_event *event)
{
	unsigned long rmid = event->hw.qos_rmid;
	u64 val = RMID_VAL_UNAVAIL;

	if (!test_bit(rmid, cache_limbo_bitmap))
		val = __rmid_read(rmid);

	/*
	 * Ignore this reading on error states and do not update the value.
	 */
	if (val & (RMID_VAL_ERROR | RMID_VAL_UNAVAIL))
		return;

	val *= qos_l3_scale; /* cachelines -> bytes */

	local64_set(&event->count, val);
}

static void intel_qos_event_start(struct perf_event *event, int mode)
{
	struct intel_qos_state *state = &__get_cpu_var(qos_state);
	unsigned long rmid = event->hw.qos_rmid;
	unsigned long flags;

	if (!(event->hw.qos_state & PERF_HES_STOPPED))
		return;

	event->hw.qos_state &= ~PERF_HES_STOPPED;

	raw_spin_lock_irqsave(&state->lock, flags);
	if (state->cnt++)
		WARN_ON_ONCE(state->rmid != rmid);
	else
		WARN_ON_ONCE(state->rmid);
	state->rmid = rmid;
	wrmsrl(MSR_IA32_PQR_ASSOC, state->rmid);
	raw_spin_unlock_irqrestore(&state->lock, flags);
}

static void intel_qos_event_stop(struct perf_event *event, int mode)
{
	struct intel_qos_state *state = &__get_cpu_var(qos_state);
	unsigned long flags;

	if (event->hw.qos_state & PERF_HES_STOPPED)
		return;

	event->hw.qos_state |= PERF_HES_STOPPED;

	raw_spin_lock_irqsave(&state->lock, flags);
	intel_qos_event_read(event);

	if (!--state->cnt) {
		state->rmid = 0;
		wrmsrl(MSR_IA32_PQR_ASSOC, 0);
	} else {
		WARN_ON_ONCE(!state->rmid);
	}

	raw_spin_unlock_irqrestore(&state->lock, flags);
}

static int intel_qos_event_add(struct perf_event *event, int mode)
{
	unsigned long flags;
	int rmid;

	raw_spin_lock_irqsave(&cache_lock, flags);

	event->hw.qos_state = PERF_HES_STOPPED;
	rmid = event->hw.qos_rmid;
	if (rmid <= 0)
		goto unlock;

	if (mode & PERF_EF_START)
		intel_qos_event_start(event, mode);

unlock:
	raw_spin_unlock_irqrestore(&cache_lock, flags);

	return 0;
}

static void intel_qos_event_del(struct perf_event *event, int mode)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&cache_lock, flags);
	intel_qos_event_stop(event, mode);
	raw_spin_unlock_irqrestore(&cache_lock, flags);
}

static void intel_qos_event_destroy(struct perf_event *event)
{
	struct perf_event *group_other = NULL;

	mutex_lock(&cache_mutex);
	raw_spin_lock_irq(&cache_lock);

	list_del(&event->hw.qos_events_entry);

	/*
	 * If there's another event in this group...
	 */
	if (!list_empty(&event->hw.qos_group_entry)) {
		group_other = list_first_entry(&event->hw.qos_group_entry,
					       struct perf_event,
					       hw.qos_group_entry);
		list_del(&event->hw.qos_group_entry);
	}
	/*
	 * And we're the group leader..
	 */
	if (!list_empty(&event->hw.qos_groups_entry)) {
		/*
		 * If there was a group_other, make that leader, otherwise
		 * destroy the group and return the RMID.
		 */
		if (group_other) {
			list_replace(&event->hw.qos_groups_entry,
				     &group_other->hw.qos_groups_entry);
		} else {
			int rmid = event->hw.qos_rmid;
			if (rmid > 0)
				__put_rmid(rmid);
			list_del(&event->hw.qos_groups_entry);
		}
	}

	raw_spin_unlock_irq(&cache_lock);
	mutex_unlock(&cache_mutex);
}

static struct pmu intel_qos_pmu;

/*
 * Takes non-sampling task,cgroup or machine wide events.
 *
 * XXX there's a bit of a problem in that we cannot simply do the one
 * event per node as one would want, since that one event would one get
 * scheduled on the one cpu. But we want to 'schedule' the RMID on all
 * CPUs.
 *
 * This means we want events for each CPU, however, that generates a lot
 * of duplicate values out to userspace -- this is not to be helped
 * unless we want to change the core code in some way.
 */
static int intel_qos_event_init(struct perf_event *event)
{
	struct perf_event *group;

	if (event->attr.type != intel_qos_pmu.type)
		return -ENOENT;

	if (event->attr.config & ~QOS_EVENT_MASK)
		return -EINVAL;

	if (event->cpu == -1) /* must have per-cpu events; see above */
		return -EINVAL;

	/* unsupported modes and filters */
	if (event->attr.exclude_user   ||
	    event->attr.exclude_kernel ||
	    event->attr.exclude_hv     ||
	    event->attr.exclude_idle   ||
	    event->attr.exclude_host   ||
	    event->attr.exclude_guest  ||
	    event->attr.sample_period) /* no sampling */
		return -EINVAL;

	event->destroy = intel_qos_event_destroy;

	mutex_lock(&cache_mutex);

	INIT_LIST_HEAD(&event->hw.qos_group_entry);
	group = intel_qos_setup_event(event); /* will also set rmid */

	raw_spin_lock_irq(&cache_lock);
	if (group) {
		event->hw.qos_rmid = group->hw.qos_rmid;
		list_add_tail(&event->hw.qos_group_entry,
			      &group->hw.qos_group_entry);
	} else {
		list_add_tail(&event->hw.qos_groups_entry,
			      &cache_groups);
	}

	list_add_tail(&event->hw.qos_events_entry, &cache_events);
	raw_spin_unlock_irq(&cache_lock);

	mutex_unlock(&cache_mutex);

	return 0;
}

EVENT_ATTR_STR(cache_occupancy, intel_qos_cache, "event=0x01");

static struct attribute *intel_qos_events_attr[] = {
	EVENT_PTR(intel_qos_cache),
	NULL,
};

static struct attribute_group intel_qos_events_group = {
	.name = "events",
	.attrs = intel_qos_events_attr,
};

PMU_FORMAT_ATTR(event, "config:0-7");
static struct attribute *intel_qos_formats_attr[] = {
	&format_attr_event.attr,
	NULL,
};

static struct attribute_group intel_qos_format_group = {
	.name = "format",
	.attrs = intel_qos_formats_attr,
};

const struct attribute_group *intel_qos_attr_groups[] = {
	&intel_qos_events_group,
	&intel_qos_format_group,
	NULL,
};

static struct pmu intel_qos_pmu = {
	.attr_groups	= intel_qos_attr_groups,
	.task_ctx_nr	= perf_sw_context,
	.event_init	= intel_qos_event_init,
	.add		= intel_qos_event_add,
	.del		= intel_qos_event_del,
	.start		= intel_qos_event_start,
	.stop		= intel_qos_event_stop,
	.read		= intel_qos_event_read,
};

static int __init intel_qos_init(void)
{
	int i, cpu, ret;

	if (!cpu_has(&boot_cpu_data, X86_FEATURE_CQM_OCCUP_LLC)) {
		pr_info("Intel QoS not supported\n");
		return -ENODEV;
	}

	qos_l3_scale = boot_cpu_data.x86_cache_occ_scale;

	/*
	 * It's possible that not all resources support the same number
	 * of RMIDs. Instead of making scheduling much more complicated
	 * (where we have to match a task's RMID to a cpu that supports
	 * that many RMIDs) just find the minimum RMIDs supported across
	 * all cpus.
	 *
	 * Also, check that the scales match on all cpus.
	 */
	for_each_online_cpu(cpu) {
		struct cpuinfo_x86 *c = &cpu_data(cpu);

		if (c->x86_cache_max_rmid < qos_max_rmid)
			qos_max_rmid = c->x86_cache_max_rmid;

		if (c->x86_cache_occ_scale != qos_l3_scale) {
			pr_err("Multiple LLC scale values, disabling\n");
			return -EINVAL;
		}
	}

	qos_rmid_bitmap = kmalloc(sizeof(long) * BITS_TO_LONGS(qos_max_rmid), GFP_KERNEL);
	if (!qos_rmid_bitmap)
		return -ENOMEM;

	cache_limbo_bitmap = kmalloc(sizeof(long) * BITS_TO_LONGS(qos_max_rmid), GFP_KERNEL);
	if (!cache_limbo_bitmap)
		return -ENOMEM; /* XXX frees */

	cache_freed_rmid = kmalloc(sizeof(int) * qos_max_rmid, GFP_KERNEL);
	if (!cache_freed_rmid)
		return -ENOMEM; /* XXX free bitmaps */

	bitmap_zero(qos_rmid_bitmap, qos_max_rmid);
	bitmap_set(qos_rmid_bitmap, 0, 1); /* RMID 0 is special */
	cache_rotation_rmid = __get_rmid(); /* keep one free RMID for rotation */
	if (WARN_ON_ONCE(cache_rotation_rmid < 0))
		return cache_rotation_rmid;

	/*
	 * XXX hotplug notifiers!
	 */
	for_each_possible_cpu(i) {
		struct intel_qos_state *state = &per_cpu(qos_state, i);

		raw_spin_lock_init(&state->lock);
		state->rmid = 0;
	}

	ret = perf_pmu_register(&intel_qos_pmu, "intel_qos", -1);
	if (ret)
		pr_err("Intel QoS perf registration failed: %d\n", ret);
	else
		pr_info("Intel QoS monitoring enabled\n");

	return ret;
}
device_initcall(intel_qos_init);
