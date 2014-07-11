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

/*
 * All events
 */
static LIST_HEAD(cache_events);

/*
 * Groups of events that have the same target(s), one RMID per group.
 */
static LIST_HEAD(cache_groups);

/*
 * Mask of CPUs for reading QoS values. We only need one per-socket.
 *
 * NOTE: this is not the same as the intel uncore code. We still
 * enforce a system-wide event, e.g. an event on all cpus. It's just
 * that we want to use this cpumask for reading the perf values.
 */
static cpumask_t qos_cpumask;

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

struct qos_rmid_entry {
	u64 rmid;
	struct list_head list;
};

/*
 * A least recently used list of RMIDs.
 *
 * Oldest entry at the head, newest (most recently used) entry at the
 * tail. This list is never traversed, it's only used to keep track of
 * the lru order. That is, we only pick entries of the head or insert
 * them on the tail.
 *
 * All entries on the list are 'free', and their RMIDs are not currently
 * in use. To mark an RMID as in use, remove its entry from the lru
 * list.
 *
 * This list is protected by cache_mutex.
 */
static LIST_HEAD(qos_rmid_lru);

/*
 * We use a simple array of pointers so that we can lookup a struct
 * qos_rmid_entry in O(1). This alleviates the callers of __get_rmid()
 * and __put_rmid() from having to worry about dealing with struct
 * qos_rmid_entry - they just deal with rmids, i.e. integers.
 *
 * Once this array is initialized it is read-only. No locks are required
 * to access it.
 *
 * All entries for all RMIDs can be looked up in the this array at all
 * times.
 */
static struct qos_rmid_entry **qos_rmid_ptrs;

static inline struct qos_rmid_entry *__rmid_entry(int rmid)
{
	struct qos_rmid_entry *entry;

	entry = qos_rmid_ptrs[rmid];
	WARN_ON(entry->rmid != rmid);

	return entry;
}

/*
 * Returns < 0 on fail.
 *
 * We expect to be called with cache_mutex held.
 */
static int __get_rmid(void)
{
	struct qos_rmid_entry *entry;

	WARN_ON(!mutex_is_locked(&cache_mutex));

	if (list_empty(&qos_rmid_lru))
		return -EAGAIN;

	entry = list_first_entry(&qos_rmid_lru, struct qos_rmid_entry, list);
	list_del(&entry->list);

	return entry->rmid;
}

static void __put_rmid(int rmid)
{
	struct qos_rmid_entry *entry;

	WARN_ON(!mutex_is_locked(&cache_mutex));

	entry = __rmid_entry(rmid);

	list_add_tail(&entry->list, &qos_rmid_lru);
}

static int intel_qos_setup_rmid_cache(void)
{
	struct qos_rmid_entry *entry;
	int r;

	qos_rmid_ptrs = kmalloc(sizeof(struct qos_rmid_entry *) *
				(qos_max_rmid + 1), GFP_KERNEL);
	if (!qos_rmid_ptrs)
		return -ENOMEM;

	for (r = 0; r <= qos_max_rmid; r++) {
		struct qos_rmid_entry *entry;

		entry = kmalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry)
			goto fail;

		INIT_LIST_HEAD(&entry->list);
		entry->rmid = r;
		qos_rmid_ptrs[r] = entry;

		list_add_tail(&entry->list, &qos_rmid_lru);
	}

	/*
	 * RMID 0 is special and is always allocated. It's used for all
	 * tasks that are not monitored.
	 */
	entry = __rmid_entry(0);
	list_del(&entry->list);

	return 0;
fail:
	while (r--)
		kfree(qos_rmid_ptrs[r]);

	kfree(qos_rmid_ptrs);
	return -ENOMEM;
}

/*
 * Determine if @a and @b measure the same set of tasks.
 */
