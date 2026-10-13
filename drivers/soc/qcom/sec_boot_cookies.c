// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung boot cookies, bring-up aid on top of sec_debug.
 *
 * sec_debug writes the IMEM restart reason and upload cause that Samsung's
 * XBL reads on the next boot. With sec_boot_cookies.crash_to_recovery=1 the
 * restart reason is set to recovery instead (IMEM at once, before sec_debug
 * is up, and the PMIC PON reason once the PON driver is up), and sec_debug
 * keeps it on panics, so a panic, watchdog reset or forced reset of a test
 * kernel lands in recovery, where its log can be read. A reboot that doesn't
 * ask for recovery, bootloader or download sets the PON reason back to
 * Samsung's NORMALBOOT, so e.g. "adb reboot system" boots the boot partition
 * again.
 */

#define pr_fmt(fmt) "sec_boot_cookies: " fmt

#include <linux/init.h>
#include <linux/input/qpnp-power-on.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/sec_debug.h>
#include <linux/string.h>

static bool crash_to_recovery;
module_param(crash_to_recovery, bool, 0444);

bool sec_boot_cookies_crash_to_recovery(void)
{
	return crash_to_recovery;
}

static int __init sec_boot_cookies_init(void)
{
	struct device_node *np;
	void __iomem *addr;

	if (!crash_to_recovery)
		return 0;

	np = of_find_compatible_node(NULL, NULL, "qcom,msm-imem-restart_reason");
	if (!np) {
		pr_err("no restart_reason node\n");
		return -ENODEV;
	}
	addr = of_iomap(np, 0);
	of_node_put(np);
	if (!addr) {
		pr_err("can't map restart_reason\n");
		return -ENOMEM;
	}
	__raw_writel(RESTART_REASON_RECOVERY, addr);
	iounmap(addr);
	/* make sure the cookie is in IMEM before anything else can reset */
	wmb();
	pr_info("restart reason recovery\n");
	return 0;
}
early_initcall(sec_boot_cookies_init);

static int sec_boot_cookies_reboot(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	const char *cmd = data;

	if (action != SYS_RESTART)
		return NOTIFY_DONE;
	if (cmd && (!strncmp(cmd, "recovery", 8) ||
		    !strncmp(cmd, "bootloader", 10) ||
		    !strncmp(cmd, "download", 8)))
		return NOTIFY_DONE;
	qpnp_pon_set_restart_reason(PON_RESTART_REASON_NORMALBOOT);
	return NOTIFY_DONE;
}

static struct notifier_block sec_boot_cookies_reboot_nb = {
	.notifier_call = sec_boot_cookies_reboot,
};

static int __init sec_boot_cookies_pon_init(void)
{
	int ret;

	if (!crash_to_recovery)
		return 0;
	ret = qpnp_pon_set_restart_reason(PON_RESTART_REASON_RECOVERY);
	pr_info("PON restart reason recovery: %d\n", ret);
	register_reboot_notifier(&sec_boot_cookies_reboot_nb);
	return 0;
}
late_initcall(sec_boot_cookies_pon_init);
