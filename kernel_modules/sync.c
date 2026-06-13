/* Demonstrate protecting a shared counter with spinlock vs mutex. */

#include <asm/processor.h> /* cpu_relax */
#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/version.h>

enum sync_mode {
	SYNC_NONE,
	SYNC_SPINLOCK,
	SYNC_MUTEX,
};

static char *sync = "mutex";
module_param(sync, charp, 0444);
MODULE_PARM_DESC(sync, "none|spinlock|mutex (default: mutex)");

static unsigned int threads = 4;
module_param(threads, uint, 0444);
MODULE_PARM_DESC(threads, "number of kthreads to run");

static unsigned long iterations = 200000;
module_param(iterations, ulong, 0444);
MODULE_PARM_DESC(iterations, "increments per thread");

static enum sync_mode selected_mode;
static struct task_struct **workers;
static atomic_t threads_left;
static DECLARE_COMPLETION(start_comp);
static DECLARE_COMPLETION(all_done);

static u64 counter;
static DEFINE_SPINLOCK(counter_spinlock);
static DEFINE_MUTEX(counter_mutex);

static const char *mode_to_string(enum sync_mode m)
{
	switch (m) {
	case SYNC_NONE:
		return "none";
	case SYNC_SPINLOCK:
		return "spinlock";
	case SYNC_MUTEX:
		return "mutex";
	}
	return "unknown";
}

static int parse_mode(const char *value, enum sync_mode *out)
{
	if (!value)
		return -EINVAL;

	if (sysfs_streq(value, "none"))
		*out = SYNC_NONE;
	else if (sysfs_streq(value, "spinlock"))
		*out = SYNC_SPINLOCK;
	else if (sysfs_streq(value, "mutex"))
		*out = SYNC_MUTEX;
	else
		return -EINVAL;

	return 0;
}

static void bump_counter(void)
{
	switch (selected_mode) {
	case SYNC_SPINLOCK:
		spin_lock(&counter_spinlock);
		break;
	case SYNC_MUTEX:
		mutex_lock(&counter_mutex);
		break;
	case SYNC_NONE:
		break;
	default:
		break;
	}
    counter++;
	switch (selected_mode) {
	case SYNC_SPINLOCK:
		spin_unlock(&counter_spinlock);
		break;
	case SYNC_MUTEX:
		mutex_unlock(&counter_mutex);
		break;
	case SYNC_NONE:
		break;
	default:
		break;
	}
}

static int worker_fn(void *data)
{
	unsigned long i;

	wait_for_completion(&start_comp);

	for (i = 0; i < iterations; i++) {
		if (kthread_should_stop())
			break;
		bump_counter();
	}

	if (atomic_dec_and_test(&threads_left))
		complete(&all_done);

	return 0;
}

static struct task_struct *start_worker(unsigned int id)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 11)
	return kthread_run(worker_fn, (void *)(unsigned long)id,
			   "sync_demo/%u", id);
#else
	struct task_struct *t;

	t = kthread_create(worker_fn, (void *)(unsigned long)id,
			   "sync_demo/%u", id);
	if (!IS_ERR(t))
		wake_up_process(t);
	return t;
#endif
}

static int __init sync_demo_init(void)
{
	unsigned int i, started = 0;
	u64 expected;
	int ret;

	ret = parse_mode(sync, &selected_mode);
	if (ret) {
		pr_err("sync_demo: invalid sync='%s'\n", sync ? sync : "<null>");
		return ret;
	}

	if (!threads) {
		pr_warn("sync_demo: threads=0 treated as 1\n");
		threads = 1;
	}

	workers = kcalloc(threads, sizeof(*workers), GFP_KERNEL);
	if (!workers)
		return -ENOMEM;

	reinit_completion(&start_comp);
	reinit_completion(&all_done);
	atomic_set(&threads_left, threads);
	counter = 0;

	for (i = 0; i < threads; i++) {
		workers[i] = start_worker(i);
		if (IS_ERR(workers[i])) {
			ret = PTR_ERR(workers[i]);
			goto stop_started;
		}
		started++;
	}

	pr_info("sync_demo: running sync=%s threads=%u iterations=%lu\n",
		mode_to_string(selected_mode), threads, iterations);

	if (num_online_cpus() < 2 && selected_mode == SYNC_NONE)
		pr_warn("sync_demo: only one CPU online; races may not be visible\n");
	complete_all(&start_comp);
	wait_for_completion(&all_done);
	expected = (u64)threads * (u64)iterations;
	if (selected_mode == SYNC_NONE) {
		u64 delta = (counter > expected) ? (counter - expected) :
						 (expected - counter);
		if (delta)
			pr_info("sync_demo: unsynchronized updates skewed counter by %llu (expected %llu got %llu)\n",
				  delta, expected, counter);
		else
			pr_err("sync_demo: unsynchronized run finished without visible skew (expected %llu got %llu)\n",
				  expected, counter);
	} else if (counter != expected) {
		pr_err("sync_demo: counter mismatch under %s (expected %llu got %llu)\n",
			  mode_to_string(selected_mode), expected, counter);
	} else {
		pr_info("sync_demo: counter reached %llu with %s\n",
			counter, mode_to_string(selected_mode));
	}

	ret = 0;
	goto out;

stop_started:
	complete_all(&start_comp);
	while (started--)
		kthread_stop(workers[started]);
out:
	kfree(workers);
	workers = NULL;
	return ret;
}

static void __exit sync_demo_exit(void)
{
	pr_info("sync_demo: unloaded\n");
}

module_init(sync_demo_init);
module_exit(sync_demo_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(__FILE__);
