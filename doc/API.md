# rwMemory Kernel API

cross-process memory read/write library for ARM64 GKI. the lib
(lib/rwmem.c + lib/rwmem_req.c + lib/touch.c + lib/dmabuf.c) is pure
functionality: page-table walk, physical read/write, maps enumeration,
cross-process remap, module base lookup, touch injection, dma-buf
export, plus the request table of the protocol. communication is left
to the consumer: the lib never installs a channel. link it into your
module and pick any channel (syscall slot, socket, cdev, procfs).
src/ ships the syscall channel the rwMemory module itself uses, a
consumer may reuse it or write its own.

## Requirements

- deps.mk exposes the objects (lib/rwmem.o lib/rwmem_req.o
  lib/dmabuf.o lib/touch.o src/rwmem_sc.o), the include dirs (lib and
  src) and the deps (KernCall, Type_info). the transport object is
  only linked in, the slot is patched only when rwmem_sc_init runs
- inject a resolver at init: struct rwmem_resolver { name_to_addr }.
  the lib calls it for every non-exported kernel symbol, the call
  site is __nocfi so a __nocfi resolver works too
- ccflags: -DCONFIG_TI_REMAP -DCONFIG_KERNSC_PATCH
  -DCONFIG_KERNSC_DISCOVER, plus -I for lib, src, the KernCall,
  Type_info and KallRecon include dirs
- layout discovery is BTF first (type_info), anchor bootstrap
  fallback (task struct scan)

## Handle model

open returns a handle id in 0..RWMEM_MAX_HANDLES-1 (64), a
referenced struct pid kept in a ring-allocated table. every
operation resolves the pid via pid_task at call time, so the
process may die or exec between calls. close releases the
reference.

## Layout

all struct offsets are resolved once and cached (BTF via type_info,
anchor scan fallback). missing layout makes a layout-dependent call
return -EOPNOTSUPP (pid_list, get_cmdline) or -EFAULT (read/write
without a resolvable pgd), never fault. query_maps uses compile-time
fields and needs no layout.

## API

**int rwmem_core_init(const struct rwmem_resolver *res)**

store the resolver and bring the mechanism up (touch injection
kprobes, pool allocation). -EINVAL when res or res->name_to_addr is
NULL.

**void rwmem_core_exit(void)**

tear the mechanism down and drop the resolver.

**unsigned long rwmem_name_to_addr(const char *name)**

the resolver the lib was given, for a transport that needs kernel
symbols of its own. 0 when the name is unknown or the lib is not
initialized.

**long rwmem_dispatch(unsigned int cmd, unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3)**

the protocol request table: maps RWMEM_CMD_* plus the four raw
arguments a channel carried onto the lib calls. a transport
authenticates the caller, then hands the command and its four
arguments here, which is the only glue a channel needs. -ENOSYS for
an unknown cmd.

**int rwmem_sc_init(const char *key)**
**void rwmem_sc_exit(void)**

the reference syscall transport (src/): finds an empty ni_syscall
slot, patches it, answers root only with the key authenticated and
forwards to rwmem_dispatch. -EINVAL for an empty key, the KernCall
error otherwise.

**int rwmem_pgd_off(u32 *out)**

byte offset of mm_struct.pgd. used by the channel for the PTE
walk. -ENOENT when BTF and anchor bootstrap both fail.

**int rwmem_open(pid_t pid)**

resolve pid to a handle. -EINVAL for pid <= 0, -ESRCH when the
pid does not exist, -EBUSY when the handle table is full.

**void rwmem_close(int id)**

release the handle. silent for invalid id.

**ssize_t rwmem_read(int id, size_t vaddr, char __user *buf, size_t size)**
**ssize_t rwmem_write(int id, size_t vaddr, const char __user *buf, size_t size)**

page-walk to physical, then read/write through the direct map.
-EINVAL for null buf, size == 0 or size > RWMEM_MAX_TRANSFER
(16 MB). -EBADF for invalid handle. -ENOMEM when the bounce page
cannot be allocated. -EPERM when a write hits a read-only page
(non-force). -EFAULT when the range has no mapping and no byte
was transferred, or a user copy fails. -EIO when the physical
read/write fails. partial transfer: returns the bytes done so far
instead of an error once at least one page moved.

**ssize_t rwmem_read_force(int id, size_t vaddr, char __user *buf, size_t size)**
**ssize_t rwmem_write_force(int id, size_t vaddr, const char __user *buf, size_t size)**

force flips the PTE before the transfer and restores it after
(also when the transfer errors mid-page): writes flip the write
permission (PTE_DBM set, PTE_RDONLY cleared), reads flip the user
permission (PTE_USER). on ARM64 user pages always carry PTE_USER,
so the read path rarely triggers; it exists for symmetry.

