// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/string.h>
#include <asm/pgtable.h>
#include <asm/io.h>

#include "core.h"
#include "type_info.h"
#include "anchor.h"
#include "btf.h"
#include "rwmem.h"

extern unsigned long kr_name_to_addr(const char *name);

#define RWMEM_STRUCT BIT(BTF_KIND_STRUCT)

int rw_safe_read(void *dst, const void *src, size_t sz)
{
	return copy_from_kernel_nofault(dst, src, sz);
}

static struct pid *g_handles[RWMEM_MAX_HANDLES];
static DEFINE_MUTEX(g_handle_lock);
static u32 g_handle_next;

static long g_off_pgd = -1;
static long g_off_pid = -1;
static long g_off_tasks = -1;
static long g_off_mm = -1;
static long g_off_arg_start = -1;
static long g_off_map_count = -1;

static int off_from_btf(const char *type, const char *member, long *out)
{
	u32 id;
	u32 bit_off;
	u32 bit_sz;

	if (ti_type_by_name(ti_base(), type, RWMEM_STRUCT, &id))
		return -ENOENT;
	if (ti_member_off(ti_base(), id, member, &bit_off, &bit_sz))
		return -ENOENT;
	*out = bit_off / 8;
	return 0;
}

static bool is_task_size_cand(unsigned long v)
{
	if (v == TASK_SIZE)
		return true;
	if (v == (1UL << 39) || v == (1UL << 48) || v == (1UL << 52))
		return true;
	return false;
}

static int pgd_off_from_anchor(u32 *out)
{
	struct mm_struct *mm;
	unsigned long v;
	unsigned long task_size_pos = 0;
	long i;
	int ret = -ENOENT;

	mm = get_task_mm(current);
	if (!mm)
		return -ENOENT;

	for (i = 0; i < 256; i++) {
		if (rw_safe_read(&v, (char *)mm + i * 8, 8))
			break;
		if (is_task_size_cand(v)) {
			task_size_pos = i * 8;
			break;
		}
	}
	if (task_size_pos) {
		for (i = task_size_pos + 8; i < task_size_pos + 64; i += 8) {
			if (rw_safe_read(&v, (char *)mm + i, 8))
				break;
			if (v >= 0xffff000000000000UL &&
			    !(v & (PAGE_SIZE - 1))) {
				*out = (u32)i;
				ret = 0;
				break;
			}
		}
	}
	mmput(mm);
	return ret;
}

static int off_from_anchor_task(const char *member, long *out)
{
	struct ti_boot_args args;
	struct ti_task_offs to;
	struct task_struct *cur = current;

	memset(&args, 0, sizeof(args));
	args.pid = task_pid_nr(cur);
	args.tgid = task_tgid_nr(cur);
	memcpy((void *)args.comm, cur->comm, sizeof(args.comm));
	args.ref_pid = 1;
	args.ref_tgid = 1;
	if (ti_bootstrap_task(&args, &to))
		return -ENOENT;
	if (!strcmp(member, "pid")) {
		if (!to.off_pid)
			return -ENOENT;
		*out = to.off_pid;
		return 0;
	}
	if (!strcmp(member, "tasks")) {
		if (!to.off_tasks)
			return -ENOENT;
		*out = to.off_tasks;
		return 0;
	}
	if (!strcmp(member, "mm")) {
		if (!to.off_mm)
			return -ENOENT;
		*out = to.off_mm;
		return 0;
	}
	if (!strcmp(member, "pgd")) {
		u32 off;

		if (!pgd_off_from_anchor(&off)) {
			*out = off;
			return 0;
		}
		if (!to.mm_pgd) {
			*out = to.mm_pgd;
			return 0;
		}
		return -ENOENT;
	}
	if (!strcmp(member, "arg_start")) {
		if (!to.mm_arg_start)
			return -ENOENT;
		*out = to.mm_arg_start;
		return 0;
	}
	return -ENOENT;
}

