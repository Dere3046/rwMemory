// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/printk.h>

#include "core.h"
#include "type_info.h"
#include "rwmem_proto.h"
#include "rwmem.h"
#include "rwmem_sc.h"

static char sc_key[RWMEM_KEY_MAX] = "rwmem";
module_param_string(key, sc_key, sizeof(sc_key), 0400);

static unsigned long __nocfi name_to_addr(const char *name)
{
	if (kallrecon_klp)
		return kallrecon_klp(name);
	return kallsyms_name_to_addr(name);
}

static const struct rwmem_resolver rwmem_res = {
	.name_to_addr = name_to_addr,
};

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

	res.name_to_addr = name_to_addr;
	ti_init(&res);
	pr_info("[rwmem] ti_init btf=%d types=%u\n", ti_btf_available(),
		ti_type_count());

	ret = rwmem_core_init(&rwmem_res);
	if (ret)
		return ret;

	ret = rwmem_sc_init(sc_key);
	if (ret)
		return ret;

	pr_info("[rwmem] loaded\n");
	return 0;
}

static void __exit rwmem_exit(void)
{
	rwmem_sc_exit();
	rwmem_core_exit();
	ti_exit();
	pr_info("[rwmem] unloaded\n");
}

module_init(rwmem_init);
module_exit(rwmem_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("rwMemory: cross-process memory read/write via syscall channel");