static bool __match_event(struct perf_event *a, struct perf_event *b)
{
	if ((a->attach_state & PERF_ATTACH_TASK) !=
	    (b->attach_state & PERF_ATTACH_TASK))
		return false;

	/* not task */

	return true; /* if not task, we're machine wide */
}

/*
 * Determine if @na's tasks intersect with @b's tasks
 */
static bool __conflict_event(struct perf_event *a, struct perf_event *b)
{
	/*
	 * If one of them is not a task, same story as above with cgroups.
	 */
	if (!(a->attach_state & PERF_ATTACH_TASK) ||
	    !(b->attach_state & PERF_ATTACH_TASK))
		return true;

	/*
	 * Must be non-overlapping.
	 */
	return false;
}

/*
 * Find a group and setup RMID.
 *
 * If we're part of a group, we use the group's RMID.
 */
static int intel_qos_setup_event(struct perf_event *event,
				 struct perf_event **group, int cpu)
{
	struct perf_event *iter;
	int rmid;

	list_for_each_entry(iter, &cache_groups, hw.qos_groups_entry) {
		if (__match_event(iter, event)) {
			/* All tasks in a group share an RMID */
			event->hw.qos_rmid = iter->hw.qos_rmid;
			event->hw.qos_package_count =
				iter->hw.qos_package_count;
			*group = iter;
			return 0;
		}

		if (__conflict_event(iter, event))
			return -EINVAL;
	}

	rmid = __get_rmid();
	if (rmid < 0)
		return rmid;

	event->hw.qos_rmid = rmid;

	/*
	 * For a task event we need counters for each package so that we
	 * can cache the last read value.
	 */
	if (cpu == -1) {
		u64 *counts;

		counts = kzalloc(sizeof(u64) *
				 cpumask_weight(&qos_cpumask), GFP_KERNEL);
		if (!counts) {
			__put_rmid(rmid);
			return -ENOMEM;
		}

		event->hw.qos_package_count = counts;
	}

	return 0;
}

static void intel_qos_async_read(void *data)
{
	struct perf_event *event = data;
	unsigned long flags;
	int i, index = 0;
	u64 val;

	raw_spin_lock_irqsave(&cache_lock, flags);

	for_each_cpu(i, &qos_cpumask) {
		if (i == smp_processor_id())
			break;
		index++;
	}

	val = __rmid_read(event->hw.qos_rmid);
	event->hw.qos_package_count[index] = val;

	local64_set(&event->count, 0);
	for (i = 0; i < cpumask_weight(&qos_cpumask); i++)
		local64_add(event->hw.qos_package_count[i], &event->count);

	raw_spin_unlock_irqrestore(&cache_lock, flags);
}