static int get_off(long *cache, const char *type, const char *member)
{
	if (*cache >= 0)
		return 0;
	if (!off_from_btf(type, member, cache))
		goto done;
	if (!strcmp(type, "task_struct")) {
		if (!off_from_anchor_task(member, cache))
			goto done;
	} else if (!strcmp(type, "mm_struct") &&
		   (!strcmp(member, "pgd") || !strcmp(member, "arg_start"))) {
		if (!off_from_anchor_task(member, cache))
			goto done;
	}
	*cache = -1;
	return -ENOENT;
done:
	pr_info("[rwmem] %s.%s off=%ld\n", type, member, *cache);
	return 0;
}

struct pid *rwmem_handle_get(int id)
{
	struct pid *pid = NULL;

	if (id < 0 || id >= RWMEM_MAX_HANDLES)
		return NULL;
	mutex_lock(&g_handle_lock);
	if (g_handles[id])
		pid = get_pid(g_handles[id]);
	mutex_unlock(&g_handle_lock);
	return pid;
}

int rwmem_pgd_off(u32 *out)
{
	if (get_off(&g_off_pgd, "mm_struct", "pgd"))
		return -ENOENT;
	*out = (u32)g_off_pgd;
	return 0;
}

int rwmem_open(pid_t pid)
{
	int id = -EBUSY;
	u32 i;

	if (pid <= 0)
		return -EINVAL;
	mutex_lock(&g_handle_lock);
	for (i = 0; i < RWMEM_MAX_HANDLES; i++) {
		int slot = (g_handle_next + i) % RWMEM_MAX_HANDLES;

		if (g_handles[slot])
			continue;
		g_handles[slot] = find_get_pid(pid);
		if (!g_handles[slot]) {
			id = -ESRCH;
			break;
		}
		g_handle_next = slot + 1;
		id = slot;
		break;
	}
	mutex_unlock(&g_handle_lock);
	return id;
}

void rwmem_close(int id)
{
	struct pid *pid = NULL;

	if (id < 0 || id >= RWMEM_MAX_HANDLES)
		return;
	mutex_lock(&g_handle_lock);
	if (g_handles[id]) {
		pid = g_handles[id];
		g_handles[id] = NULL;
	}
	mutex_unlock(&g_handle_lock);
	if (pid)
		put_pid(pid);
}

static size_t size_inside_page(unsigned long start, unsigned long size)
{
	unsigned long sz = PAGE_SIZE - (start & (PAGE_SIZE - 1));

	return min(sz, size);
}

size_t rwmem_phy_addr(struct mm_struct *mm, size_t vaddr,
			      pte_t **out_pte)
{
	pgd_t *pgd;
	pgd_t *pgde;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	size_t paddr = 0;

	if (out_pte)
		*out_pte = NULL;

	if (get_off(&g_off_pgd, "mm_struct", "pgd"))
		return 0;
	pgd = *(pgd_t **)((char *)mm + g_off_pgd);
	if (!pgd)
		return 0;
	pgde = pgd + pgd_index(vaddr);
	if (pgd_none(*pgde))
		return 0;
	p4d = p4d_offset(pgde, vaddr);
	if (p4d_none(*p4d))
		return 0;
	pud = pud_offset(p4d, vaddr);
	if (pud_none(*pud))
		return 0;
	pmd = pmd_offset(pud, vaddr);
	if (pmd_none(*pmd))
		return 0;
	if (pmd_leaf(*pmd))
		return 0;
	pte = pte_offset_kernel(pmd, vaddr);
	if (pte_none(*pte))
		return 0;

	paddr = page_to_phys(pte_page(*pte)) | (vaddr & ~PAGE_MASK);
	if (out_pte)
		*out_pte = pte;
	return paddr;
}

static size_t get_proc_phy_addr(struct pid *pid, size_t vaddr, pte_t **out_pte)
{
	struct task_struct *task;
	struct mm_struct *mm;
	size_t paddr;

	task = pid_task(pid, PIDTYPE_PID);
	if (!task)
		return 0;
	mm = get_task_mm(task);
	if (!mm)
		return 0;
	paddr = rwmem_phy_addr(mm, vaddr, out_pte);
	mmput(mm);
	return paddr;
}

