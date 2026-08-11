# rwMemory

cross-process memory read/write kernel module for ARM64 GKI
kernels. no kernel source needed: layout discovery is BTF first
(type_info) with anchor bootstrap fallback, symbols via
KallRecon. syscall channel built on the Kerncall patch API with
a custom handler: uid and key authenticated, every unauthorized
call keeps the ni_syscall behavior (-ENOSYS), probing the table
cannot tell the channel from an empty slot.

## license

GPL-2.0
