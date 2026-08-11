// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>

#include "core.h"
#include "type_info.h"
#include "hidemod.h"
#include "rwmem_sc.h"
#include "rwmem.h"

unsigned long __nocfi kr_name_to_addr(const char *name)
{
	if (kallrecon_klp)
		return kallrecon_klp(name);
	return kallsyms_name_to_addr(name);
}

static int __init rwmem_init(void)
{
	struct ti_resolver res;
	int ret;

	find_kallsyms_base();
	if (!kallrecon_klp) {
		pr_warn("[rwmem] kallsyms recovery failed\n");
		return -ENODATA;
	}
	pr_info("[rwmem] kallsyms: %u symbols\n", klnum_val);

	res.name_to_addr = kr_name_to_addr;
	ti_init(&res);
	pr_info("[rwmem] ti_init btf=%d types=%u\n", ti_btf_available(),
		ti_type_count());

	ret = rwmem_sc_init();
	if (ret)
		return ret;

	hm_init(&(struct hm_resolver){ .name_to_addr = kr_name_to_addr });
	hm_set_actions(HM_ACT_ALL);
	hm_add_module(THIS_MODULE, NULL);

	pr_info("[rwmem] loaded\n");
	return 0;
}

static void __exit rwmem_exit(void)
{
	hm_unhide();
	hm_exit();
	rwmem_sc_exit();
	ti_exit();
	pr_info("[rwmem] unloaded\n");
}

module_init(rwmem_init);
module_exit(rwmem_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("rwMemory: cross-process memory read/write via syscall channel");
