// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input-event-codes.h>
#include <linux/kprobes.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "rwmem_proto.h"
#include "touch.h"

extern unsigned long kr_name_to_addr(const char *name);

#define RWMEM_TOUCH_MAX_EVENTS 64

struct rwmem_touch_event {
	unsigned int type;
	unsigned int code;
	int value;
};

struct rwmem_event_pool {
	struct rwmem_touch_event events[RWMEM_TOUCH_MAX_EVENTS];
	unsigned int size;
	spinlock_t event_lock;
};

typedef void (*hm_input_handle_event)(struct input_dev *dev,
				      unsigned int type, unsigned int code,
				      int value);
typedef int (*hm_register_kprobe)(struct kprobe *p);
typedef void (*hm_unregister_kprobe)(struct kprobe *p);

static hm_input_handle_event g_input_handle_event;
static hm_register_kprobe g_register_kprobe;
static hm_unregister_kprobe g_unregister_kprobe;
static struct rwmem_event_pool *g_pool;
static struct input_dev *g_touch_dev;

static void input_event_cache(unsigned int type, unsigned int code, int value)
{
	struct rwmem_touch_event *event;
	unsigned long flags;

	if (!g_pool)
		return;
	spin_lock_irqsave(&g_pool->event_lock, flags);
	if (g_pool->size >= RWMEM_TOUCH_MAX_EVENTS) {
		spin_unlock_irqrestore(&g_pool->event_lock, flags);
		return;
	}
	event = &g_pool->events[g_pool->size++];
	event->type = type;
	event->code = code;
	event->value = value;
	spin_unlock_irqrestore(&g_pool->event_lock, flags);
}

static void handle_cache_events(struct input_dev *dev)
{
	struct rwmem_touch_event event;
	unsigned long flags, flags2;
	unsigned int i;

	if (!g_pool || !dev)
		return;
	spin_lock_irqsave(&g_pool->event_lock, flags);
	if (g_pool->size == 0) {
		spin_unlock_irqrestore(&g_pool->event_lock, flags);
		return;
	}
	spin_lock_irqsave(&dev->event_lock, flags2);
	for (i = 0; i < g_pool->size; i++) {
		event = g_pool->events[i];
		g_input_handle_event(dev, event.type, event.code, event.value);
	}
	spin_unlock_irqrestore(&dev->event_lock, flags2);
	g_pool->size = 0;
	spin_unlock_irqrestore(&g_pool->event_lock, flags);
}

static int input_event_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct input_dev *dev;

	if (regs->regs[1] != EV_SYN)
		return 0;
	dev = (struct input_dev *)regs->regs[0];
	if (dev)
		handle_cache_events(dev);
	return 0;
}

static struct kprobe g_input_event_kp = {
	.symbol_name = "input_event",
	.pre_handler = input_event_pre,
};static int input_inject_event_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct input_handle *handle;

	if (regs->regs[1] != EV_SYN)
		return 0;
	handle = (struct input_handle *)regs->regs[0];
	if (handle && handle->dev)
		handle_cache_events(handle->dev);
	return 0;
}

static struct kprobe g_input_inject_event_kp = {
	.symbol_name = "input_inject_event",
	.pre_handler = input_inject_event_pre,
};

static struct input_dev *find_touch_device(void)
{
	struct list_head *input_dev_list;
	struct mutex *input_mutex;
	struct input_dev *dev;

	if (g_touch_dev)
		return g_touch_dev;
	input_dev_list = (struct list_head *)kr_name_to_addr("input_dev_list");
	input_mutex = (struct mutex *)kr_name_to_addr("input_mutex");
	if (!input_dev_list || !input_mutex)
		return NULL;

	mutex_lock(input_mutex);
	list_for_each_entry(dev, input_dev_list, node) {
		if (test_bit(EV_ABS, dev->evbit) &&
		    (test_bit(ABS_MT_POSITION_X, dev->absbit) ||
		     test_bit(ABS_X, dev->absbit))) {
			g_touch_dev = dev;
			mutex_unlock(input_mutex);
			return dev;
		}
	}
	mutex_unlock(input_mutex);
	return NULL;
}

static int input_mt_slot_state(struct input_dev *dev, int slot, int new_id)
{
	struct input_mt *mt = dev->mt;
	struct input_mt_slot *sl;
	int id;

	if (!mt || slot < 0 || slot >= mt->num_slots)
		return -EINVAL;

	if (new_id) {
		sl = &mt->slots[slot];
		id = input_mt_get_value(sl, ABS_MT_TRACKING_ID);
		if (id < 0)
			id = input_mt_new_trkid(mt);
		input_event_cache(EV_ABS, ABS_MT_TRACKING_ID, id);
	} else {
		input_event_cache(EV_ABS, ABS_MT_TRACKING_ID, -1);
	}
	return 0;
}

int __nocfi rwmem_touch(const struct rwmem_touch_arg __user *arg)
{
	struct rwmem_touch_arg karg;
	struct input_dev *dev;
	int ret;

	if (!arg)
		return -EINVAL;
	if (copy_from_user(&karg, arg, sizeof(karg)))
		return -EFAULT;
	if (karg.cmd < RWMEM_TOUCH_DOWN || karg.cmd > RWMEM_TOUCH_UP)
		return -EINVAL;
	if (!g_input_handle_event || !g_pool)
		return -EINVAL;

	dev = find_touch_device();
	if (!dev)
		return -ENODEV;

	input_event_cache(EV_ABS, ABS_MT_SLOT, karg.slot);
	if (karg.cmd == RWMEM_TOUCH_UP) {
		input_mt_slot_state(dev, karg.slot, 0);
		input_event_cache(EV_KEY, BTN_TOUCH, 0);
	} else {
		ret = input_mt_slot_state(dev, karg.slot, 1);
		if (ret)
			return ret;
		input_event_cache(EV_ABS, ABS_MT_POSITION_X, karg.x);
		input_event_cache(EV_ABS, ABS_MT_POSITION_Y, karg.y);
		input_event_cache(EV_KEY, BTN_TOUCH, 1);
	}
	input_event_cache(EV_SYN, SYN_REPORT, 0);
	handle_cache_events(dev);

	return 0;
}

int __nocfi rwmem_touch_init(void)
{
	g_input_handle_event = (hm_input_handle_event)
		kr_name_to_addr("input_handle_event");
	if (!g_input_handle_event)
		return -EOPNOTSUPP;
	g_register_kprobe = (hm_register_kprobe)
		kr_name_to_addr("register_kprobe");
	g_unregister_kprobe = (hm_unregister_kprobe)
		kr_name_to_addr("unregister_kprobe");

	g_pool = kzalloc(sizeof(*g_pool), GFP_KERNEL);
	if (!g_pool)
		return -ENOMEM;
	spin_lock_init(&g_pool->event_lock);

	if (g_register_kprobe) {
		if (g_register_kprobe(&g_input_event_kp))
			pr_warn("[rwmem] input_event kprobe failed\n");
		if (g_register_kprobe(&g_input_inject_event_kp))
			pr_warn("[rwmem] input_inject_event kprobe failed\n");
	}
	return 0;
}

void __nocfi rwmem_touch_exit(void)
{
	if (g_unregister_kprobe) {
		g_unregister_kprobe(&g_input_event_kp);
		g_unregister_kprobe(&g_input_inject_event_kp);
	}
	kfree(g_pool);
	g_pool = NULL;
	g_touch_dev = NULL;
}
