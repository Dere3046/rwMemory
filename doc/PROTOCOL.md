# rwMemory User Protocol

syscall-based cross-process memory access. the module patches a
syscall slot with its own handler; the handler authenticates and
routes commands to the rwmem lib.

## Channel

the handler keeps the ni_syscall behavior for every unauthorized
call: uid check first (non-root returns instantly), key check
second, any failure returns -ENOSYS. probing all 512 slots cannot
tell the channel apart from an empty slot. the only visible
response is a successful handshake below.

slot selection order (kernel side):
1. 249, then fallbacks 42, 18, 415 (first slot whose table entry
   is ni_syscall wins)
2. leftmost free slot scan as last resort

userspace discovery must follow the same order and probe only
known empty candidates. never scan 0..511: real syscalls execute
(93 = exit kills the process).

## Handshake

    syscall(slot, key, 0x1000, 0, 0, 0, 0) == 0x53434831UL

key is the module parameter (default "rwmem", root readable,
0400). 0x1000 is SC_CMD_HELLO, 0x53434831 is SC_MAGIC, both from
the Kerncall protocol. discover the slot by probing the preference
list then the constant-empty candidates with the handshake.

## Conventions

- syscall arguments: regs[0] = key, regs[1] = cmd, regs[2..5] =
  command arguments
- return values are raw long; negative values are -errno
  (userspace syscall() maps them to -1/errno)
- RWMEM_MAX_TRANSFER 16 MB per transfer, RWMEM_MAX_HANDLES 64

## Commands

### open 0x1001

    regs[2] = pid

returns a handle id, -EINVAL (pid <= 0), -ESRCH (no such pid),
-EBUSY (handle table full).

### read 0x1002 / write 0x1003

    regs[2] = handle, regs[3] = vaddr, regs[4] = size, regs[5] = buf

-EINVAL (bad args or size > 16 MB), -EBADF (bad handle), -EPERM
(write to read-only page), -EFAULT (no mapping, nothing moved),
-EIO (physical access failed). partial transfers return the bytes
moved.

### close 0x1004

    regs[2] = handle

returns 0.

### pid_list 0x1100

    regs[3] = pid buffer, regs[4] = max

returns the pid count, -EOPNOTSUPP/-ENODATA/-ENOENT/-EFAULT.

### query_maps 0x1101

    regs[2] = handle, regs[3] = rwmem_map buffer, regs[4] = max,
    regs[5] = start address

returns the entry count. struct rwmem_map:
    unsigned long start, end, flags; char path[64]

-EOPNOTSUPP/-EBADF/-ESRCH/-ENOENT/-EFAULT.

### get_cmdline 0x1102

    regs[2] = handle, regs[3] = rwmem_cmdline buffer

struct rwmem_cmdline: unsigned long arg_start, arg_end.
addresses only; read the range with cmd 0x1002.

### read_force 0x1200 / write_force 0x1201

same layout as read/write. force flips the PTE: write force
bypasses read-only pages, read force flips the user permission
(rarely triggered on ARM64).

### vector 0x1202

    regs[2] = handle, regs[3] = rwmem_iovec buffer, regs[4] = count,
    regs[5] = mode (0 read, 1 write)

struct rwmem_iovec: unsigned long vaddr, size, buf. non-force.
returns total bytes, partial total on error, else the error code.

## Error codes

-ENOSYS on any unauthorized call (wrong key, non-root, unknown
command) keeps the empty-slot behavior. authorized calls surface
the lib error codes above.
