// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/printk.h>

#include "rwmem.h"
#include "rw_slide.h"

#define RWMEM_SLIDE_BUF_WORDS (18 * 1024)
unsigned int rw_slide_buf[RWMEM_SLIDE_BUF_WORDS];

int rw_slide_init(struct rw_slide_win *w, unsigned long pos,
		  unsigned int chunksz, unsigned int margin)
{
	w->chunksz = chunksz;
	w->margin = margin;
	w->addr = pos;

	if (rw_safe_read(rw_slide_buf, (void *)w->addr, chunksz + margin))
		return -1;

	w->off = 0;
	return 0;
}

int rw_slide_advance(struct rw_slide_win *w, unsigned int n)
{
	w->off += n;

	if (w->off >= w->chunksz) {
		unsigned long cursor = w->addr + w->off;
		unsigned long new_addr = (cursor - w->margin) & ~0xFFFULL;

		w->addr = new_addr;
		if (rw_safe_read(rw_slide_buf, (void *)w->addr,
				 w->chunksz + w->margin))
			return -1;
		w->off = cursor - w->addr;
	}
	return 0;
}