static size_t read_ram_physical(size_t paddr, char *buf, size_t size)
{
	size_t done = 0;

	while (size > 0) {
		size_t sz = size_inside_page(paddr, size);

		if (rw_safe_read(buf, __va(paddr), sz))
			break;
		buf += sz;
		paddr += sz;
		size -= sz;
		done += sz;
	}
	return done;
}

static size_t write_ram_physical(size_t paddr, const char *buf, size_t size)
{
	size_t done = 0;

	while (size > 0) {
		size_t sz = size_inside_page(paddr, size);

		memcpy(__va(paddr), buf, sz);
		buf += sz;
		paddr += sz;
		size -= sz;
		done += sz;
	}
	return done;
}

static ssize_t rwmem_rw_mm(struct mm_struct *mm, size_t vaddr, char __user *buf,
			   size_t size, bool write, bool force)
{
	char *bounce;
	pte_t *pte;
	pte_t orig;
	bool flipped;
	size_t done = 0;
	int ret;

	if (!buf || !size)
		return -EINVAL;
	if (size > RWMEM_MAX_TRANSFER)
		return -EINVAL;

	bounce = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	while (done < size) {
		size_t phy;
		size_t page_left;

		phy = rwmem_phy_addr(mm, vaddr + done, &pte);
		if (!phy)
			break;
		page_left = size_inside_page(phy, size - done);
		if (page_left > PAGE_SIZE)
			page_left = PAGE_SIZE;

		orig = *pte;
		flipped = false;
		if (force && !write && !(pte_val(*pte) & PTE_USER)) {
			set_pte(pte, __pte(pte_val(*pte) | PTE_USER));
			dsb(ish);
			flipped = true;
		} else if (write && !force && !pte_write(*pte)) {
			if (done)
				goto out_done;
			ret = -EPERM;
			goto out;
		} else if (force && write && !pte_write(*pte)) {
			set_pte(pte, __pte((pte_val(*pte) | PTE_DBM) &
					    ~PTE_RDONLY));
			dsb(ish);
			flipped = true;
		}

		if (write) {
			if (copy_from_user(bounce, buf + done, page_left)) {
				ret = -EFAULT;
				goto out_restore;
			}
			if (!write_ram_physical(phy, bounce, page_left)) {
				ret = -EIO;
				goto out_restore;
			}
		} else {
			if (!read_ram_physical(phy, bounce, page_left)) {
				ret = -EIO;
				goto out_restore;
			}
			if (copy_to_user(buf + done, bounce, page_left)) {
				ret = -EFAULT;
				goto out_restore;
			}
		}

		if (flipped)
			set_pte(pte, orig);
		done += page_left;
	}
out_done:
	ret = done ? (ssize_t)done : -EFAULT;
out:
	kfree(bounce);
	return ret;
out_restore:
	if (flipped)
		set_pte(pte, orig);
	goto out;
}

static ssize_t rwmem_rw(int id, size_t vaddr, char __user *buf, size_t size,
			bool write, bool force)
{
	struct pid *pid;
	struct task_struct *task;
	struct mm_struct *mm;
	ssize_t ret;

	if (!buf || !size)
		return -EINVAL;
	if (size > RWMEM_MAX_TRANSFER)
		return -EINVAL;
	pid = rwmem_handle_get(id);
	if (!pid)
		return -EBADF;
	task = pid_task(pid, PIDTYPE_PID);
	if (!task) {
		ret = -ESRCH;
		goto out_pid;
	}
	mm = get_task_mm(task);
	if (!mm) {
		ret = -ESRCH;
		goto out_pid;
	}

	mmap_read_lock(mm);
	ret = rwmem_rw_mm(mm, vaddr, buf, size, write, force);
	mmap_read_unlock(mm);
	mmput(mm);
out_pid:
	put_pid(pid);
	return ret;
}

