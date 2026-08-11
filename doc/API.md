# rwMemory Kernel API

cross-process memory read/write library for ARM64 GKI. the lib
(lib/rwmem.c) is pure functionality: page-table walk, physical
read/write, maps enumeration. link the lib into your own module;
the syscall channel (src/) is one consumer of the lib.

## Requirements

- link these objects into your module (see rwMM Makefile):
  deps/Kerncall/lib/sc.o, deps/Kerncall/lib/sc_slide.o,
  deps/hidemod/lib/hidemod.o (when RWMEM_HIDE),
  deps/type_info/lib/*.o,
  deps/type_info/kallrecon/lib/{core,anchor}.o
- provide `kr_name_to_addr(const char *)` (KallRecon wrapper)
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

## Behavior notes

- physical access: get_proc_phy_addr walks the process page
  table, then read/write through __va with rw_safe_read / memcpy,
  page by page
- all kernel reads go through rw_safe_read (copy_from_kernel_nofault
  wrapper); a failed page walk stops the transfer, never crashes
- BTF layout is authoritative; anchor scan only fills the gaps
  (pgd, task offsets) on kernels without usable BTF
- lib/rw_slide.c is gone: the sliding-window reader now lives in
  Kerncall as sc_slide (lib/sc_slide.c), used by the channel's
  page-table walk, link deps/Kerncall/lib/sc_slide.o with sc.o
- module hiding (hidemod) is opt-in: build with RWMEM_HIDE=1 to
  enable the HIDE/UNHIDE channel commands and auto-register the
  module for hiding; default builds ship no hiding at all
- maps iteration is selectable at build time: the branch follows the
  kernel headers used, headers with maple_tree.h (6.1+) take
  vma_iter_init + mas_find; headers without it (5.10/5.15) use the
  vm_next list from mm->mmap, or from find_vma when built with
  RWMEM_MAPS_FINDVMA=1
