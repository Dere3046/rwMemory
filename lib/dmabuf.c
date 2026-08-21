// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/kernel.h>
#include <linux/dma-buf.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

MODULE_IMPORT_NS(DMA_BUF);

#include "rwmem_proto.h"
#include "rwmem.h"
#include "dmabuf.h"

extern unsigned long kr_name_to_addr(const char *name);

struct rwmem_dmabuf {
	struct page **pages;
	int npages;
	size_t size;
};

static struct sg_table *rwmem_dmabuf_map_dma_buf(struct dma_buf_attachment *att,
						 enum dma_data_direction dir)
{
	struct rwmem_dmabuf *db = att->dmabuf->priv;
	struct sg_table *sg;
	int i;

	sg = kmalloc(sizeof(*sg), GFP_KERNEL);
	if (!sg)
		return ERR_PTR(-ENOMEM);
	if (sg_alloc_table(sg, db->npages, GFP_KERNEL)) {
		kfree(sg);
		return ERR_PTR(-ENOMEM);
	}
	for (i = 0; i < db->npages; i++)
		sg_set_page(&sg->sgl[i], db->pages[i], PAGE_SIZE, 0);
	return sg;
}

static void rwmem_dmabuf_unmap_dma_buf(struct dma_buf_attachment *att,
				       struct sg_table *sg,
				       enum dma_data_direction dir)
{
	sg_free_table(sg);
	kfree(sg);
}

static vm_fault_t rwmem_dmabuf_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct dma_buf *buf = vma->vm_file->private_data;
	struct rwmem_dmabuf *db = buf->priv;
	unsigned long idx = (vmf->address - vma->vm_start) >> PAGE_SHIFT;
	struct page *page;

	if (idx >= db->npages)
		return VM_FAULT_SIGBUS;
	page = db->pages[idx];
	get_page(page);
	vmf->page = page;
	return 0;
}

static const struct vm_operations_struct rwmem_dmabuf_vm_ops = {
	.fault = rwmem_dmabuf_fault,
};

static int rwmem_dmabuf_mmap(struct dma_buf *buf, struct vm_area_struct *vma)
{
	if (vma->vm_end - vma->vm_start > buf->size)
		return -EINVAL;
	vma->vm_ops = &rwmem_dmabuf_vm_ops;
	return 0;
}

static void rwmem_dmabuf_release(struct dma_buf *buf)
{
	struct rwmem_dmabuf *db = buf->priv;
	int i;

	for (i = 0; i < db->npages; i++)
		put_page(db->pages[i]);
	kfree(db->pages);
	kfree(db);
}

static const struct dma_buf_ops rwmem_dmabuf_ops = {
	.map_dma_buf = rwmem_dmabuf_map_dma_buf,
	.unmap_dma_buf = rwmem_dmabuf_unmap_dma_buf,
	.mmap = rwmem_dmabuf_mmap,
	.release = rwmem_dmabuf_release,
};

static int __nocfi rwmem_dmabuf_export_do(struct pid *pid, unsigned long vaddr,
					  size_t size, int *out_fd)
{
	struct rwmem_dmabuf *db;
	struct dma_buf *buf;
	struct task_struct *task;
	struct mm_struct *mm;
	struct dma_buf_export_info exp;
	unsigned long off;
	int npages;
	int fd;

	if (!size || size > RWMEM_MAX_TRANSFER)
		return -EINVAL;

	task = pid_task(pid, PIDTYPE_PID);
	if (!task)
		return -ESRCH;
	mm = get_task_mm(task);
	if (!mm)
		return -ESRCH;

	npages = DIV_ROUND_UP(size, PAGE_SIZE);
	db = kzalloc(sizeof(*db), GFP_KERNEL);
	if (!db) {
		mmput(mm);
		return -ENOMEM;
	}
	db->pages = kcalloc(npages, sizeof(*db->pages), GFP_KERNEL);
	if (!db->pages) {
		kfree(db);
		mmput(mm);
		return -ENOMEM;
	}
	db->npages = 0;
	db->size = size;

	mmap_read_lock(mm);
	for (off = 0; off < size; off += PAGE_SIZE) {
		pte_t *pte;
		size_t phy;
		struct page *page;

		phy = rwmem_phy_addr(mm, vaddr + off, &pte);
		if (!phy)
			continue;
		if (!pfn_valid(phy >> PAGE_SHIFT))
			continue;
		page = pfn_to_page(phy >> PAGE_SHIFT);
		if (PageReserved(page))
			continue;
		get_page(page);
		db->pages[db->npages++] = page;
	}
	mmap_read_unlock(mm);
	mmput(mm);

	if (!db->npages) {
		kfree(db->pages);
		kfree(db);
		return -EFAULT;
	}

	memset(&exp, 0, sizeof(exp));
	exp.exp_name = "rwmem";
	exp.ops = &rwmem_dmabuf_ops;
	exp.size = size;
	exp.flags = O_RDWR | O_CLOEXEC;
	exp.priv = db;

	buf = dma_buf_export(&exp);
	if (IS_ERR(buf)) {
		fd = PTR_ERR(buf);
		rwmem_dmabuf_release(buf);
		return fd;
	}
	fd = dma_buf_fd(buf, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		dma_buf_put(buf);
		return fd;
	}
	*out_fd = fd;
	return 0;
}


int rwmem_dmabuf_export(const struct rwmem_dmabuf_arg __user *arg)
{
	struct rwmem_dmabuf_arg karg;
	struct pid *pid;
	int fd;
	int ret;

	if (!arg)
		return -EINVAL;
	if (copy_from_user(&karg, arg, sizeof(karg)))
		return -EFAULT;

	pid = rwmem_handle_get(karg.handle);
	if (!pid)
		return -EBADF;
	ret = rwmem_dmabuf_export_do(pid, karg.vaddr, karg.size, &fd);
	put_pid(pid);
	if (ret)
		return ret;

	karg.out_fd = fd;
	if (copy_to_user(&((struct rwmem_dmabuf_arg __user *)arg)->out_fd,
			 &karg.out_fd, sizeof(karg.out_fd)))
		return -EFAULT;
	return 0;
}
