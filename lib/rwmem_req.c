// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/types.h>
#include <linux/uaccess.h>

#include "rwmem.h"

long rwmem_dispatch(unsigned int cmd, unsigned long a0, unsigned long a1,
		    unsigned long a2, unsigned long a3)
{
	switch (cmd) {
	case RWMEM_CMD_OPEN:
		return rwmem_open((pid_t)a0);
	case RWMEM_CMD_CLOSE:
		rwmem_close((int)a0);
		return 0;
	case RWMEM_CMD_READ:
		return rwmem_read((int)a0, (size_t)a1, (char __user *)a3,
				  (size_t)a2);
	case RWMEM_CMD_WRITE:
		return rwmem_write((int)a0, (size_t)a1, (const char __user *)a3,
				   (size_t)a2);
	case RWMEM_CMD_READ_FORCE:
		return rwmem_read_force((int)a0, (size_t)a1,
					(char __user *)a3, (size_t)a2);
	case RWMEM_CMD_WRITE_FORCE:
		return rwmem_write_force((int)a0, (size_t)a1,
					 (const char __user *)a3, (size_t)a2);
	case RWMEM_CMD_PID_LIST:
		return rwmem_pid_list((pid_t __user *)a1, (size_t)a2);
	case RWMEM_CMD_QUERY_MAPS:
		return rwmem_query_maps((int)a0, (struct rwmem_map __user *)a1,
					(size_t)a2, (unsigned long)a3);
	case RWMEM_CMD_GET_CMDLINE:
		return rwmem_get_cmdline((int)a0,
					 (struct rwmem_cmdline __user *)a1);
	case RWMEM_CMD_VECTOR:
		return rwmem_vector((int)a0, (struct rwmem_iovec __user *)a1,
				    (size_t)a2, (int)a3);
	case RWMEM_CMD_REMAP:
		return rwmem_remap((struct rwmem_remap_arg __user *)a0);
	case RWMEM_CMD_GET_BASE:
		return rwmem_get_base((struct rwmem_base_arg __user *)a0);
	case RWMEM_CMD_TOUCH:
		return rwmem_touch((struct rwmem_touch_arg __user *)a0);
	case RWMEM_CMD_DMABUF:
		return rwmem_dmabuf_export((struct rwmem_dmabuf_arg __user *)a0);
	default:
		return -ENOSYS;
	}
}
