// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/cpumask.h>
#include <linux/jiffies.h>
#include <linux/version.h>
#include <uapi/linux/sched/types.h>

static int cpu = 0;
module_param(cpu, int, 0444);
MODULE_PARM_DESC(cpu, "CPU to bind the reader thread to");

static int stall_ms = 30000;
module_param(stall_ms, int, 0444);
MODULE_PARM_DESC(stall_ms, "How long the reader stays in rcu_read_lock(), in ms");

static int start_delay_ms = 0;
module_param(start_delay_ms, int, 0444);
MODULE_PARM_DESC(start_delay_ms, "Delay before updater calls synchronize_rcu(), in ms");

/* Avoid clashing with rt_prio() helper in sched/rt.h by renaming the backing
 * variable while keeping the exposed module parameter name. */
static int lkmc_rt_prio = 99;
module_param_named(rt_prio, lkmc_rt_prio, int, 0444);
MODULE_PARM_DESC(rt_prio, "SCHED_FIFO priority for the reader thread (1..99)");
#define LKMC_RT_PRIO lkmc_rt_prio

static struct task_struct *reader_task;
static struct task_struct *updater_task;

static int reader_fn(void *arg)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0)
	struct sched_param sp = { .sched_priority = LKMC_RT_PRIO };
#endif
	unsigned long end;

	/* Bind early (also done by kthread_bind() from init). */
	if (cpu >= 0 && cpu < nr_cpu_ids)
		set_cpus_allowed_ptr(current, cpumask_of(cpu));

	/*
	 * Make it deterministic: FIFO means no timeslice-based preemption,
	 * so we avoid context switches that would otherwise let RCU see
	 * quiescent states.
	 */
	/* v5.9-rc1 616d91b68cd56c47cf61f7758b80b1f70d3ef3e8 ("sched: Remove sched_setscheduler*() EXPORTs")
	 * removed the export; fall back to the exported sched_set_fifo() helper on newer kernels. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0)
	if (sched_setscheduler(current, SCHED_FIFO, &sp))
		pr_warn("rcu_stall_demo: failed to set SCHED_FIFO prio=%d\n", LKMC_RT_PRIO);
#else
	sched_set_fifo(current);
	if (LKMC_RT_PRIO != 99)
		pr_warn_once("rcu_stall_demo: rt_prio ignored on this kernel (using sched_set_fifo)\n");
#endif

	pr_info("rcu_stall_demo: reader starting on CPU %d, holding rcu_read_lock for %d ms\n",
		raw_smp_processor_id(), stall_ms);

	rcu_read_lock();
	end = jiffies + msecs_to_jiffies(stall_ms);

	/* Busy spin while holding the RCU read-side critical section. */
	while (time_before(jiffies, end)) {
		if (kthread_should_stop())
			break;
		cpu_relax();
	}

	rcu_read_unlock();

	pr_info("rcu_stall_demo: reader done\n");
	return 0;
}

static int updater_fn(void *arg)
{
	pr_info("rcu_stall_demo: updater sleeping %d ms, then calling synchronize_rcu()\n",
		start_delay_ms);

	if (start_delay_ms > 0)
		msleep(start_delay_ms);

	/*
	 * This starts a grace period and waits for it. Since the reader is
	 * holding rcu_read_lock() without reaching a quiescent state, the
	 * grace period should extend long enough to trigger an RCU stall warning.
	 */
	pr_info("rcu_stall_demo: updater calling synchronize_rcu() (this may block)\n");
	synchronize_rcu();
	pr_info("rcu_stall_demo: updater synchronize_rcu() returned\n");

	return 0;
}

static int __init rcu_stall_demo_init(void)
{
	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu)) {
		pr_err("rcu_stall_demo: CPU %d invalid/offline\n", cpu);
		return -EINVAL;
	}

	reader_task = kthread_run(reader_fn, NULL, "rcu_stall_reader");
	if (IS_ERR(reader_task)) {
		pr_err("rcu_stall_demo: failed to start reader thread\n");
		return PTR_ERR(reader_task);
	}
	kthread_bind(reader_task, cpu);

	updater_task = kthread_run(updater_fn, NULL, "rcu_stall_updater");
	if (IS_ERR(updater_task)) {
		pr_err("rcu_stall_demo: failed to start updater thread\n");
		kthread_stop(reader_task);
		return PTR_ERR(updater_task);
	}

	pr_info("rcu_stall_demo: loaded (cpu=%d stall_ms=%d start_delay_ms=%d rt_prio=%d)\n",
		cpu, stall_ms, start_delay_ms, LKMC_RT_PRIO);
	return 0;
}

static void __exit rcu_stall_demo_exit(void)
{
	pr_info("rcu_stall_demo: unloading\n");

	if (updater_task)
		kthread_stop(updater_task);

	if (reader_task)
		kthread_stop(reader_task);

	pr_info("rcu_stall_demo: unloaded\n");
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(__FILE__);
MODULE_AUTHOR("demo");
module_init(rcu_stall_demo_init);
module_exit(rcu_stall_demo_exit);
