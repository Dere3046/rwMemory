# rwMemory

cross-process memory read/write kernel module for ARM64 GKI
kernels. no kernel source needed: layout discovery is BTF first
(type_info) with anchor bootstrap fallback, symbols via
KallRecon. syscall channel built on the Kerncall patch API with
a custom handler: uid and key authenticated, every unauthorized
call keeps the ni_syscall behavior (-ENOSYS), probing the table
cannot tell the channel from an empty slot.

## layout

- lib/: pure functionality (rwmem.c page walk + physical
  read/write; rw_slide.c optional sliding-window reader)
- src/: module entry + syscall channel wiring (custom handler,
  slot 249 with cross-zone fallbacks, stealth)
- User/: userspace example (target + attacker)
- doc/API.md: kernel API for LKM consumers
- doc/PROTOCOL.md: userspace syscall protocol

## usage

clone deps into deps/ first (Kerncall, type_info plus KallRecon
inside type_info), then `make KDIR=/path/to/kernel-source`.
verified on 7 KMI versions (android12-5.10 to android16-6.12),
CI builds the full matrix.

insmod rwmem.ko key=<your key> (default "rwmem"). userspace
calls syscall(slot, key, cmd, ...), see doc/PROTOCOL.md. slot
selection: 249, fallbacks 42/18/415, leftmost scan.

## license

GPL-2.0