static void intel_qos_event_read(struct perf_event *__event)
{
	struct perf_event *event;
	unsigned long rmid;
	int i, index, phys_id;
	u64 val;

	/*
	 * Walk up the chain of parent events till we find the root.
	 * By default all child event counters are accumulated in the
	 * parent, leading to duplicate values for task events. So we
	 * just leave all child counters at zero and only update the
	 * parent's counter.
	 */
	for (event = __event; event->parent; event = event->parent)
		;

	rmid = event->hw.qos_rmid;
	val = __rmid_read(rmid);

	/*
	 * Ignore this reading on error states and do not update the value.
	 */
	if (val & (RMID_VAL_ERROR | RMID_VAL_UNAVAIL))
		return;

	val *= qos_l3_scale; /* cachelines -> bytes */

	/*
	 * If this event is per-cpu then we don't need to do any
	 * aggregation in the kernel, it's all done in userland.
	 */
	if (event->cpu != -1) {
		local64_set(&event->count, val);
		return;
	}

	/*
	 * OK, we've got a task event, recompute the total occupancy.
	 *
	 * There is a race window here because we're using stale
	 * occupancy values since we're not able to do a cross-CPU
	 * (socket) call to do the occupancy read because we're
	 * executing with interrupts disabled.
	 *
	 * In an ideal world we'd do a smp_call_function_single() to
	 * read the other sockets' instantaneous values because it may
	 * have changed (reduced) since we last updated ->hw.qos_value[].
	 *
	 * If these values prove to be wildly inaccurate we may want to
	 * consider installing a per-socket hrtimer to refresh the
	 * values periodically.
	 */
	local64_set(&event->count, 0);

	phys_id = topology_physical_package_id(smp_processor_id());
	index = 0;

	/* Convert phys_id to hw->qos_package_count index */
	for_each_cpu(i, &qos_cpumask) {
		if (phys_id == topology_physical_package_id(i)) {
			index = i;
			continue;
		}

		event->hw.qos_csd.func = intel_qos_async_read;
		event->hw.qos_csd.info = event;
		event->hw.qos_csd.flags = 0;
		smp_call_function_single_async(i, &event->hw.qos_csd);
	}

	event->hw.qos_package_count[index] = val;

	for (i = 0; i < cpumask_weight(&qos_cpumask); i++)
		local64_add(event->hw.qos_package_count[i], &event->count);
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

static int intel_qos_event_init(struct perf_event *event)
{
	struct perf_event *group = NULL;
	int err;

	if (event->attr.type != intel_qos_pmu.type)
		return -ENOENT;

	if (event->attr.config & ~QOS_EVENT_MASK)
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

	INIT_LIST_HEAD(&event->hw.qos_group_entry);
	INIT_LIST_HEAD(&event->hw.qos_groups_entry);
	INIT_LIST_HEAD(&event->hw.qos_events_entry);

	event->destroy = intel_qos_event_destroy;

	mutex_lock(&cache_mutex);

	/* Will also set rmid */
	err = intel_qos_setup_event(event, &group, event->cpu);
	if (err)
		goto out;

	raw_spin_lock_irq(&cache_lock);
	if (group) {
		list_add_tail(&event->hw.qos_group_entry,
			      &group->hw.qos_group_entry);
	} else {
		list_add_tail(&event->hw.qos_groups_entry,
			      &cache_groups);
	}

	list_add_tail(&event->hw.qos_events_entry, &cache_events);
	raw_spin_unlock_irq(&cache_lock);

out:
	mutex_unlock(&cache_mutex);
	return err;
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

static ssize_t
intel_qos_get_attr_readers(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	int n = cpulist_scnprintf(buf, PAGE_SIZE - 2, &qos_cpumask);

	buf[n++] = '\n';
	buf[n] = '\0';
	return n;
}

static DEVICE_ATTR(readers, S_IRUGO, intel_qos_get_attr_readers, NULL);
static struct attribute *intel_qos_pmu_attr[] = {
	&dev_attr_readers.attr,
	NULL,
};

static const struct attribute_group intel_qos_pmu_group = {
	.attrs = intel_qos_pmu_attr,
};

const struct attribute_group *intel_qos_attr_groups[] = {
	&intel_qos_events_group,
	&intel_qos_format_group,
	&intel_qos_pmu_group,
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

static inline void qos_pick_event_reader(int cpu)
{
	int phys_id = topology_physical_package_id(cpu);
	int i;

	for_each_cpu(i, &qos_cpumask) {
		if (phys_id == topology_physical_package_id(i))
			return;	/* already got reader for this socket */
	}

	cpumask_set_cpu(cpu, &qos_cpumask);
}

static int __init intel_qos_init(void)
{
	int i, cpu, ret;

	if (!cpu_has(&boot_cpu_data, X86_FEATURE_CQM_OCCUP_LLC))
		return -ENODEV;

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

	ret = intel_qos_setup_rmid_cache();
	if (ret)
		return ret;

	/*
	 * XXX hotplug notifiers!
	 */
	for_each_possible_cpu(i) {
		struct intel_qos_state *state = &per_cpu(qos_state, i);

		raw_spin_lock_init(&state->lock);
		qos_pick_event_reader(i);
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
