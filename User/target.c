// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char g_secret[32] = "SECRET123";

int main(void)
{
	int fd = atoi(getenv("TARGET_FD") ? getenv("TARGET_FD") : "-1");

	printf("[target] pid: %d\n", getpid());
	printf("[target] addr: %p\n", (void *)g_secret);
	printf("[target] value: %s\n", g_secret);
	fflush(stdout);

	if (fd >= 0)
		dprintf(fd, "%d %lx\n", getpid(), (unsigned long)g_secret);

	sleep(5);
	printf("[target] value now: %s\n", g_secret);
	return 0;
}
