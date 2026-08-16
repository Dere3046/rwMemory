// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef RWMEM_TOUCH_H
#define RWMEM_TOUCH_H

int rwmem_touch(const struct rwmem_touch_arg __user *arg);
int rwmem_touch_init(void);
void rwmem_touch_exit(void);

#endif
