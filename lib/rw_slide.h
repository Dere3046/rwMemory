// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef RWMEM_SLIDE_H
#define RWMEM_SLIDE_H

#include <linux/types.h>

extern unsigned int rw_slide_buf[];

struct rw_slide_win {
	unsigned long addr;
	unsigned int  chunksz;
	unsigned int  margin;
	unsigned int  off;
};

int rw_slide_init(struct rw_slide_win *w, unsigned long pos,
		  unsigned int chunksz, unsigned int margin);
int rw_slide_advance(struct rw_slide_win *w, unsigned int n);

static inline void *rw_slide_ptr(const struct rw_slide_win *w,
				 const void *buf)
{
	return (unsigned char *)buf + w->off;
}

static inline unsigned long rw_slide_addr(const struct rw_slide_win *w)
{
	return w->addr + w->off;
}

#endif