ssize_t rwmem_read(int id, size_t vaddr, char __user *buf, size_t size)
{
	return rwmem_rw(id, vaddr, buf, size, false, false);
}

ssize_t rwmem_write(int id, size_t vaddr, const char __user *buf, size_t size)
{
	return rwmem_rw(id, vaddr, (char __user *)buf, size, true, false);
}

ssize_t rwmem_read_force(int id, size_t vaddr, char __user *buf, size_t size)
{
	return rwmem_rw(id, vaddr, buf, size, false, true);
}

ssize_t rwmem_write_force(int id, size_t vaddr, const char __user *buf,
			  size_t size)
{
	return rwmem_rw(id, vaddr, (char __user *)buf, size, true, true);
}

ssize_t rwmem_vector(int id, struct rwmem_iovec __user *vec, size_t count,
		     int mode)
{
	struct pid *pid;
	struct task_struct *task;
	struct mm_struct *mm;
	ssize_t total = 0;
	size_t i;

	if (!vec || !count)
		return -EINVAL;
	pid = rwmem_handle_get(id);
	if (!pid)
		return -EBADF;
	task = pid_task(pid, PIDTYPE_PID);
	if (!task) {
		put_pid(pid);
		return -ESRCH;
	}
	mm = get_task_mm(task);
	if (!mm) {
		put_pid(pid);
		return -ESRCH;
	}

	mmap_read_lock(mm);
	for (i = 0; i < count; i++) {
		struct rwmem_iovec iv;
		ssize_t n;

		if (copy_from_user(&iv, &vec[i], sizeof(iv))) {
			total = total ? total : -EFAULT;
			break;
		}
		if (!iv.buf || !iv.size || iv.size > RWMEM_MAX_TRANSFER) {
			total = total ? total : -EINVAL;
			break;
		}
		if (mode == RWMEM_VEC_WRITE)
			n = rwmem_rw_mm(mm, iv.vaddr, (char __user *)iv.buf,
					iv.size, true, false);
		else
			n = rwmem_rw_mm(mm, iv.vaddr, (char __user *)iv.buf,
					iv.size, false, false);
		if (n < 0) {
			total = total ? total : n;
			break;
		}
		total += n;
	}
	mmap_read_unlock(mm);
	mmput(mm);
	put_pid(pid);
	return total;
}

ssize_t rwmem_pid_list(pid_t __user *buf, size_t max)
{
	struct task_struct *init_task_ptr;
	struct task_struct *task;
	pid_t *klist;
	size_t cnt = 0;
	int ret;

	if (!buf || !max)
		return -EINVAL;
	if (get_off(&g_off_pid, "task_struct", "pid") ||
	    get_off(&g_off_tasks, "task_struct", "tasks"))
		return -EOPNOTSUPP;

	init_task_ptr = (struct task_struct *)kr_name_to_addr("init_task");
	if (!init_task_ptr)
		return -ENODATA;

	klist = kmalloc_array(max, sizeof(pid_t), GFP_KERNEL);
	if (!klist)
		return -ENOMEM;

	rcu_read_lock();
	list_for_each_entry_rcu(task, &init_task_ptr->tasks, tasks) {
		if (cnt >= max)
			break;
		klist[cnt++] = *(pid_t *)((char *)task + g_off_pid);
	}
	rcu_read_unlock();

	ret = cnt ? (ssize_t)cnt : -ENOENT;
	if (copy_to_user(buf, klist, cnt * sizeof(pid_t)))
		ret = -EFAULT;
	kfree(klist);
	return ret;
}

static int map_path(struct vm_area_struct *vma, char *out, size_t outsz)
{
	struct file *file;
	struct path *pathp;

	file = vma->vm_file;
	if (!file)
		return -ENOENT;
	pathp = &file->f_path;
	if (IS_ERR(d_path(pathp, out, outsz)))
		return -ENOENT;
	out[outsz - 1] = 0;
	return 0;
}

