// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/printk.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/cred.h>

#include "sc.h"
#include "rwmem_proto.h"
#include "rwmem.h"
#include "rwmem_sc.h"

#define RWMEM_SLOT_MAIN 249
static const int rwmem_slot_pref[] = { 249, 42, 18, 415 };

static char g_key[RWMEM_KEY_MAX];
static unsigned long g_orig;
static int g_slot = -1;

static bool slot_is_empty(int nr)
{
	unsigned long *sct;
	unsigned long ni;
	unsigned long ni_cfi;
	unsigned long v;

	sct = (unsigned long *)rwmem_name_to_addr("sys_call_table");
	ni = rwmem_name_to_addr("__arm64_sys_ni_syscall");
	ni_cfi = rwmem_name_to_addr("__arm64_sys_ni_syscall.cfi_jt");
	if (!sct || !ni)
		return false;
	if (copy_from_kernel_nofault(&v, &sct[nr], sizeof(v)))
		return false;
	return v == ni || (ni_cfi && v == ni_cfi);
}

static long rwmem_handler(const struct pt_regs *regs)
{
	const char __user *key_ptr;
	char kbuf[RWMEM_KEY_MAX] = {0};
	long cmd;

	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return -ENOSYS;

	key_ptr = (const char __user *)regs->regs[0];
	if (strncpy_from_user(kbuf, key_ptr, sizeof(kbuf) - 1) < 0)
		return -ENOSYS;
	if (strncmp(kbuf, g_key, sizeof(kbuf)))
		return -ENOSYS;

	cmd = regs->regs[1];
	if (cmd == SC_CMD_HELLO)
		return SC_MAGIC;

	return rwmem_dispatch(cmd, regs->regs[2], regs->regs[3], regs->regs[4],
			      regs->regs[5]);
}

int rwmem_sc_init(const char *key)
{
	struct sc_layout layout = {
		.resolve = rwmem_name_to_addr,
		.pgd_off = rwmem_pgd_off,
	};
	struct sc_cfg cfg = {0};
	int slot = -1;
	size_t i;
	int ret;

	if (!key || !key[0])
		return -EINVAL;
	strscpy(g_key, key, sizeof(g_key));

	strscpy(cfg.key, g_key, sizeof(cfg.key));
	cfg.layout = &layout;
	cfg.no_patch = true;

	ret = sc_init(&cfg);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(rwmem_slot_pref); i++) {
		if (slot_is_empty(rwmem_slot_pref[i])) {
			slot = rwmem_slot_pref[i];
			break;
		}
	}
	if (slot < 0)
		slot = sc_find_slot_scan();
	if (slot < 0) {
		sc_exit();
		return -EBUSY;
	}

	ret = sc_patch(slot, (unsigned long)rwmem_handler, &g_orig);
	if (ret) {
		sc_exit();
		return ret;
	}
	g_slot = slot;

	pr_info("[rwmem] channel slot=%d key=%s\n", g_slot, g_key);
	return 0;
}

void rwmem_sc_exit(void)
{
	if (g_slot >= 0)
		sc_unpatch(g_slot);
	g_slot = -1;
	sc_exit();
}
