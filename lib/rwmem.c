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

#ifndef _LINUX_MAPLE_TREE_H
struct ma_state {
	void *tree;
	unsigned long index;
	unsigned long last;
	void *node;
	unsigned long min;
	unsigned long max;
	void *alloc;
	unsigned int status;
	unsigned char depth;
	unsigned char offset;
	unsigned char mas_flags;
	unsigned char end;
	unsigned int store_type;
};
#endif

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
static long g_off_mmap = -1;
static long g_off_mm_mt = -1;
static long g_off_map_count = -1;
static long g_off_vm_start = -1;
static long g_off_vm_end = -1;
static long g_off_vm_flags = -1;
static long g_off_vm_next = -1;
static long g_off_vm_file = -1;
static long g_off_file_path = -1;
static long g_off_dentry_name = -1;
static long g_off_qstr_name = -1;

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

static struct pid *handle_get(int id)
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

static size_t get_proc_phy_addr(struct pid *pid, size_t vaddr, pte_t **out_pte)
{
	struct task_struct *task;
	struct mm_struct *mm;
	pgd_t *pgd;
	pgd_t *pgde;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	size_t paddr = 0;

	*out_pte = NULL;
	task = pid_task(pid, PIDTYPE_PID);
	if (!task)
		return 0;
	mm = get_task_mm(task);
	if (!mm)
		return 0;

	if (get_off(&g_off_pgd, "mm_struct", "pgd"))
		goto out;
	pgd = *(pgd_t **)((char *)mm + g_off_pgd);
	if (!pgd)
		goto out;
	pgde = pgd + pgd_index(vaddr);
	if (pgd_none(*pgde))
		goto out;
	p4d = p4d_offset(pgde, vaddr);
	if (p4d_none(*p4d))
		goto out;
	pud = pud_offset(p4d, vaddr);
	if (pud_none(*pud))
		goto out;
	pmd = pmd_offset(pud, vaddr);
	if (pmd_none(*pmd))
		goto out;
	if (pmd_leaf(*pmd))
		goto out;
	pte = pte_offset_kernel(pmd, vaddr);
	if (pte_none(*pte))
		goto out;

	paddr = page_to_phys(pte_page(*pte)) | (vaddr & ~PAGE_MASK);
	*out_pte = pte;
out:
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

static ssize_t rwmem_rw(int id, size_t vaddr, char __user *buf, size_t size,
			bool write, bool force)
{
	struct pid *pid;
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
	pid = handle_get(id);
	if (!pid)
		return -EBADF;

	bounce = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!bounce) {
		put_pid(pid);
		return -ENOMEM;
	}

	while (done < size) {
		size_t phy;
		size_t page_left;

		phy = get_proc_phy_addr(pid, vaddr + done, &pte);
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
	put_pid(pid);
	return ret;
out_restore:
	if (flipped)
		set_pte(pte, orig);
	goto out;
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
	ssize_t total = 0;
	size_t i;

	if (!vec || !count)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		struct rwmem_iovec iv;
		ssize_t n;

		if (copy_from_user(&iv, &vec[i], sizeof(iv)))
			return total ? total : -EFAULT;
		if (!iv.buf || !iv.size || iv.size > RWMEM_MAX_TRANSFER)
			return total ? total : -EINVAL;
		if (mode == RWMEM_VEC_WRITE)
			n = rwmem_write(id, iv.vaddr, (char __user *)iv.buf,
					iv.size);
		else
			n = rwmem_read(id, iv.vaddr, (char __user *)iv.buf,
				       iv.size);
		if (n < 0)
			return total ? total : n;
		total += n;
	}
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
	unsigned long file_addr;
	unsigned long dentry_addr;
	unsigned long name_addr;

	if (get_off(&g_off_vm_file, "vm_area_struct", "vm_file") ||
	    get_off(&g_off_file_path, "file", "f_path") ||
	    get_off(&g_off_dentry_name, "dentry", "d_name") ||
	    get_off(&g_off_qstr_name, "qstr", "name"))
		return -EOPNOTSUPP;

	file_addr = *(unsigned long *)((char *)vma + g_off_vm_file);
	if (!file_addr)
		return -ENOENT;
	dentry_addr = *(unsigned long *)((char *)file_addr + g_off_file_path +
					 sizeof(unsigned long));
	if (!dentry_addr)
		return -ENOENT;
	name_addr = *(unsigned long *)((char *)dentry_addr + g_off_dentry_name +
				       g_off_qstr_name);
	if (!name_addr)
		return -ENOENT;
	if (rw_safe_read(out, (void *)name_addr, outsz - 1))
		return -EFAULT;
	out[outsz - 1] = 0;
	return 0;
}

enum rwmem_maps_mode {
	RWMEM_MAPS_NONE = 0,
	RWMEM_MAPS_LIST,
	RWMEM_MAPS_MTREE,
};

static enum rwmem_maps_mode g_maps_mode;

#ifdef _LINUX_MAPLE_TREE_H
#define RWMEM_MAS_FIND(mas, max) mas_find((mas), (max))
#else
static void *(*g_mas_find)(struct ma_state *mas, unsigned long max);