#ifdef CONFIG_RWMEM_MAPS_FINDVMA
extern struct vm_area_struct *find_vma(struct mm_struct *mm,
				       unsigned long addr);
#endif

ssize_t rwmem_query_maps(int id, struct rwmem_map __user *out, size_t max,
			 unsigned long start)
{
	struct pid *pid;
	struct task_struct *task;
	struct mm_struct *mm;
	struct rwmem_map *klist;
	size_t cnt = 0;
	ssize_t ret;

	if (!out || !max)
		return -EINVAL;
	if (max > RWMEM_MAPS_MAX)
		max = RWMEM_MAPS_MAX;

	pid = rwmem_handle_get(id);
	if (!pid)
		return -EBADF;
	task = pid_task(pid, PIDTYPE_PID);
	if (!task) {
		ret = -ESRCH;
		goto out_pid;
	}
	mm = get_task_mm(task);
	if (!mm) {
		ret = -ESRCH;
		goto out_pid;
	}

	klist = kmalloc_array(max, sizeof(struct rwmem_map), GFP_KERNEL);
	if (!klist) {
		ret = -ENOMEM;
		goto out_mm;
	}

#ifdef _LINUX_MAPLE_TREE_H
	{
		struct vma_iterator vmi;
		struct vm_area_struct *vma;

		vma_iter_init(&vmi, mm, start);
		mmap_read_lock(mm);
		vma = mas_find(&vmi.mas, ULONG_MAX);
		while (vma && cnt < max) {
			klist[cnt].start = vma->vm_start;
			klist[cnt].end = vma->vm_end;
			klist[cnt].flags = vma->vm_flags;
			memset(klist[cnt].path, 0, sizeof(klist[cnt].path));
			map_path(vma, klist[cnt].path, sizeof(klist[cnt].path));
			cnt++;
			vma = mas_find(&vmi.mas, ULONG_MAX);
		}
		mmap_read_unlock(mm);
	}
#elif defined(CONFIG_RWMEM_MAPS_FINDVMA)
	{
		struct vm_area_struct *vma;

		mmap_read_lock(mm);
		vma = find_vma(mm, start);
		while (vma && cnt < max) {
			klist[cnt].start = vma->vm_start;
			klist[cnt].end = vma->vm_end;
			klist[cnt].flags = vma->vm_flags;
			memset(klist[cnt].path, 0, sizeof(klist[cnt].path));
			map_path(vma, klist[cnt].path, sizeof(klist[cnt].path));
			cnt++;
			vma = vma->vm_next;
		}
		mmap_read_unlock(mm);
	}
#else
	{
		struct vm_area_struct *vma;

		mmap_read_lock(mm);
		vma = mm->mmap;
		while (vma && cnt < max) {
			if (vma->vm_end > start) {
				klist[cnt].start = vma->vm_start;
				klist[cnt].end = vma->vm_end;
				klist[cnt].flags = vma->vm_flags;
				memset(klist[cnt].path, 0,
				       sizeof(klist[cnt].path));
				map_path(vma, klist[cnt].path,
					 sizeof(klist[cnt].path));
				cnt++;
			}
			vma = vma->vm_next;
		}
		mmap_read_unlock(mm);
	}
#endif

	ret = cnt ? (ssize_t)cnt : -ENOENT;
	if (copy_to_user(out, klist, cnt * sizeof(struct rwmem_map)))
		ret = -EFAULT;
	kfree(klist);
out_mm:
	mmput(mm);
out_pid:
	put_pid(pid);
	return ret;
}

