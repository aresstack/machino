// nowait: switch the MIPS idle loop from WAIT to polling, at runtime.
//
// The causal test for the T40NN hardlock (docs/ap17-hardlock.md). After every
// watchdog reset the boot log prints the program counter each CPU held when the
// hardware fired. Across every hardlock so far CPU0 stood at
//
//   0x8001AC08 = ingenic_wait_irqoff+0x24      (the MIPS WAIT idle routine)
//
// while CPU1 was still executing user space. After a clean `reboot` the
// rebooting CPU stands in __delay instead, so the value is diagnostic, not a
// fixed idle snapshot. The hypothesis is therefore: CPU0 enters WAIT and the
// wake-up that should end it is lost.
//
// arch/mips/kernel/idle.c decides how a CPU idles:
//
//   void arch_cpu_idle(void) { if (cpu_wait) cpu_wait(); else local_irq_enable(); }
//
// cpu_wait is a plain exported function pointer, read on every idle entry. Set
// it to NULL and the CPU idles by polling with interrupts enabled - the same
// thing the `nowait` kernel parameter does, only without a bootloader write.
// This module does exactly that, and undoes it on unload or when its
// parameter is set back to 0.
//
//   insmod nowait.ko                    WAIT off, idle polls
//   echo 0 > /sys/module/nowait/parameters/enable   WAIT back on
//   rmmod nowait                        WAIT back on
//
// A/B/A on the camera: load, idle 35 min, first access -> survives over enough
// rounds means the WAIT/wake-up path is the cause; still dies means it is not.
// Unload (or reboot) must bring the hang back for the result to count.
//
// Cost while loaded: the idle CPU never sleeps, so it runs warmer and draws
// more. Fine for a test, not for production.
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include <asm/idle.h>

static void (*saved_wait)(void);
static bool active;

static void wait_off(void)
{
	if (active) return;
	saved_wait = cpu_wait;
	cpu_wait = NULL;
	active = true;
	pr_info("nowait: cpu_wait was %pS, now NULL - idle polls, no WAIT\n", saved_wait);
}

static void wait_on(void)
{
	if (!active) return;
	cpu_wait = saved_wait;
	active = false;
	pr_info("nowait: cpu_wait restored to %pS\n", cpu_wait);
}

static int enable_set(const char *val, const struct kernel_param *kp)
{
	bool want;
	int rc = strtobool(val, &want);   /* kstrtobool arrives in 4.6; 4.4 has strtobool */
	if (rc) return rc;
	if (want) wait_off(); else wait_on();
	return 0;
}

static int enable_get(char *buf, const struct kernel_param *kp)
{
	return sprintf(buf, "%d\n", active ? 1 : 0);
}

static const struct kernel_param_ops enable_ops = {
	.set = enable_set,
	.get = enable_get,
};
module_param_cb(enable, &enable_ops, NULL, 0644);
MODULE_PARM_DESC(enable, "1 = WAIT idle off (poll), 0 = WAIT idle restored");

static int __init nowait_init(void)
{
	pr_info("nowait: loaded; cpu_wait is %pS\n", cpu_wait);
	wait_off();
	return 0;
}

static void __exit nowait_exit(void)
{
	wait_on();
	pr_info("nowait: unloaded\n");
}

module_init(nowait_init);
module_exit(nowait_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("machino");
MODULE_DESCRIPTION("T40NN hardlock causal test: disable the MIPS WAIT idle at runtime");
