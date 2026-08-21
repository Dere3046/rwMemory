// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef RWMEM_PHY_MEM_H
#define RWMEM_PHY_MEM_H

#include <linux/types.h>

#include "rwmem_proto.h"

int rw_safe_read(void *dst, const void *src, size_t sz);

struct pid *rwmem_handle_get(int id);
size_t rwmem_phy_addr(struct mm_struct *mm, size_t vaddr, pte_t **out_pte);

int rwmem_pgd_off(u32 *out);
int rwmem_open(pid_t pid);
void rwmem_close(int id);
ssize_t rwmem_read(int id, size_t vaddr, char __user *buf, size_t size);
ssize_t rwmem_write(int id, size_t vaddr, const char __user *buf, size_t size);
ssize_t rwmem_read_force(int id, size_t vaddr, char __user *buf, size_t size);
ssize_t rwmem_write_force(int id, size_t vaddr, const char __user *buf,
			  size_t size);
ssize_t rwmem_vector(int id, struct rwmem_iovec __user *vec, size_t count,
		     int mode);
ssize_t rwmem_pid_list(pid_t __user *buf, size_t max);
ssize_t rwmem_query_maps(int id, struct rwmem_map __user *out, size_t max,
			 unsigned long start);
ssize_t rwmem_get_cmdline(int id, struct rwmem_cmdline __user *out);
int rwmem_remap(const struct rwmem_remap_arg __user *arg);
int rwmem_get_base(const struct rwmem_base_arg __user *arg);
int rwmem_touch(const struct rwmem_touch_arg __user *arg);
int rwmem_dmabuf_export(const struct rwmem_dmabuf_arg __user *arg);

#endif