static __nocfi void *call_mas_find(struct ma_state *mas, unsigned long max)
{
	return g_mas_find(mas, max);
}

#define RWMEM_MAS_FIND(mas, max) call_mas_find((mas), (max))
#endif

static void rwmem_mas_init(struct ma_state *mas, void *tree,
			   unsigned long index)
{
	memset(mas, 0, sizeof(*mas));
	mas->tree = tree;
	mas->index = index;
	mas->last = index;
	mas->max = ULONG_MAX;
#ifdef MAS_NONE
	mas->node = MAS_NONE;
#else
	mas->status = 1; /* ma_start */
#endif
}

static int ma_state_layout_ok(u32 id)
{
	u32 size;
	u32 bit_off;
	u32 bit_sz;

	size = ti_type_size(ti_base(), id);
	if (size != sizeof(struct ma_state))
		return -1;
	if (ti_member_off(ti_base(), id, "tree", &bit_off, &bit_sz) ||
	    (bit_off / 8) != offsetof(struct ma_state, tree))
		return -1;
	if (ti_member_off(ti_base(), id, "index", &bit_off, &bit_sz) ||
	    (bit_off / 8) != offsetof(struct ma_state, index))
		return -1;
	if (ti_member_off(ti_base(), id, "last", &bit_off, &bit_sz) ||
	    (bit_off / 8) != offsetof(struct ma_state, last))
		return -1;
	return 0;
}

static int maps_mode_probe(void)
{
	u32 id;
	long off_mmap;
	long off_next;
	long off_mt;

	if (g_maps_mode)
		return 0;

	if (!off_from_btf("mm_struct", "mm_mt", &off_mt) &&
	    !ti_type_by_name(ti_base(), "ma_state", RWMEM_STRUCT, &id) &&
	    !ma_state_layout_ok(id)) {
		g_off_mm_mt = off_mt;
		g_maps_mode = RWMEM_MAPS_MTREE;
		pr_info("[rwmem] maps: maple tree\n");
		return 0;
	}
	if (!off_from_btf("mm_struct", "mmap", &off_mmap) &&
	    !off_from_btf("vm_area_struct", "vm_next", &off_next)) {
		g_off_mmap = off_mmap;
		g_off_vm_next = off_next;
		g_maps_mode = RWMEM_MAPS_LIST;
		pr_info("[rwmem] maps: vm_next list mmap=%ld vm_next=%ld\n",
			g_off_mmap, g_off_vm_next);
		return 0;
	}
	g_maps_mode = RWMEM_MAPS_NONE;
	return -EOPNOTSUPP;
}

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
	if (maps_mode_probe() ||
	    get_off(&g_off_vm_start, "vm_area_struct", "vm_start") ||
	    get_off(&g_off_vm_end, "vm_area_struct", "vm_end") ||
	    get_off(&g_off_vm_flags, "vm_area_struct", "vm_flags"))
		return -EOPNOTSUPP;

	pid = handle_get(id);
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

	if (g_maps_mode == RWMEM_MAPS_MTREE) {
		struct ma_state mas;
		struct vm_area_struct *vma;

		rwmem_mas_init(&mas, (char *)mm + g_off_mm_mt, start);
		rcu_read_lock();
		vma = RWMEM_MAS_FIND(&mas, ULONG_MAX);
		while (vma && cnt < max) {
			unsigned long vstart =
				*(unsigned long *)((char *)vma +
						   g_off_vm_start);
			unsigned long vend =
				*(unsigned long *)((char *)vma + g_off_vm_end);
			unsigned long vflags =
				*(unsigned long *)((char *)vma +
						   g_off_vm_flags);

			klist[cnt].start = vstart;
			klist[cnt].end = vend;
			klist[cnt].flags = vflags;
			memset(klist[cnt].path, 0, sizeof(klist[cnt].path));
			map_path(vma, klist[cnt].path, sizeof(klist[cnt].path));
			cnt++;
			vma = RWMEM_MAS_FIND(&mas, ULONG_MAX);
		}
		rcu_read_unlock();
	} else {
		struct vm_area_struct *vma;

		vma = *(struct vm_area_struct **)((char *)mm + g_off_mmap);
		while (vma && cnt < max) {
			unsigned long vstart =
				*(unsigned long *)((char *)vma +
						   g_off_vm_start);
			unsigned long vend =
				*(unsigned long *)((char *)vma + g_off_vm_end);
			unsigned long vflags =
				*(unsigned long *)((char *)vma +
						   g_off_vm_flags);

			if (vend > start) {
				klist[cnt].start = vstart;
				klist[cnt].end = vend;
				klist[cnt].flags = vflags;
				memset(klist[cnt].path, 0,
				       sizeof(klist[cnt].path));
				map_path(vma, klist[cnt].path,
					 sizeof(klist[cnt].path));
				cnt++;
			}
			vma = *(struct vm_area_struct **)((char *)vma +
							  g_off_vm_next);
		}
	}

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

	pid = handle_get(id);
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