ssize_t rwmem_get_cmdline(int id, struct rwmem_cmdline __user *out)
{
	struct pid *pid;
	struct task_struct *task;
	struct mm_struct *mm;
	struct rwmem_cmdline cl;
	ssize_t ret;

	if (!out)
		return -EINVAL;
	if (get_off(&g_off_arg_start, "mm_struct", "arg_start"))
		return -EOPNOTSUPP;

	pid = rwmem_handle_get(id);
	if (!pid)
		return -EBADF;
	task = pid_task(pid, PIDTYPE_PID);
	if (!task) {
		ret = -ESRCH;
		goto out_pid;
	}
	mm = get_task_mm(task);
	if (!mm) {
		ret = -ESRCH;
		goto out_pid;
	}

	cl.arg_start = *(unsigned long *)((char *)mm + g_off_arg_start);
	cl.arg_end = *(unsigned long *)((char *)mm + g_off_arg_start + 8);
	ret = copy_to_user(out, &cl, sizeof(cl)) ? -EFAULT : 0;
	mmput(mm);
out_pid:
	put_pid(pid);
	return ret;
}

struct rwmem_remap_ctx {
	struct pid *src_pid;
	unsigned long src_base;
	unsigned long dst_base;
	size_t size;
	struct page **pages;
	size_t npages;
};

static vm_fault_t rwmem_remap_fault(const struct vm_special_mapping *sm,
				    struct vm_area_struct *vma,
				    struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}

static void rwmem_remap_close(struct vm_area_struct *vma)
{
	struct rwmem_remap_ctx *ctx = vma->vm_private_data;
	size_t i;

	if (ctx) {
		for (i = 0; i < ctx->npages; i++)
			if (ctx->pages[i])
				put_page(ctx->pages[i]);
		kfree(ctx->pages);
		put_pid(ctx->src_pid);
		kfree(ctx);
		vma->vm_private_data = NULL;
	}
}

static const struct vm_operations_struct rwmem_remap_vm_ops = {
	.close = rwmem_remap_close,
};

static struct vm_area_struct *(*g_install_special_mapping)(struct mm_struct *,
							   unsigned long,
							   unsigned long,
							   unsigned long,
							   const struct vm_special_mapping *);
static int (*g_remap_pfn_range)(struct vm_area_struct *, unsigned long,
				unsigned long, unsigned long, pgprot_t);

static bool rwmem_remap_supported(void)
{
	return kr_name_to_addr("mas_find") != 0;
}

