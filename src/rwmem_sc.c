// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/string.h>

#include "sc.h"
#include "hidemod.h"
#include "rwmem_proto.h"
#include "rwmem.h"
#include "touch.h"
#include "dmabuf.h"
#include "rwmem_sc.h"

extern unsigned long kr_name_to_addr(const char *name);

static char sc_key[RWMEM_KEY_MAX] = "rwmem";
module_param_string(key, sc_key, sizeof(sc_key), 0400);

#define RWMEM_SLOT_MAIN 249
static const int rwmem_slot_pref[] = { 249, 42, 18, 415 };

static unsigned long g_orig;
static int g_slot = -1;

static bool slot_is_empty(int nr)
{
	unsigned long *sct;
	unsigned long ni;
	unsigned long ni_cfi;
	unsigned long v;

	sct = (unsigned long *)kr_name_to_addr("sys_call_table");
	ni = kr_name_to_addr("__arm64_sys_ni_syscall");
	ni_cfi = kr_name_to_addr("__arm64_sys_ni_syscall.cfi_jt");
	if (!sct || !ni)
		return false;
	if (copy_from_kernel_nofault(&v, &sct[nr], sizeof(v)))
		return false;
	return v == ni || (ni_cfi && v == ni_cfi);
}

static long rwmem_handler(const struct pt_regs *regs)
{
	const char __user *key_ptr;
	long cmd;
	char kbuf[RWMEM_KEY_MAX] = {0};

	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return -ENOSYS;

	key_ptr = (const char __user *)regs->regs[0];
	if (strncpy_from_user(kbuf, key_ptr, sizeof(kbuf) - 1) < 0)
		return -ENOSYS;
	if (strncmp(kbuf, sc_key, sizeof(kbuf)))
		return -ENOSYS;

	cmd = regs->regs[1];
	if (cmd == SC_CMD_HELLO)
		return SC_MAGIC;

	switch (cmd) {
	case RWMEM_CMD_OPEN:
		return rwmem_open((pid_t)regs->regs[2]);
	case RWMEM_CMD_CLOSE:
		rwmem_close((int)regs->regs[2]);
		return 0;
	case RWMEM_CMD_READ:
		return rwmem_read((int)regs->regs[2], regs->regs[3],
				  (char __user *)regs->regs[5],
				  (size_t)regs->regs[4]);
	case RWMEM_CMD_WRITE:
		return rwmem_write((int)regs->regs[2], regs->regs[3],
				   (const char __user *)regs->regs[5],
				   (size_t)regs->regs[4]);
	case RWMEM_CMD_READ_FORCE:
		return rwmem_read_force((int)regs->regs[2], regs->regs[3],
					(char __user *)regs->regs[5],
					(size_t)regs->regs[4]);
	case RWMEM_CMD_WRITE_FORCE:
		return rwmem_write_force((int)regs->regs[2], regs->regs[3],
					 (const char __user *)regs->regs[5],
					 (size_t)regs->regs[4]);
	case RWMEM_CMD_PID_LIST:
		return rwmem_pid_list((pid_t __user *)regs->regs[3],
				      (size_t)regs->regs[4]);
	case RWMEM_CMD_QUERY_MAPS:
		return rwmem_query_maps((int)regs->regs[2],
					(struct rwmem_map __user *)regs->regs[3],
					(size_t)regs->regs[4],
					(unsigned long)regs->regs[5]);
	case RWMEM_CMD_GET_CMDLINE:
		return rwmem_get_cmdline((int)regs->regs[2],
					 (struct rwmem_cmdline __user *)
					 regs->regs[3]);
	case RWMEM_CMD_VECTOR:
		return rwmem_vector((int)regs->regs[2],
				    (struct rwmem_iovec __user *)regs->regs[3],
				    (size_t)regs->regs[4],
				    (int)regs->regs[5]);
	case RWMEM_CMD_REMAP:
		return rwmem_remap((struct rwmem_remap_arg __user *)regs->regs[2]);
	case RWMEM_CMD_GET_BASE:
		return rwmem_get_base((struct rwmem_base_arg __user *)regs->regs[2]);
	case RWMEM_CMD_TOUCH:
		return rwmem_touch((struct rwmem_touch_arg __user *)regs->regs[2]);
	case RWMEM_CMD_DMABUF:
		return rwmem_dmabuf_export((struct rwmem_dmabuf_arg __user *)
					   regs->regs[2]);
#ifdef CONFIG_RWMEM_HIDE
	case RWMEM_CMD_HIDE:
		return hm_hide();
	case RWMEM_CMD_UNHIDE:
		return hm_unhide();
#endif
	default:
		return -ENOSYS;
	}
}

int rwmem_sc_init(void)
{
	struct sc_layout layout = {
		.resolve = kr_name_to_addr,
		.pgd_off = rwmem_pgd_off,
	};
	struct sc_cfg cfg = {0};
	int slot = -1;
	size_t i;
	int ret;

	strscpy(cfg.key, sc_key, sizeof(cfg.key));
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

	pr_info("[rwmem] channel slot=%d key=%s\n", g_slot, sc_key);
	return 0;
}

void rwmem_sc_exit(void)
{
	if (g_slot >= 0)
		sc_unpatch(g_slot);
	g_slot = -1;
	sc_exit();
}