**ssize_t rwmem_vector(int id, struct rwmem_iovec __user *vec, size_t count, int mode)**

batch of iovecs (vaddr/size/buf) in one call, RWMEM_VEC_READ or
RWMEM_VEC_WRITE. any mode other than RWMEM_VEC_WRITE is treated
as read. non-force. returns total bytes on success; on error
returns the partial total if something moved, otherwise the
error code.

**ssize_t rwmem_pid_list(pid_t __user *buf, size_t max)**

walk the task list from init_task, copy up to max pids.
-EINVAL (null buf or max == 0), -ENOMEM (allocation failed),
-EOPNOTSUPP without task layout, -ENODATA without init_task,
-ENOENT when the list is empty, -EFAULT on copy failure.
returns the pid count.

**ssize_t rwmem_query_maps(int id, struct rwmem_map __user *out, size_t max, unsigned long start)**

enumerate the mm vm areas. three compile-time modes chosen by the
build: kernels with maple tree (6.1+) iterate with vma_iter_init +
mas_find; older kernels use the vm_next list from mm->mmap (default)
or from find_vma (RWMEM_MAPS_FINDVMA=1). the mtree/iter modes start
from start; the mmap-list mode filters areas ending above start.
each entry: start/end/flags plus the file path resolved through
vma->vm_file->f_path with d_path (empty when anonymous). -EINVAL
(null out or max == 0), -ENOMEM (allocation failed),
-EBADF/-ESRCH for handle/task/mm, -ENOENT when no area matches,
-EFAULT on copy failure. returns the entry count.

**ssize_t rwmem_get_cmdline(int id, struct rwmem_cmdline __user *out)**

returns the mm arg_start/arg_end addresses, not the contents.
the caller reads the range with rwmem_read. -EINVAL (null out),
-EOPNOTSUPP without layout, -EBADF/-ESRCH, -EFAULT on copy
failure.

**int rwmem_remap(const struct rwmem_remap_arg __user *arg)**

map source process memory into the caller's mm, native access
without per-page syscalls. arg: handle, src_vaddr, dst_vaddr
(page aligned), size, writable. builds a VM_PFNMAP special
mapping at dst_vaddr and fills it page by page with
remap_pfn_range from the source physical pages (gaps skipped).
mapped pages are pinned (get_page) for the life of the mapping
and released on unmap, so source memory stays valid even if
swapped or reclaimed. needs maple tree (rwmem_name_to_addr
"mas_find" nonzero, 6.1+), older kernels return -EOPNOTSUPP.
-EINVAL (null arg, bad size, unaligned dst), -EFAULT (bad user
arg), -EBADF (bad handle), -EOPNOTSUPP (symbol missing or kernel
too old), -EADDRINUSE (dst occupied), -ESRCH (no mm), -ENOMEM.

**int rwmem_get_base(const struct rwmem_base_arg __user *arg)**

find the mapping base of a file-backed module by name. arg:
handle, name[64], out. walks the vmas (vma_iter_init+mas_find on
6.1+, mm->mmap+vm_next below) and matches dentry->d_name.name.
returns 0 and fills out with vm_start, -ENOENT when the module is
not mapped. -EINVAL/-EFAULT/-EBADF/-ESRCH as usual.

**int rwmem_touch(const struct rwmem_touch_arg __user *arg)**

inject touch events into the kernel input core. arg: cmd
(RWMEM_TOUCH_DOWN/MOVE/UP), x, y, slot. finds the touch input_dev
by capability (EV_ABS + ABS_MT_POSITION_X/ABS_X), queues events in
a pool (flushed on SYN by input_event/input_inject_event kprobes)
and injects via input_handle_event. DOWN allocates a tracking id
via input_mt_new_trkid, MOVE reuses it, UP releases. -EINVAL (bad
arg or input not initialized), -ENODEV (no touch device),
-EOPNOTSUPP (input_handle_event unresolved).

**int rwmem_dmabuf_export(const struct rwmem_dmabuf_arg __user *arg)**

export source process memory as a dma-buf. arg: handle, vaddr,
size, out_fd. pins the source pages (get_page), builds a dma-buf
(sg_table + mmap fault mapping), returns an fd the caller can
mmap(MAP_SHARED) for zero-copy native access, or hand to other
processes/devices. -EINVAL (bad arg), -EBADF (bad handle), -ESRCH
(no task/mm), -ENOMEM, -EFAULT (no mapped pages). requires
MODULE_IMPORT_NS(DMA_BUF).
