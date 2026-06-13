/* https://cirosantilli.com/linux-kernel-module-cheat#workqueues */

#include <linux/delay.h> /* usleep_range */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h> /* atomic_t */
#include <linux/workqueue.h>

struct my_work {
	struct work_struct work;
	int id;
};

static int num_queues = 2;
module_param(num_queues, int, 0444);
MODULE_PARM_DESC(num_queues, "Number of workqueues to spawn (>=1)");

static struct workqueue_struct **queues;
static struct my_work *works;
static atomic_t run = ATOMIC_INIT(1);

static void work_func(struct work_struct *work)
{
	struct my_work *mw = container_of(work, struct my_work, work);
	int i = 0;
	unsigned int sleep_us = 500000 * mw->id; /* id 1 => 0.5s, id 2 => 1s */

	while (atomic_read(&run)) {
		pr_info("work.id=%d i=%d\n", mw->id, i);
		usleep_range(sleep_us, sleep_us + 1000);
		i++;
		if (i == 10)
			i = 0;
	}
}

static int myinit(void)
{
	int i;
	struct workqueue_struct *wq;

	if (num_queues < 1)
		return -EINVAL;
	queues = kcalloc(num_queues, sizeof(*queues), GFP_KERNEL);
	if (!queues)
		return -ENOMEM;
	works = kcalloc(num_queues, sizeof(*works), GFP_KERNEL);
	if (!works) {
		kfree(queues);
		return -ENOMEM;
	}
	for (i = 0; i < num_queues; i++) {
		wq = alloc_workqueue("myworkqueue-%d", WQ_UNBOUND, 1, i + 1);
		if (!wq)
			goto err_create;
		queues[i] = wq;
		works[i].id = i + 1;
		INIT_WORK(&works[i].work, work_func);
		queue_work(queues[i], &works[i].work);
	}

	return 0;

err_create:
	while (--i >= 0)
		destroy_workqueue(queues[i]);
	kfree(works);
	kfree(queues);
	return -ENOMEM;
}

static void myexit(void)
{
	int i;

	atomic_set(&run, 0);
	if (queues) {
		for (i = 0; i < num_queues; i++) {
			if (!queues[i])
				continue;
			flush_workqueue(queues[i]);
			destroy_workqueue(queues[i]);
		}
	}
	kfree(works);
	kfree(queues);
}

module_init(myinit)
module_exit(myexit)
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(__FILE__);
