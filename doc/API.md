# rwMemory Kernel API

cross-process memory read/write library for ARM64 GKI. the lib
(lib/rwmem.c + lib/touch.c) is pure functionality: page-table walk,
physical read/write, maps enumeration, cross-process remap, module
base lookup, touch injection. communication is left to the consumer:
link the lib into your own module and pick any channel (syscall,
socket, custom). the only injection the lib requires is a symbol
resolver: implement `kr_name_to_addr(const char *)`. src/ ships one
consumer example (the syscall channel).

## Requirements

- link these objects into your module (see rwMM Makefile):
  deps/Kerncall/lib/sc.o, deps/Kerncall/lib/sc_slide.o,
  deps/hidemod/lib/hidemod.o (when RWMEM_HIDE),
  deps/type_info/lib/*.o,
  deps/type_info/kallrecon/lib/{core,anchor}.o
- provide `kr_name_to_addr(const char *)` (KallRecon wrapper),
  the pointer-injection interface used by the lib for every
  non-exported kernel symbol
- ccflags: -DCONFIG_TI_REMAP -DCONFIG_KERNSC_PATCH
  -DCONFIG_KERNSC_DISCOVER -I lib -I deps/type_info/lib
  -I deps/type_info/kallrecon/lib -I deps/Kerncall/lib
  -I deps/hidemod/lib
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
swapped or reclaimed. needs maple tree (kr_name_to_addr
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
