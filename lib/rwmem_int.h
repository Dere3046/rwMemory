// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef RWMEM_INT_H
#define RWMEM_INT_H

/*
 * every kernel address the lib needs goes through the resolver the
 * consumer injected, the call site is __nocfi so a __nocfi resolver
 * works too
 */
unsigned long rw_resolve(const char *name);

#endif
