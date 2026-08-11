// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef RWMEM_RWMEM_H
#define RWMEM_RWMEM_H

#define RWMEM_KEY_MAX 64
#define RWMEM_MAX_HANDLES 64
#define RWMEM_MAX_TRANSFER (16UL << 20)

/* hello handled by Kerncall: SC_CMD_HELLO returns SC_MAGIC */
#define RWMEM_CMD_OPEN        0x1001
#define RWMEM_CMD_READ        0x1002
#define RWMEM_CMD_WRITE       0x1003
#define RWMEM_CMD_CLOSE       0x1004

#define RWMEM_CMD_PID_LIST    0x1100
#define RWMEM_CMD_QUERY_MAPS  0x1101
#define RWMEM_CMD_GET_CMDLINE 0x1102

#define RWMEM_CMD_READ_FORCE  0x1200
#define RWMEM_CMD_WRITE_FORCE 0x1201
#define RWMEM_CMD_VECTOR      0x1202

#define RWMEM_VEC_READ 0
#define RWMEM_VEC_WRITE 1

#define RWMEM_MAP_PATH_MAX 256

struct rwmem_map {
	unsigned long start;
	unsigned long end;
	unsigned long flags;
	char path[RWMEM_MAP_PATH_MAX];
};

struct rwmem_iovec {
	unsigned long vaddr;
	unsigned long size;
	unsigned long buf;
};

struct rwmem_cmdline {
	unsigned long arg_start;
	unsigned long arg_end;
};

#endif
