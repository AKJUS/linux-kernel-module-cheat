/* https://cirosantilli.com/linux-kernel-module-cheat#init-module */

#include <linux/module.h>
#include <linux/kernel.h>

static int myinit(void)
{
	pr_info("init_module\n");
	return 0;
}

static void myexit(void)
{
	pr_info("cleanup_module\n");
}
module_init(myinit)
module_exit(myexit)
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(__FILE__);

