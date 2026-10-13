/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2013-2018, The Linux Foundation. All rights reserved.
 */

#ifdef CONFIG_MSM_BOOT_STATS
int boot_stats_init(void);

#ifdef CONFIG_SEC_BSP
extern uint32_t bs_linuxloader_start;
extern uint32_t bs_linux_start;
extern uint32_t bs_uefi_start;
extern uint32_t bs_bootloader_load_kernel;
extern unsigned int get_boot_stat_time(void);
extern unsigned int get_boot_stat_freq(void);
#endif
#else
static inline int boot_stats_init(void) { return 0; }
#endif
