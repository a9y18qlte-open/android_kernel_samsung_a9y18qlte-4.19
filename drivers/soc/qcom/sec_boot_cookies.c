// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung boot cookies, a stand-in for sec_debug's init until that is ported.
 *
 * Samsung's XBL reads the IMEM restart reason and upload cause left by the
 * previous boot. If a running kernel didn't mark itself as booted normally,
 * the next reset is treated as abnormal and XBL retrains DDR, which wipes
 * RAM, ramoops included. Write what sec_debug_init() writes on 4.4 at debug
 * level low: restart reason NORMAL and upload cause INIT. Download mode is
 * kept off through msm_poweroff.download_mode=0.
 *
 * Bring-up aid: with sec_boot_cookies.crash_to_recovery=1 the restart reason
 * is set to recovery instead (IMEM at once, PMIC PON reason once the PON
 * driver is up), so a panic, watchdog reset or forced reset of a test kernel
 * lands in recovery, where pstore can be read.
 */

#define pr_fmt(fmt) "sec_boot_cookies: " fmt

#include <linux/init.h>
#include <linux/input/qpnp-power-on.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/moduleparam.h>

#define RESTART_REASON_NORMAL	0x0
#define RESTART_REASON_RECOVERY	0x77665502
#define UPLOAD_CAUSE_INIT	0xCAFEBABE

static bool crash_to_recovery;
module_param(crash_to_recovery, bool, 0444);

static int __init sec_boot_cookie_write(const char *compat, u32 val)
{
	struct device_node *np;
	void __iomem *addr;

	np = of_find_compatible_node(NULL, NULL, compat);
	if (!np) {
		pr_err("no %s node\n", compat);
		return -ENODEV;
	}
	addr = of_iomap(np, 0);
	of_node_put(np);
	if (!addr) {
		pr_err("can't map %s\n", compat);
		return -ENOMEM;
	}
	__raw_writel(val, addr);
	iounmap(addr);
	return 0;
}

static int __init sec_boot_cookies_init(void)
{
	sec_boot_cookie_write("qcom,msm-imem-restart_reason",
			      crash_to_recovery ? RESTART_REASON_RECOVERY :
						  RESTART_REASON_NORMAL);
	sec_boot_cookie_write("qcom,msm-imem-upload_cause", UPLOAD_CAUSE_INIT);
	/* make sure the cookies are in IMEM before anything else can reset */
	wmb();
	pr_info("restart reason %s, upload cause init\n",
		crash_to_recovery ? "recovery" : "normal");
	return 0;
}
early_initcall(sec_boot_cookies_init);

static int __init sec_boot_cookies_pon_init(void)
{
	int ret;

	if (!crash_to_recovery)
		return 0;
	ret = qpnp_pon_set_restart_reason(PON_RESTART_REASON_RECOVERY);
	pr_info("PON restart reason recovery: %d\n", ret);
	return 0;
}
late_initcall(sec_boot_cookies_pon_init);