int __nocfi rwmem_remap(const struct rwmem_remap_arg __user *arg)
{
	struct rwmem_remap_arg karg;
	struct rwmem_remap_ctx *ctx;
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	struct pid *pid;
	unsigned long flags;
	pgprot_t prot;
	size_t off;
	int ret;

	if (!arg)
		return -EINVAL;
	if (!rwmem_remap_supported())
		return -EOPNOTSUPP;
	if (copy_from_user(&karg, arg, sizeof(karg)))
		return -EFAULT;
	if (!karg.size || karg.size > RWMEM_MAX_TRANSFER ||
	    (karg.dst_vaddr & ~PAGE_MASK))
		return -EINVAL;
	karg.size = round_up(karg.size, PAGE_SIZE);

	pid = rwmem_handle_get(karg.handle);
	if (!pid)
		return -EBADF;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto out_pid;
	}
	ctx->src_pid = get_pid(pid);
	ctx->src_base = karg.src_vaddr;
	ctx->dst_base = karg.dst_vaddr;
	ctx->size = karg.size;

	if (!g_install_special_mapping) {
		g_install_special_mapping =
			(void *)kr_name_to_addr("_install_special_mapping");
		if (!g_install_special_mapping) {
			ret = -EOPNOTSUPP;
			goto out_ctx;
		}
	}
	if (!g_remap_pfn_range) {
		g_remap_pfn_range = (void *)kr_name_to_addr("remap_pfn_range");
		if (!g_remap_pfn_range) {
			ret = -EOPNOTSUPP;
			goto out_ctx;
		}
	}

	mm = current->mm;
	if (!mm) {
		ret = -ESRCH;
		goto out_ctx;
	}
	mmap_write_lock(mm);
	if (find_vma(mm, karg.dst_vaddr) &&
	    find_vma(mm, karg.dst_vaddr)->vm_start <
		    karg.dst_vaddr + karg.size) {
		mmap_write_unlock(mm);
		ret = -EADDRINUSE;
		goto out_ctx;
	}
	flags = VM_READ | VM_SHARED | VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC |
		VM_PFNMAP;
	if (karg.writable)
		flags |= VM_WRITE;

	vma = g_install_special_mapping(mm, karg.dst_vaddr, karg.size,
					flags, &(struct vm_special_mapping){
						.name = "rwmem_map",
						.fault = rwmem_remap_fault,
					});
	if (IS_ERR(vma)) {
		mmap_write_unlock(mm);
		ret = PTR_ERR(vma);
		goto out_ctx;
	}
	vma->vm_private_data = ctx;
	vma->vm_ops = &rwmem_remap_vm_ops;

	prot = vm_get_page_prot(flags);
	ctx->pages = kcalloc(DIV_ROUND_UP(karg.size, PAGE_SIZE),
			     sizeof(*ctx->pages), GFP_KERNEL);
	if (!ctx->pages) {
		mmap_write_unlock(mm);
		ret = -ENOMEM;
		goto out_ctx;
	}
	for (off = 0; off < karg.size; off += PAGE_SIZE) {
		pte_t *pte;
		struct page *page;
		size_t phy;

		phy = get_proc_phy_addr(pid, karg.src_vaddr + off, &pte);
		if (!phy)
			continue;
		if (!pfn_valid(phy >> PAGE_SHIFT))
			continue;
		page = pfn_to_page(phy >> PAGE_SHIFT);
		if (PageReserved(page))
			continue;
		get_page(page);
		ctx->pages[ctx->npages++] = page;
		ret = g_remap_pfn_range(vma, karg.dst_vaddr + off,
					phy >> PAGE_SHIFT, PAGE_SIZE, prot);
		if (ret)
			break;
	}
	mmap_write_unlock(mm);

	return 0;
out_ctx:
	put_pid(ctx->src_pid);
	kfree(ctx);
out_pid:
	put_pid(pid);
	return ret;
}

int rwmem_get_base(const struct rwmem_base_arg __user *arg)
{
	struct rwmem_base_arg karg;
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	struct pid *pid;
	struct task_struct *task;
	int ret = -ENOENT;
#ifdef _LINUX_MAPLE_TREE_H
	struct vma_iterator vmi;
#endif

	if (!arg)
		return -EINVAL;
	if (copy_from_user(&karg, arg, sizeof(karg)))
		return -EFAULT;
	karg.name[sizeof(karg.name) - 1] = 0;

	pid = rwmem_handle_get(karg.handle);
	if (!pid)
		return -EBADF;
	task = pid_task(pid, PIDTYPE_PID);
	if (!task) {
		ret = -ESRCH;
		goto out_pid;
	}
	mm = get_task_mm(task);
	if (!mm) {
		ret = -ESRCH;
		goto out_pid;
	}

	mmap_read_lock(mm);
#ifdef _LINUX_MAPLE_TREE_H
	vma_iter_init(&vmi, mm, 0);
	vma = mas_find(&vmi.mas, ULONG_MAX);
#else
	vma = mm->mmap;
#endif
	while (vma) {
		if (vma->vm_file) {
			struct dentry *dentry = vma->vm_file->f_path.dentry;

			if (dentry &&
			    !strcmp(dentry->d_name.name, karg.name)) {
				karg.out = vma->vm_start;
				ret = 0;
				break;
			}
		}
#ifdef _LINUX_MAPLE_TREE_H
		vma = mas_find(&vmi.mas, ULONG_MAX);
#else
		vma = vma->vm_next;
#endif
	}
	mmap_read_unlock(mm);
	mmput(mm);
out_pid:
	put_pid(pid);
	if (!ret && copy_to_user(&((struct rwmem_base_arg __user *)arg)->out,
				 &karg.out, sizeof(karg.out)))
		return -EFAULT;
	return ret;
}
