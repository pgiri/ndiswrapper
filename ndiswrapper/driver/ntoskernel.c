/*
 *  Copyright (C) 2003-2005 Pontus Fuchs, Giridhar Pemmasani
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 */

#include "ntoskernel.h"
#include "ndis.h"
#include "usb.h"
#include "pnp.h"
#include "loader.h"
#include "ntoskernel_exports.h"
#include "nvmalloc.h"

/* MDLs describe a range of virtual address with an array of physical
 * pages right after the header. For different ranges of virtual
 * addresses, the number of entries of physical pages may be different
 * (depending on number of entries required). If we want to allocate
 * MDLs from a pool, the size has to be constant. So we assume that
 * maximum range used by a driver is MDL_CACHE_PAGES; if a driver
 * requests an MDL for a bigger region, we allocate it with kmalloc;
 * otherwise, we allocate from the pool */

#define MDL_CACHE_PAGES 3
#define MDL_CACHE_SIZE (sizeof(struct mdl) + \
			(sizeof(PFN_NUMBER) * MDL_CACHE_PAGES))
struct wrap_mdl {
	struct nt_list list;
	struct mdl mdl[0];
};

/* everything here is for all drivers/devices - not per driver/device */
static spinlock_t dispatcher_lock;
spinlock_t ntoskernel_lock;
static void *mdl_cache;
static struct nt_list wrap_mdl_list;

static struct work_struct kdpc_work;
static void kdpc_worker(struct work_struct *dummy);

static struct nt_list kdpc_list;
static spinlock_t kdpc_list_lock;

static struct nt_list callback_objects;

struct nt_list object_list;

struct bus_driver {
	struct nt_list list;
	char name[MAX_DRIVER_NAME_LEN];
	struct driver_object drv_obj;
};

static struct nt_list bus_driver_list;

static struct work_struct ntos_work;
static struct nt_list ntos_work_list;
static spinlock_t ntos_work_lock;
static void ntos_work_worker(struct work_struct *dummy);
spinlock_t irp_cancel_lock;
static NT_SPIN_LOCK nt_list_lock;
static struct nt_slist wrap_timer_slist;
CCHAR cpu_count;

/* compute ticks (100ns) since 1601 until when system booted into
 * wrap_ticks_to_boot */
u64 wrap_ticks_to_boot;

#if defined(CONFIG_X86_64)
static struct timer_list shared_data_timer;
struct kuser_shared_data kuser_shared_data;
#endif

WIN_SYMBOL_MAP("KeTickCount", &jiffies)
WIN_SYMBOL_MAP("KeNumberProcessors", &cpu_count)
WIN_SYMBOL_MAP("NlsMbCodePageTag", FALSE)

struct workqueue_struct *ntos_wq;

#ifdef WRAP_PREEMPT
DEFINE_PER_CPU(struct irql_info, irql_info);
#endif

#if defined(CONFIG_X86_64)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
static void update_user_shared_data_proc(struct timer_list *tl)
#else
static void update_user_shared_data_proc(unsigned long data)
#endif
{
	/* timer is supposed to be scheduled every 10ms, but bigger
	 * intervals seem to work (tried up to 50ms) */
	*((ULONG64 *)&kuser_shared_data.system_time) = ticks_1601();
	*((ULONG64 *)&kuser_shared_data.interrupt_time) =
		jiffies * TICKSPERSEC / HZ;
	*((ULONG64 *)&kuser_shared_data.tick) = jiffies;

	mod_timer(&shared_data_timer, jiffies + MSEC_TO_HZ(30));
}
#endif

void *allocate_object(ULONG size, enum common_object_type type,
		      struct unicode_string *name)
{
	struct common_object_header *hdr;
	void *body;

	/* we pad header as prefix to body */
	hdr = ExAllocatePoolWithTag(NonPagedPool, OBJECT_SIZE(size), 0);
	if (!hdr) {
		WARNING("couldn't allocate memory");
		return NULL;
	}
	memset(hdr, 0, OBJECT_SIZE(size));
	if (name) {
		hdr->name.buf = ExAllocatePoolWithTag(NonPagedPool,
						      name->max_length, 0);
		if (!hdr->name.buf) {
			ExFreePool(hdr);
			return NULL;
		}
		memcpy(hdr->name.buf, name->buf, name->max_length);
		hdr->name.length = name->length;
		hdr->name.max_length = name->max_length;
	}
	hdr->type = type;
	hdr->ref_count = 1;
	spin_lock_bh(&ntoskernel_lock);
	/* threads are looked up often (in KeWaitForXXX), so optimize
	 * for fast lookups of threads */
	if (type == OBJECT_TYPE_NT_THREAD)
		InsertHeadList(&object_list, &hdr->list);
	else
		InsertTailList(&object_list, &hdr->list);
	spin_unlock_bh(&ntoskernel_lock);
	body = HEADER_TO_OBJECT(hdr);
	TRACE3("allocated hdr: %p, body: %p", hdr, body);
	return body;
}

static void free_object(void *object)
{
	struct common_object_header *hdr;

	hdr = OBJECT_TO_HEADER(object);
	spin_lock_bh(&ntoskernel_lock);
	RemoveEntryList(&hdr->list);
	spin_unlock_bh(&ntoskernel_lock);
	TRACE3("freed hdr: %p, body: %p", hdr, object);
	if (hdr->name.buf)
		ExFreePool(hdr->name.buf);
	ExFreePool(hdr);
}

static int add_bus_driver(const char *name)
{
	struct bus_driver *bus_driver;

	bus_driver = kzalloc(sizeof(*bus_driver), GFP_KERNEL);
	if (!bus_driver) {
		ERROR("couldn't allocate memory");
		return -ENOMEM;
	}
	strncpy(bus_driver->name, name, sizeof(bus_driver->name));
	bus_driver->name[sizeof(bus_driver->name)-1] = 0;
	spin_lock_bh(&ntoskernel_lock);
	InsertTailList(&bus_driver_list, &bus_driver->list);
	spin_unlock_bh(&ntoskernel_lock);
	TRACE1("bus driver %s is at %p", name, &bus_driver->drv_obj);
	return STATUS_SUCCESS;
}

struct driver_object *find_bus_driver(const char *name)
{
	struct bus_driver *bus_driver;
	struct driver_object *drv_obj;

	spin_lock_bh(&ntoskernel_lock);
	drv_obj = NULL;
	nt_list_for_each_entry(bus_driver, &bus_driver_list, list) {
		if (strcmp(bus_driver->name, name) == 0) {
			drv_obj = &bus_driver->drv_obj;
			break;
		}
	}
	spin_unlock_bh(&ntoskernel_lock);
	return drv_obj;
}

wfastcall struct nt_list *WIN_FUNC(ExfInterlockedInsertHeadList,3)
	(struct nt_list *head, struct nt_list *entry, NT_SPIN_LOCK *lock)
{
	struct nt_list *first;
	unsigned long flags;

	ENTER5("head = %p, entry = %p", head, entry);
	nt_spin_lock_irqsave(lock, flags);
	first = InsertHeadList(head, entry);
	nt_spin_unlock_irqrestore(lock, flags);
	TRACE5("head = %p, old = %p", head, first);
	return first;
}

wfastcall struct nt_list *WIN_FUNC(ExInterlockedInsertHeadList,3)
	(struct nt_list *head, struct nt_list *entry, NT_SPIN_LOCK *lock)
{
	ENTER5("%p", head);
	return ExfInterlockedInsertHeadList(head, entry, lock);
}

wfastcall struct nt_list *WIN_FUNC(ExfInterlockedInsertTailList,3)
	(struct nt_list *head, struct nt_list *entry, NT_SPIN_LOCK *lock)
{
	struct nt_list *last;
	unsigned long flags;

	ENTER5("head = %p, entry = %p", head, entry);
	nt_spin_lock_irqsave(lock, flags);
	last = InsertTailList(head, entry);
	nt_spin_unlock_irqrestore(lock, flags);
	TRACE5("head = %p, old = %p", head, last);
	return last;
}

wfastcall struct nt_list *WIN_FUNC(ExInterlockedInsertTailList,3)
	(struct nt_list *head, struct nt_list *entry, NT_SPIN_LOCK *lock)
{
	ENTER5("%p", head);
	return ExfInterlockedInsertTailList(head, entry, lock);
}

wfastcall struct nt_list *WIN_FUNC(ExfInterlockedRemoveHeadList,2)
	(struct nt_list *head, NT_SPIN_LOCK *lock)
{
	struct nt_list *ret;
	unsigned long flags;

	ENTER5("head = %p", head);
	nt_spin_lock_irqsave(lock, flags);
	ret = RemoveHeadList(head);
	nt_spin_unlock_irqrestore(lock, flags);
	TRACE5("head = %p, ret = %p", head, ret);
	return ret;
}

wfastcall struct nt_list *WIN_FUNC(ExInterlockedRemoveHeadList,2)
	(struct nt_list *head, NT_SPIN_LOCK *lock)
{
	ENTER5("%p", head);
	return ExfInterlockedRemoveHeadList(head, lock);
}

wfastcall struct nt_list *WIN_FUNC(ExfInterlockedRemoveTailList,2)
	(struct nt_list *head, NT_SPIN_LOCK *lock)
{
	struct nt_list *ret;
	unsigned long flags;

	ENTER5("head = %p", head);
	nt_spin_lock_irqsave(lock, flags);
	ret = RemoveTailList(head);
	nt_spin_unlock_irqrestore(lock, flags);
	TRACE5("head = %p, ret = %p", head, ret);
	return ret;
}

wfastcall struct nt_list *WIN_FUNC(ExInterlockedRemoveTailList,2)
	(struct nt_list *head, NT_SPIN_LOCK *lock)
{
	ENTER5("%p", head);
	return ExfInterlockedRemoveTailList(head, lock);
}

wfastcall void WIN_FUNC(InitializeSListHead,1)
	(nt_slist_header *head)
{
	memset(head, 0, sizeof(*head));
}

wfastcall struct nt_slist *WIN_FUNC(ExInterlockedPushEntrySList,3)
	(nt_slist_header *head, struct nt_slist *entry, NT_SPIN_LOCK *lock)
{
	struct nt_slist *ret;

	ret = PushEntrySList(head, entry, lock);
	return ret;
}

wstdcall struct nt_slist *WIN_FUNC(ExpInterlockedPushEntrySList,2)
	(nt_slist_header *head, struct nt_slist *entry)
{
	struct nt_slist *ret;

	ret = PushEntrySList(head, entry, &nt_list_lock);
	return ret;
}

wfastcall struct nt_slist *WIN_FUNC(InterlockedPushEntrySList,2)
	(nt_slist_header *head, struct nt_slist *entry)
{
	struct nt_slist *ret;

	ret = PushEntrySList(head, entry, &nt_list_lock);
	return ret;
}

wfastcall struct nt_slist *WIN_FUNC(ExInterlockedPopEntrySList,2)
	(nt_slist_header *head, NT_SPIN_LOCK *lock)
{
	struct nt_slist *ret;

	ret = PopEntrySList(head, lock);
	return ret;
}

wstdcall struct nt_slist *WIN_FUNC(ExpInterlockedPopEntrySList,1)
	(nt_slist_header *head)
{
	struct nt_slist *ret;

	ret = PopEntrySList(head, &nt_list_lock);
	return ret;
}

wfastcall struct nt_slist *WIN_FUNC(InterlockedPopEntrySList,1)
	(nt_slist_header *head)
{
	struct nt_slist *ret;

	ret = PopEntrySList(head, &nt_list_lock);
	return ret;
}

wstdcall USHORT WIN_FUNC(ExQueryDepthSList,1)
	(nt_slist_header *head)
{
	USHORT depth;
	ENTER5("%p", head);
	depth = head->depth;
	TRACE5("%d, %p", depth, head->next);
	return depth;
}

wfastcall LONG WIN_FUNC(InterlockedIncrement,1)
	(LONG volatile *val)
{
	return post_atomic_add(*val, 1);
}

wfastcall LONG WIN_FUNC(InterlockedDecrement,1)
	(LONG volatile *val)
{
	return post_atomic_add(*val, -1);
}

wfastcall LONG WIN_FUNC(InterlockedExchange,2)
	(LONG volatile *target, LONG val)
{
	return xchg(target, val);
}

wfastcall LONG WIN_FUNC(InterlockedCompareExchange,3)
	(LONG volatile *dest, LONG new, LONG old)
{
	return cmpxchg(dest, old, new);
}

wfastcall void WIN_FUNC(ExInterlockedAddLargeStatistic,2)
	(LARGE_INTEGER volatile *plint, ULONG n)
{
	unsigned long flags;

	local_irq_save(flags);
#ifdef CONFIG_X86_64
	__asm__ __volatile__(
		"\n"
		LOCK_PREFIX "add %1, %0\n\t"
		: "+m" (*plint)
		: "r" (n));
#else
	__asm__ __volatile__(
		"1:\t"
		"   movl %1, %%ebx\n\t"
		"   movl %%edx, %%ecx\n\t"
		"   addl %%eax, %%ebx\n\t"
		"   adcl $0, %%ecx\n\t"
		    LOCK_PREFIX "cmpxchg8b %0\n\t"
		"   jnz 1b\n\t"
		: "+m" (*plint)
		: "m" (n), "A" (*plint)
		: "ebx", "ecx");
#endif
	local_irq_restore(flags);
}

static void initialize_object(struct dispatcher_header *dh, enum dh_type type,
			      int state)
{
	memset(dh, 0, sizeof(*dh));
	set_object_type(dh, type);
	dh->signal_state = state;
	InitializeListHead(&dh->wait_blocks);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
static void timer_proc(struct timer_list *tl)
#else
static void timer_proc(unsigned long data)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
	struct wrap_timer *wrap_timer = from_timer(wrap_timer, tl, timer);
#else
	struct wrap_timer *wrap_timer = (struct wrap_timer *)data;
#endif
	struct nt_timer *nt_timer;
	struct kdpc *kdpc;

	nt_timer = wrap_timer->nt_timer;
	TIMERENTER("%p(%p), %lu", wrap_timer, nt_timer, jiffies);
#ifdef TIMER_DEBUG
	BUG_ON(wrap_timer->wrap_timer_magic != WRAP_TIMER_MAGIC);
	BUG_ON(nt_timer->wrap_timer_magic != WRAP_TIMER_MAGIC);
#endif
	KeSetEvent((struct nt_event *)nt_timer, 0, FALSE);
	if (wrap_timer->repeat)
		mod_timer(&wrap_timer->timer, jiffies + wrap_timer->repeat);
	kdpc = nt_timer->kdpc;
	if (kdpc)
		queue_kdpc(kdpc);
	TIMEREXIT(return);
}

void wrap_init_timer(struct nt_timer *nt_timer, enum timer_type type,
		     struct ndis_mp_block *nmb)
{
	struct wrap_timer *wrap_timer;

	/* TODO: if a timer is initialized more than once, we allocate
	 * memory for wrap_timer more than once for the same nt_timer,
	 * wasting memory. We can check if nt_timer->wrap_timer_magic is
	 * set and not allocate, but it is not guaranteed always to be
	 * safe */
	TIMERENTER("%p", nt_timer);
	/* we allocate memory for wrap_timer behind driver's back and
	 * there is no NDIS/DDK function where this memory can be
	 * freed, so we use slack_kmalloc so it gets freed when driver
	 * is unloaded */
	if (nmb)
		wrap_timer = kzalloc(sizeof(*wrap_timer), irql_gfp());
	else
		wrap_timer = slack_kzalloc(sizeof(*wrap_timer));
	if (!wrap_timer) {
		ERROR("couldn't allocate memory for timer");
		return;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	init_timer(&wrap_timer->timer);
	wrap_timer->timer.function = timer_proc;
	wrap_timer->timer.data = (unsigned long)wrap_timer;
#else
	timer_setup(&wrap_timer->timer, timer_proc, 0);
#endif
	wrap_timer->nt_timer = nt_timer;
#ifdef TIMER_DEBUG
	wrap_timer->wrap_timer_magic = WRAP_TIMER_MAGIC;
#endif
	nt_timer->wrap_timer = wrap_timer;
	nt_timer->kdpc = NULL;
	initialize_object(&nt_timer->dh, (enum dh_type)type, 0);
	nt_timer->wrap_timer_magic = WRAP_TIMER_MAGIC;
	TIMERTRACE("timer %p (%p)", wrap_timer, nt_timer);
	spin_lock_bh(&ntoskernel_lock);
	if (nmb) {
		wrap_timer->slist.next = nmb->wnd->wrap_timer_slist.next;
		nmb->wnd->wrap_timer_slist.next = &wrap_timer->slist;
	} else {
		wrap_timer->slist.next = wrap_timer_slist.next;
		wrap_timer_slist.next = &wrap_timer->slist;
	}
	spin_unlock_bh(&ntoskernel_lock);
	TIMEREXIT(return);
}

wstdcall void WIN_FUNC(KeInitializeTimerEx,2)
	(struct nt_timer *nt_timer, enum timer_type type)
{
	TIMERENTER("%p", nt_timer);
	wrap_init_timer(nt_timer, type, NULL);
}

wstdcall void WIN_FUNC(KeInitializeTimer,1)
	(struct nt_timer *nt_timer)
{
	TIMERENTER("%p", nt_timer);
	wrap_init_timer(nt_timer, NotificationTimer, NULL);
}

/* expires and repeat are in HZ */
BOOLEAN wrap_set_timer(struct nt_timer *nt_timer, unsigned long expires_hz,
		       unsigned long repeat_hz, struct kdpc *kdpc)
{
	struct wrap_timer *wrap_timer;

	TIMERENTER("%p, %lu, %lu, %p, %lu",
		   nt_timer, expires_hz, repeat_hz, kdpc, jiffies);

	wrap_timer = nt_timer->wrap_timer;
	TIMERTRACE("%p", wrap_timer);
#ifdef TIMER_DEBUG
	if (wrap_timer->nt_timer != nt_timer)
		WARNING("bad timers: %p, %p, %p", wrap_timer, nt_timer,
			wrap_timer->nt_timer);
	if (nt_timer->wrap_timer_magic != WRAP_TIMER_MAGIC) {
		WARNING("buggy Windows timer didn't initialize timer %p",
			nt_timer);
		return FALSE;
	}
	if (wrap_timer->wrap_timer_magic != WRAP_TIMER_MAGIC) {
		WARNING("timer %p is not initialized (%lx)?",
			wrap_timer, wrap_timer->wrap_timer_magic);
		wrap_timer->wrap_timer_magic = WRAP_TIMER_MAGIC;
	}
#endif
	KeClearEvent((struct nt_event *)nt_timer);
	nt_timer->kdpc = kdpc;
	wrap_timer->repeat = repeat_hz;
	if (mod_timer(&wrap_timer->timer, jiffies + expires_hz))
		TIMEREXIT(return TRUE);
	else
		TIMEREXIT(return FALSE);
}

wstdcall BOOLEAN WIN_FUNC(KeSetTimerEx,4)
	(struct nt_timer *nt_timer, LARGE_INTEGER duetime_ticks,
	 LONG period_ms, struct kdpc *kdpc)
{
	unsigned long expires_hz, repeat_hz;

	TIMERENTER("%p, %lld, %d", nt_timer, duetime_ticks, period_ms);
	expires_hz = SYSTEM_TIME_TO_HZ(duetime_ticks);
	repeat_hz = MSEC_TO_HZ(period_ms);
	return wrap_set_timer(nt_timer, expires_hz, repeat_hz, kdpc);
}

wstdcall BOOLEAN WIN_FUNC(KeSetTimer,3)
	(struct nt_timer *nt_timer, LARGE_INTEGER duetime_ticks,
	 struct kdpc *kdpc)
{
	TIMERENTER("%p, %lld, %p", nt_timer, duetime_ticks, kdpc);
	return KeSetTimerEx(nt_timer, duetime_ticks, 0, kdpc);
}

wstdcall BOOLEAN WIN_FUNC(KeCancelTimer,1)
	(struct nt_timer *nt_timer)
{
	struct wrap_timer *wrap_timer;
	int ret;

	TIMERENTER("%p", nt_timer);
	wrap_timer = nt_timer->wrap_timer;
	if (!wrap_timer) {
		ERROR("invalid wrap_timer");
		return TRUE;
	}
#ifdef TIMER_DEBUG
	BUG_ON(wrap_timer->wrap_timer_magic != WRAP_TIMER_MAGIC);
#endif
	/* disable timer before deleting so if it is periodic timer, it
	 * won't be re-armed after deleting */
	wrap_timer->repeat = 0;
	ret = del_timer_sync(&wrap_timer->timer);
	/* the documentation for KeCancelTimer suggests the DPC is
	 * deqeued, but actually DPC is left to run */
	if (ret)
		TIMEREXIT(return TRUE);
	else
		TIMEREXIT(return FALSE);
}

wstdcall BOOLEAN WIN_FUNC(KeReadStateTimer,1)
	(struct nt_timer *nt_timer)
{
	if (nt_timer->dh.signal_state)
		return TRUE;
	else
		return FALSE;
}

wstdcall void WIN_FUNC(KeInitializeDpc,3)
	(struct kdpc *kdpc, void *func, void *ctx)
{
	ENTER3("%p, %p, %p", kdpc, func, ctx);
	memset(kdpc, 0, sizeof(*kdpc));
	kdpc->func = func;
	kdpc->ctx = ctx;
	InitializeListHead(&kdpc->list);
}

static void kdpc_worker(struct work_struct *dummy)
{
	struct nt_list *entry;
	struct kdpc *kdpc;
	unsigned long flags;
	KIRQL irql;

	WORKENTER("");
	irql = raise_irql(DISPATCH_LEVEL);
	while (1) {
		spin_lock_irqsave(&kdpc_list_lock, flags);
		entry = RemoveHeadList(&kdpc_list);
		if (entry) {
			kdpc = container_of(entry, struct kdpc, list);
			assert(kdpc->queued);
			kdpc->queued = 0;
		} else
			kdpc = NULL;
		spin_unlock_irqrestore(&kdpc_list_lock, flags);
		if (!kdpc)
			break;
		WORKTRACE("%p, %p, %p, %p, %p", kdpc, kdpc->func, kdpc->ctx,
			  kdpc->arg1, kdpc->arg2);
		assert_irql(_irql_ == DISPATCH_LEVEL);
		LIN2WIN4(kdpc->func, kdpc, kdpc->ctx, kdpc->arg1, kdpc->arg2);
		assert_irql(_irql_ == DISPATCH_LEVEL);
	}
	lower_irql(irql);
	WORKEXIT(return);
}

wstdcall void WIN_FUNC(KeFlushQueuedDpcs,0)
	(void)
{
	kdpc_worker(NULL);
}

BOOLEAN queue_kdpc(struct kdpc *kdpc)
{
	BOOLEAN ret;
	unsigned long flags;

	WORKENTER("%p", kdpc);
	spin_lock_irqsave(&kdpc_list_lock, flags);
	if (kdpc->queued)
		ret = FALSE;
	else {
		if (unlikely(kdpc->importance == HighImportance))
			InsertHeadList(&kdpc_list, &kdpc->list);
		else
			InsertTailList(&kdpc_list, &kdpc->list);
		kdpc->queued = 1;
		ret = TRUE;
	}
	spin_unlock_irqrestore(&kdpc_list_lock, flags);
	if (ret == TRUE)
		queue_work(ntos_wq, &kdpc_work);
	WORKTRACE("%d", ret);
	return ret;
}

BOOLEAN dequeue_kdpc(struct kdpc *kdpc)
{
	BOOLEAN ret;
	unsigned long flags;

	WORKENTER("%p", kdpc);
	spin_lock_irqsave(&kdpc_list_lock, flags);
	if (kdpc->queued) {
		RemoveEntryList(&kdpc->list);
		kdpc->queued = 0;
		ret = TRUE;
	} else
		ret = FALSE;
	spin_unlock_irqrestore(&kdpc_list_lock, flags);
	WORKTRACE("%d", ret);
	return ret;
}

wstdcall BOOLEAN WIN_FUNC(KeInsertQueueDpc,3)
	(struct kdpc *kdpc, void *arg1, void *arg2)
{
	WORKENTER("%p, %p, %p", kdpc, arg1, arg2);
	kdpc->arg1 = arg1;
	kdpc->arg2 = arg2;
	return queue_kdpc(kdpc);
}

wstdcall BOOLEAN WIN_FUNC(KeRemoveQueueDpc,1)
	(struct kdpc *kdpc)
{
	return dequeue_kdpc(kdpc);
}

wstdcall void WIN_FUNC(KeSetImportanceDpc,2)
	(struct kdpc *kdpc, enum kdpc_importance importance)
{
	kdpc->importance = importance;
}

static void ntos_work_worker(struct work_struct *dummy)
{
	struct ntos_work_item *ntos_work_item;
	struct nt_list *cur;

	while (1) {
		spin_lock_bh(&ntos_work_lock);
		cur = RemoveHeadList(&ntos_work_list);
		spin_unlock_bh(&ntos_work_lock);
		if (!cur)
			break;
		ntos_work_item = container_of(cur, struct ntos_work_item, list);
		WORKTRACE("%p: executing %p, %p, %p", current,
			  ntos_work_item->func, ntos_work_item->arg1,
			  ntos_work_item->arg2);
		LIN2WIN2(ntos_work_item->func, ntos_work_item->arg1,
			 ntos_work_item->arg2);
		kfree(ntos_work_item);
	}
	WORKEXIT(return);
}

int schedule_ntos_work_item(NTOS_WORK_FUNC func, void *arg1, void *arg2)
{
	struct ntos_work_item *ntos_work_item;

	WORKENTER("adding work: %p, %p, %p", func, arg1, arg2);
	ntos_work_item = kmalloc(sizeof(*ntos_work_item), irql_gfp());
	if (!ntos_work_item) {
		ERROR("couldn't allocate memory");
		return -ENOMEM;
	}
	ntos_work_item->func = func;
	ntos_work_item->arg1 = arg1;
	ntos_work_item->arg2 = arg2;
	spin_lock_bh(&ntos_work_lock);
	InsertTailList(&ntos_work_list, &ntos_work_item->list);
	spin_unlock_bh(&ntos_work_lock);
	queue_work(ntos_wq, &ntos_work);
	WORKEXIT(return 0);
}

wstdcall void WIN_FUNC(KeInitializeSpinLock,1)
	(NT_SPIN_LOCK *lock)
{
	ENTER6("%p", lock);
	nt_spin_lock_init(lock);
}

wstdcall void WIN_FUNC(KeAcquireSpinLock,2)
	(NT_SPIN_LOCK *lock, KIRQL *irql)
{
	ENTER6("%p", lock);
	*irql = nt_spin_lock_irql(lock, DISPATCH_LEVEL);
}

wstdcall void WIN_FUNC(KeReleaseSpinLock,2)
	(NT_SPIN_LOCK *lock, KIRQL oldirql)
{
	ENTER6("%p", lock);
	nt_spin_unlock_irql(lock, oldirql);
}

wstdcall void WIN_FUNC(KeAcquireSpinLockAtDpcLevel,1)
	(NT_SPIN_LOCK *lock)
{
	ENTER6("%p", lock);
	nt_spin_lock(lock);
}

wstdcall void WIN_FUNC(KeReleaseSpinLockFromDpcLevel,1)
	(NT_SPIN_LOCK *lock)
{
	ENTER6("%p", lock);
	nt_spin_unlock(lock);
}

wstdcall void WIN_FUNC(KeRaiseIrql,2)
	(KIRQL newirql, KIRQL *oldirql)
{
	ENTER6("%d", newirql);
	*oldirql = raise_irql(newirql);
}

wstdcall KIRQL WIN_FUNC(KeRaiseIrqlToDpcLevel,0)
	(void)
{
	return raise_irql(DISPATCH_LEVEL);
}

wstdcall void WIN_FUNC(KeLowerIrql,1)
	(KIRQL irql)
{
	ENTER6("%d", irql);
	lower_irql(irql);
}

wstdcall KIRQL WIN_FUNC(KeAcquireSpinLockRaiseToDpc,1)
	(NT_SPIN_LOCK *lock)
{
	ENTER6("%p", lock);
	return nt_spin_lock_irql(lock, DISPATCH_LEVEL);
}

wstdcall void *WIN_FUNC(ExAllocatePoolWithTag,3)
	(enum pool_type pool_type, SIZE_T size, ULONG tag)
{
	void *addr;

	ENTER4("pool_type: %d, size: %zu, tag: 0x%x", pool_type, size, tag);
	assert_irql(_irql_ <= DISPATCH_LEVEL);
	if (size < PAGE_SIZE)
		addr = kmalloc(size, irql_gfp());
	else {
		if (irql_gfp() & GFP_ATOMIC) {
			addr = nvmalloc(size, GFP_ATOMIC | __GFP_HIGHMEM,
					 PAGE_KERNEL);
			TRACE1("%p, %zu", addr, size);
		} else {
			addr = vmalloc(size);
			TRACE1("%p, %zu", addr, size);
		}
	}
	DBG_BLOCK(1) {
		if (addr)
			TRACE4("addr: %p, %zu", addr, size);
		else
			TRACE1("failed: %zu", size);
	}
	return addr;
}
WIN_FUNC_DECL(ExAllocatePoolWithTag,3)

wstdcall void WIN_FUNC(ExFreePoolWithTag,2)
	(void *addr, ULONG tag)
{
	TRACE4("%p", addr);
	if ((unsigned long)addr < VMALLOC_START ||
	    (unsigned long)addr >= VMALLOC_END)
		kfree(addr);
	else
		vfree(addr);

	EXIT4(return);
}

wstdcall void WIN_FUNC(ExFreePool,1)
	(void *addr)
{
	ExFreePoolWithTag(addr, 0);
}
WIN_FUNC_DECL(ExFreePool,1)

wstdcall void WIN_FUNC(ExInitializeNPagedLookasideList,7)
	(struct npaged_lookaside_list *lookaside,
	 LOOKASIDE_ALLOC_FUNC *alloc_func, LOOKASIDE_FREE_FUNC *free_func,
	 ULONG flags, SIZE_T size, ULONG tag, USHORT depth)
{
	ENTER3("lookaside: %p, size: %zu, flags: %u, head: %p, "
	       "alloc: %p, free: %p", lookaside, size, flags,
	       lookaside, alloc_func, free_func);

	memset(lookaside, 0, sizeof(*lookaside));

	lookaside->size = size;
	lookaside->tag = tag;
	lookaside->depth = 4;
	lookaside->maxdepth = 256;
	lookaside->pool_type = NonPagedPool;

	if (alloc_func)
		lookaside->alloc_func = alloc_func;
	else
		lookaside->alloc_func = WIN_FUNC_PTR(ExAllocatePoolWithTag,3);
	if (free_func)
		lookaside->free_func = free_func;
	else
		lookaside->free_func = WIN_FUNC_PTR(ExFreePool,1);

#ifndef CONFIG_X86_64
	nt_spin_lock_init(&lookaside->obsolete);
#endif
	EXIT3(return);
}

wstdcall void WIN_FUNC(ExDeleteNPagedLookasideList,1)
	(struct npaged_lookaside_list *lookaside)
{
	struct nt_slist *entry;

	ENTER3("lookaside = %p", lookaside);
	while ((entry = ExpInterlockedPopEntrySList(&lookaside->head)))
		LIN2WIN1(lookaside->free_func, entry);
	EXIT3(return);
}

wstdcall NTSTATUS WIN_FUNC(ExCreateCallback,4)
	(struct callback_object **object, struct object_attributes *attributes,
	 BOOLEAN create, BOOLEAN allow_multiple_callbacks)
{
	struct callback_object *obj;

	ENTER2("");
	spin_lock_bh(&ntoskernel_lock);
	nt_list_for_each_entry(obj, &callback_objects, callback_funcs) {
		if (obj->attributes == attributes) {
			spin_unlock_bh(&ntoskernel_lock);
			*object = obj;
			return STATUS_SUCCESS;
		}
	}
	spin_unlock_bh(&ntoskernel_lock);
	obj = allocate_object(sizeof(struct callback_object),
			      OBJECT_TYPE_CALLBACK, NULL);
	if (!obj)
		EXIT2(return STATUS_INSUFFICIENT_RESOURCES);
	InitializeListHead(&obj->callback_funcs);
	nt_spin_lock_init(&obj->lock);
	obj->allow_multiple_callbacks = allow_multiple_callbacks;
	obj->attributes = attributes;
	*object = obj;
	EXIT2(return STATUS_SUCCESS);
}

wstdcall void *WIN_FUNC(ExRegisterCallback,3)
	(struct callback_object *object, PCALLBACK_FUNCTION func, void *context)
{
	struct callback_func *callback;
	KIRQL irql;

	ENTER2("");
	irql = nt_spin_lock_irql(&object->lock, DISPATCH_LEVEL);
	if (object->allow_multiple_callbacks == FALSE &&
	    !IsListEmpty(&object->callback_funcs)) {
		nt_spin_unlock_irql(&object->lock, irql);
		EXIT2(return NULL);
	}
	nt_spin_unlock_irql(&object->lock, irql);
	callback = kmalloc(sizeof(*callback), GFP_KERNEL);
	if (!callback) {
		ERROR("couldn't allocate memory");
		return NULL;
	}
	callback->func = func;
	callback->context = context;
	callback->object = object;
	irql = nt_spin_lock_irql(&object->lock, DISPATCH_LEVEL);
	InsertTailList(&object->callback_funcs, &callback->list);
	nt_spin_unlock_irql(&object->lock, irql);
	EXIT2(return callback);
}

wstdcall void WIN_FUNC(ExUnregisterCallback,1)
	(struct callback_func *callback)
{
	struct callback_object *object;
	KIRQL irql;

	ENTER3("%p", callback);
	if (!callback)
		return;
	object = callback->object;
	irql = nt_spin_lock_irql(&object->lock, DISPATCH_LEVEL);
	RemoveEntryList(&callback->list);
	nt_spin_unlock_irql(&object->lock, irql);
	kfree(callback);
	return;
}

wstdcall void WIN_FUNC(ExNotifyCallback,3)
	(struct callback_object *object, void *arg1, void *arg2)
{
	struct callback_func *callback;
	KIRQL irql;

	ENTER3("%p", object);
	irql = nt_spin_lock_irql(&object->lock, DISPATCH_LEVEL);
	nt_list_for_each_entry(callback, &object->callback_funcs, list) {
		LIN2WIN3(callback->func, callback->context, arg1, arg2);
	}
	nt_spin_unlock_irql(&object->lock, irql);
	return;
}

/* check and set signaled state; should be called with dispatcher_lock held */
/* @grab indicates if the event should be grabbed or checked
 * - note that a semaphore may stay in signaled state for multiple
 * 'grabs' if the count is > 1 */
static int grab_object(struct dispatcher_header *dh,
		       struct task_struct *thread, int grab)
{
	EVENTTRACE("%p, %p, %d, %d", dh, thread, grab, dh->signal_state);
	if (unlikely(is_mutex_object(dh))) {
		struct nt_mutex *nt_mutex;
		nt_mutex = container_of(dh, struct nt_mutex, dh);
		EVENTTRACE("%p, %p, %d, %p, %d", nt_mutex,
			   nt_mutex->owner_thread, dh->signal_state,
			   thread, grab);
		/* either no thread owns the mutex or this thread owns
		 * it */
		assert(dh->signal_state == 1 && nt_mutex->owner_thread == NULL);
		assert(dh->signal_state < 1 && nt_mutex->owner_thread != NULL);
		if ((dh->signal_state == 1 && nt_mutex->owner_thread == NULL) ||
		    nt_mutex->owner_thread == thread) {
			if (grab) {
				dh->signal_state--;
				nt_mutex->owner_thread = thread;
			}
			EVENTEXIT(return 1);
		}
	} else if (dh->signal_state > 0) {
		/* to grab, decrement signal_state for synchronization
		 * or semaphore objects */
		if (grab && (is_synch_object(dh) || is_semaphore_object(dh)))
			dh->signal_state--;
		EVENTEXIT(return 1);
	}
	EVENTEXIT(return 0);
}

/* this function should be called holding dispatcher_lock */
static void object_signaled(struct dispatcher_header *dh)
{
	struct nt_list *cur, *next;
	struct wait_block *wb;

	EVENTENTER("%p", dh);
	nt_list_for_each_safe(cur, next, &dh->wait_blocks) {
		wb = container_of(cur, struct wait_block, list);
		assert(wb->thread != NULL);
		assert(wb->object == NULL);
		if (!grab_object(dh, wb->thread, 1))
			continue;
		EVENTTRACE("%p (%p): waking %p", dh, wb, wb->thread);
		RemoveEntryList(cur);
		wb->object = dh;
		*(wb->wait_done) = 1;
		wake_up_process(wb->thread);
	}
	EVENTEXIT(return);
}

wstdcall NTSTATUS WIN_FUNC(KeWaitForMultipleObjects,8)
	(ULONG count, void *object[], enum wait_type wait_type,
	 KWAIT_REASON wait_reason, KPROCESSOR_MODE wait_mode,
	 BOOLEAN alertable, LARGE_INTEGER *timeout,
	 struct wait_block *wait_block_array)
{
	int i, res = 0, wait_count, wait_done;
	typeof(jiffies) wait_hz = 0;
	struct wait_block *wb, wb_array[THREAD_WAIT_OBJECTS];
	struct dispatcher_header *dh;
	KIRQL irql = current_irql();

	EVENTENTER("%p, %d, %u, %p", current, count, wait_type, timeout);

	if (count > MAX_WAIT_OBJECTS ||
	    (count > THREAD_WAIT_OBJECTS && wait_block_array == NULL))
		EVENTEXIT(return STATUS_INVALID_PARAMETER);

	if (wait_block_array == NULL)
		wb = wb_array;
	else
		wb = wait_block_array;

	/* If *timeout == 0: In the case of WaitAny, if an object can
	 * be grabbed (object is in signaled state), grab and
	 * return. In the case of WaitAll, we have to first make sure
	 * all objects can be grabbed. If any/some of them can't be
	 * grabbed, either we return STATUS_TIMEOUT or wait for them,
	 * depending on how to satisfy wait. If all of them can be
	 * grabbed, we will grab them in the next loop below */

	spin_lock_bh(&dispatcher_lock);
	for (i = wait_count = 0; i < count; i++) {
		dh = object[i];
		EVENTTRACE("%p: event %p (%d)", current, dh, dh->signal_state);
		/* wait_type == 1 for WaitAny, 0 for WaitAll */
		if (grab_object(dh, current, wait_type)) {
			if (wait_type == WaitAny) {
				spin_unlock_bh(&dispatcher_lock);
				EVENTEXIT(return STATUS_WAIT_0 + i);
			}
		} else {
			EVENTTRACE("%p: wait for %p", current, dh);
			wait_count++;
		}
	}

	if (timeout && *timeout == 0 && wait_count) {
		spin_unlock_bh(&dispatcher_lock);
		EVENTEXIT(return STATUS_TIMEOUT);
	}

	/* get the list of objects the thread needs to wait on and add
	 * the thread on the wait list for each such object */
	/* if *timeout == 0, this step will grab all the objects */
	wait_done = 0;
	for (i = 0; i < count; i++) {
		dh = object[i];
		EVENTTRACE("%p: event %p (%d)", current, dh, dh->signal_state);
		wb[i].object = NULL;
		if (grab_object(dh, current, 1)) {
			EVENTTRACE("%p: no wait for %p (%d)",
				   current, dh, dh->signal_state);
			/* mark that we are not waiting on this object */
			wb[i].thread = NULL;
		} else {
			wb[i].wait_done = &wait_done;
			wb[i].thread = current;
			EVENTTRACE("%p: wait for %p", current, dh);
			InsertTailList(&dh->wait_blocks, &wb[i].list);
		}
	}
	spin_unlock_bh(&dispatcher_lock);
	if (wait_count == 0)
		EVENTEXIT(return STATUS_SUCCESS);

	assert(timeout == NULL || *timeout != 0);
	if (timeout == NULL)
		wait_hz = 0;
	else
		wait_hz = SYSTEM_TIME_TO_HZ(*timeout);

	if (irql >= DISPATCH_LEVEL) {
		WARNING("attempt to wait with irql %d", irql);
		EVENTEXIT(return STATUS_INVALID_PARAMETER);
	}
	EVENTTRACE("%p: sleep for %ld on %p", current, wait_hz, &wait_done);
	/* we don't honor 'alertable' - according to description for
	 * this, even if waiting in non-alertable state, thread may be
	 * alerted in some circumstances */
	while (wait_count) {
		res = wait_condition(wait_done, wait_hz, TASK_INTERRUPTIBLE);
		spin_lock_bh(&dispatcher_lock);
		EVENTTRACE("%p woke up: %d, %d", current, res, wait_done);
		/* the event may have been set by the time
		 * wrap_wait_event returned and spinlock obtained, so
		 * don't rely on value of 'res' - check event status */
		if (!wait_done) {
			assert(res <= 0);
			/* timed out or interrupted; remove from wait list */
			for (i = 0; i < count; i++) {
				if (!wb[i].thread)
					continue;
				EVENTTRACE("%p: timedout, dequeue %p (%p)",
					   current, object[i], wb[i].object);
				assert(wb[i].object == NULL);
				RemoveEntryList(&wb[i].list);
			}
			spin_unlock_bh(&dispatcher_lock);
			if (res < 0)
				EVENTEXIT(return STATUS_ALERTED);
			else
				EVENTEXIT(return STATUS_TIMEOUT);
		}
		assert(res > 0);
		/* woken because object(s) signaled */
		for (i = 0; wait_count && i < count; i++) {
			if (!wb[i].thread || !wb[i].object)
				continue;
			DBG_BLOCK(1) {
				if (wb[i].object != object[i]) {
					EVENTTRACE("oops %p != %p",
						   wb[i].object, object[i]);
					continue;
				}
			}
			wait_count--;
			if (wait_type == WaitAny) {
				int j;
				/* done; remove from rest of wait list */
				for (j = i + 1; j < count; j++) {
					if (wb[j].thread && !wb[j].object)
						RemoveEntryList(&wb[j].list);
				}
				spin_unlock_bh(&dispatcher_lock);
				EVENTEXIT(return STATUS_WAIT_0 + i);
			}
		}
		wait_done = 0;
		spin_unlock_bh(&dispatcher_lock);
		if (wait_count == 0)
			EVENTEXIT(return STATUS_SUCCESS);

		/* this thread is still waiting for more objects, so
		 * let it wait for remaining time and those objects */
		if (timeout)
			wait_hz = res;
		else
			wait_hz = 0;
	}
	/* should never reach here, but compiler wants return value */
	ERROR("%p: wait_hz: %ld", current, wait_hz);
	EVENTEXIT(return STATUS_SUCCESS);
}

wstdcall NTSTATUS WIN_FUNC(KeWaitForSingleObject,5)
	(void *object, KWAIT_REASON wait_reason, KPROCESSOR_MODE wait_mode,
	 BOOLEAN alertable, LARGE_INTEGER *timeout)
{
	return KeWaitForMultipleObjects(1, &object, WaitAny, wait_reason,
					wait_mode, alertable, timeout, NULL);
}

wstdcall void WIN_FUNC(KeInitializeEvent,3)
	(struct nt_event *nt_event, enum event_type type, BOOLEAN state)
{
	EVENTENTER("event = %p, type = %d, state = %d", nt_event, type, state);
	initialize_object(&nt_event->dh, (enum dh_type)type, state);
	EVENTEXIT(return);
}

wstdcall LONG WIN_FUNC(KeSetEvent,3)
	(struct nt_event *nt_event, KPRIORITY incr, BOOLEAN wait)
{
	LONG old_state;

	EVENTENTER("%p, %d", nt_event, nt_event->dh.type);
	if (wait == TRUE)
		WARNING("wait = %d, not yet implemented", wait);
	spin_lock_bh(&dispatcher_lock);
	old_state = nt_event->dh.signal_state;
	nt_event->dh.signal_state = 1;
	if (old_state == 0)
		object_signaled(&nt_event->dh);
	spin_unlock_bh(&dispatcher_lock);
	EVENTEXIT(return old_state);
}

wstdcall void WIN_FUNC(KeClearEvent,1)
	(struct nt_event *nt_event)
{
	EVENTENTER("%p", nt_event);
	nt_event->dh.signal_state = 0;
	EVENTEXIT(return);
}

wstdcall LONG WIN_FUNC(KeResetEvent,1)
	(struct nt_event *nt_event)
{
	LONG old_state;

	EVENTENTER("%p", nt_event);
	old_state = xchg(&nt_event->dh.signal_state, 0);
	EVENTEXIT(return old_state);
}

wstdcall LONG WIN_FUNC(KeReadStateEvent,1)
	(struct nt_event *nt_event)
{
	LONG state;

	state = nt_event->dh.signal_state;
	EVENTTRACE("%d", state);
	return state;
}

wstdcall void WIN_FUNC(KeInitializeMutex,2)
	(struct nt_mutex *mutex, ULONG level)
{
	EVENTENTER("%p", mutex);
	initialize_object(&mutex->dh, MutexObject, 1);
	mutex->dh.size = sizeof(*mutex);
	InitializeListHead(&mutex->list);
	mutex->abandoned = FALSE;
	mutex->apc_disable = 1;
	mutex->owner_thread = NULL;
	EVENTEXIT(return);
}

wstdcall LONG WIN_FUNC(KeReleaseMutex,2)
	(struct nt_mutex *mutex, BOOLEAN wait)
{
	LONG ret;
	struct task_struct *thread;

	EVENTENTER("%p, %d, %p", mutex, wait, current);
	if (wait == TRUE)
		WARNING("wait: %d", wait);
	thread = current;
	spin_lock_bh(&dispatcher_lock);
	EVENTTRACE("%p, %p, %p, %d", mutex, thread, mutex->owner_thread,
		   mutex->dh.signal_state);
	if ((mutex->owner_thread == thread) && (mutex->dh.signal_state <= 0)) {
		ret = mutex->dh.signal_state++;
		if (ret == 0) {
			mutex->owner_thread = NULL;
			object_signaled(&mutex->dh);
		}
	} else {
		ret = STATUS_MUTANT_NOT_OWNED;
		WARNING("invalid mutex: %p, %p, %p", mutex, mutex->owner_thread,
			thread);
	}
	EVENTTRACE("%p, %p, %p, %d", mutex, thread, mutex->owner_thread,
		   mutex->dh.signal_state);
	spin_unlock_bh(&dispatcher_lock);
	EVENTEXIT(return ret);
}

wstdcall void WIN_FUNC(KeInitializeSemaphore,3)
	(struct nt_semaphore *semaphore, LONG count, LONG limit)
{
	EVENTENTER("%p: %d", semaphore, count);
	/* if limit > 1, we need to satisfy as many waits (until count
	 * becomes 0); so we keep decrementing count every time a wait
	 * is satisfied */
	initialize_object(&semaphore->dh, SemaphoreObject, count);
	semaphore->dh.size = sizeof(*semaphore);
	semaphore->limit = limit;
	EVENTEXIT(return);
}

wstdcall LONG WIN_FUNC(KeReleaseSemaphore,4)
	(struct nt_semaphore *semaphore, KPRIORITY incr, LONG adjustment,
	 BOOLEAN wait)
{
	LONG ret;

	EVENTENTER("%p", semaphore);
	spin_lock_bh(&dispatcher_lock);
	ret = semaphore->dh.signal_state;
	assert(ret >= 0);
	if (semaphore->dh.signal_state + adjustment <= semaphore->limit)
		semaphore->dh.signal_state += adjustment;
	else {
		WARNING("releasing %d over limit %d", adjustment,
			semaphore->limit);
		semaphore->dh.signal_state = semaphore->limit;
	}
	if (semaphore->dh.signal_state > 0)
		object_signaled(&semaphore->dh);
	spin_unlock_bh(&dispatcher_lock);
	EVENTEXIT(return ret);
}

wstdcall NTSTATUS WIN_FUNC(KeDelayExecutionThread,3)
	(KPROCESSOR_MODE wait_mode, BOOLEAN alertable, LARGE_INTEGER *interval)
{
	int res;
	long timeout;

	if (wait_mode != 0)
		ERROR("invalid wait_mode %d", wait_mode);

	timeout = SYSTEM_TIME_TO_HZ(*interval);
	EVENTTRACE("%p, %lld, %ld", current, *interval, timeout);
	if (timeout <= 0)
		EVENTEXIT(return STATUS_SUCCESS);

	if (alertable)
		set_current_state(TASK_INTERRUPTIBLE);
	else
		set_current_state(TASK_UNINTERRUPTIBLE);

	res = schedule_timeout(timeout);
	EVENTTRACE("%p, %d", current, res);
	if (res == 0)
		EVENTEXIT(return STATUS_SUCCESS);
	else
		EVENTEXIT(return STATUS_ALERTED);
}

wstdcall ULONGLONG WIN_FUNC(KeQueryInterruptTime,0)
	(void)
{
	EXIT5(return jiffies * TICKSPERJIFFY);
}

wstdcall ULONG WIN_FUNC(KeQueryTimeIncrement,0)
	(void)
{
	EXIT5(return TICKSPERSEC / HZ);
}

wstdcall void WIN_FUNC(KeQuerySystemTime,1)
	(LARGE_INTEGER *time)
{
	*time = ticks_1601();
	TRACE5("%llu, %lu", *time, jiffies);
}

wstdcall void WIN_FUNC(KeQueryTickCount,1)
	(LARGE_INTEGER *count)
{
	*count = jiffies;
}

wstdcall LARGE_INTEGER WIN_FUNC(KeQueryPerformanceCounter,1)
	(LARGE_INTEGER *counter)
{
	if (counter)
		*counter = HZ;
	return jiffies;
}

wstdcall KAFFINITY WIN_FUNC(KeQueryActiveProcessors,0)
	(void)
{
	int i, n;
	KAFFINITY bits = 0;
	n = num_online_cpus();
	for (i = 0; i < n; i++)
		bits = (bits << 1) | 1;
	return bits;
}

struct nt_thread *get_current_nt_thread(void)
{
	struct task_struct *task = current;
	struct nt_thread *thread;
	struct common_object_header *header;

	TRACE6("task: %p", task);
	thread = NULL;
	spin_lock_bh(&ntoskernel_lock);
	nt_list_for_each_entry(header, &object_list, list) {
		TRACE6("%p, %d", header, header->type);
		if (header->type != OBJECT_TYPE_NT_THREAD)
			break;
		thread = HEADER_TO_OBJECT(header);
		TRACE6("%p, %p", thread, thread->task);
		if (thread->task == task)
			break;
		else
			thread = NULL;
	}
	spin_unlock_bh(&ntoskernel_lock);
	if (thread == NULL)
		TRACE4("couldn't find thread for task %p, %d", task, task->pid);
	TRACE6("%p", thread);
	return thread;
}

static struct task_struct *get_nt_thread_task(struct nt_thread *thread)
{
	struct task_struct *task;
	struct common_object_header *header;

	TRACE6("%p", thread);
	task = NULL;
	spin_lock_bh(&ntoskernel_lock);
	nt_list_for_each_entry(header, &object_list, list) {
		TRACE6("%p, %d", header, header->type);
		if (header->type != OBJECT_TYPE_NT_THREAD)
			break;
		if (thread == HEADER_TO_OBJECT(header)) {
			task = thread->task;
			break;
		}
	}
	spin_unlock_bh(&ntoskernel_lock);
	if (task == NULL)
		TRACE2("%p: couldn't find task for %p", current, thread);
	return task;
}

static struct nt_thread *create_nt_thread(struct task_struct *task)
{
	struct nt_thread *thread;
	thread = allocate_object(sizeof(*thread), OBJECT_TYPE_NT_THREAD, NULL);
	if (!thread) {
		ERROR("couldn't allocate thread object");
		EXIT2(return NULL);
	}
	thread->task = task;
	if (task)
		thread->pid = task->pid;
	else
		thread->pid = 0;
	nt_spin_lock_init(&thread->lock);
	InitializeListHead(&thread->irps);
	initialize_object(&thread->dh, ThreadObject, 0);
	thread->dh.size = sizeof(*thread);
	thread->prio = LOW_PRIORITY;
	return thread;
}

wstdcall struct nt_thread *WIN_FUNC(KeGetCurrentThread,0)
	(void)
{
	struct nt_thread *thread = get_current_nt_thread();
	TRACE2("%p, %p", thread, current);
	return thread;
}

wstdcall KPRIORITY WIN_FUNC(KeQueryPriorityThread,1)
	(struct nt_thread *thread)
{
	KPRIORITY prio;
	struct task_struct *task;

	TRACE2("%p", thread);
#ifdef CONFIG_X86_64
	/* sis163u driver for amd64 passes 0x1f from thread created by
	 * PsCreateSystemThread - no idea what is 0x1f */
	if (thread == (void *)0x1f)
		thread = get_current_nt_thread();
#endif
	if (!thread) {
		TRACE2("invalid thread");
		EXIT2(return LOW_REALTIME_PRIORITY);
	}
	task = get_nt_thread_task(thread);
	if (!task) {
		TRACE2("couldn't find task for thread: %p", thread);
		EXIT2(return LOW_REALTIME_PRIORITY);
	}

	prio = thread->prio;

	TRACE2("%d", prio);
	return prio;
}

wstdcall KPRIORITY WIN_FUNC(KeSetPriorityThread,2)
	(struct nt_thread *thread, KPRIORITY prio)
{
	KPRIORITY old_prio;
	struct task_struct *task;

	TRACE2("thread: %p, priority = %u", thread, prio);
#ifdef CONFIG_X86_64
	if (thread == (void *)0x1f)
		thread = get_current_nt_thread();
#endif
	if (!thread) {
		TRACE2("invalid thread");
		EXIT2(return LOW_REALTIME_PRIORITY);
	}
	task = get_nt_thread_task(thread);
	if (!task) {
		TRACE2("couldn't find task for thread: %p", thread);
		EXIT2(return LOW_REALTIME_PRIORITY);
	}

	old_prio = thread->prio;
	thread->prio = prio;

	TRACE2("%d, %d", old_prio, thread->prio);
	return old_prio;
}

struct thread_trampoline {
	void (*func)(void *) wstdcall;
	void *ctx;
	struct nt_thread *thread;
	struct completion started;
};

static int ntdriver_thread(void *data)
{
	struct thread_trampoline *thread_tramp = data;
	/* yes, a tramp! */
	typeof(thread_tramp->func) func = thread_tramp->func;
	typeof(thread_tramp->ctx) ctx = thread_tramp->ctx;

	thread_tramp->thread->task = current;
	thread_tramp->thread->pid = current->pid;
	TRACE2("thread: %p, task: %p (%d)", thread_tramp->thread,
	       current, current->pid);
	complete(&thread_tramp->started);

#ifdef PF_NOFREEZE
	current->flags |= PF_NOFREEZE;
#endif
	strncpy(current->comm, "ntdriver", sizeof(current->comm));
	current->comm[sizeof(current->comm)-1] = 0;
	LIN2WIN1(func, ctx);
	ERROR("task: %p", current);
	return 0;
}

wstdcall NTSTATUS WIN_FUNC(PsCreateSystemThread,7)
	(void **handle, ULONG access, void *obj_attr, void *process,
	 void *client_id, void (*func)(void *) wstdcall, void *ctx)
{
	struct thread_trampoline thread_tramp;

	ENTER2("handle = %p, access = %u, obj_attr = %p, process = %p, "
	       "client_id = %p, func = %p, context = %p", handle, access,
	       obj_attr, process, client_id, func, ctx);

	thread_tramp.thread = create_nt_thread(NULL);
	if (!thread_tramp.thread) {
		ERROR("couldn't allocate thread object");
		EXIT2(return STATUS_RESOURCES);
	}
	TRACE2("thread: %p", thread_tramp.thread);
	thread_tramp.func = func;
	thread_tramp.ctx = ctx;
	init_completion(&thread_tramp.started);

	thread_tramp.thread->task = kthread_run(ntdriver_thread,
						&thread_tramp, "ntdriver");
	if (IS_ERR(thread_tramp.thread->task)) {
		free_object(thread_tramp.thread);
		EXIT2(return STATUS_FAILURE);
	}
	TRACE2("created task: %p", thread_tramp.thread->task);

	wait_for_completion(&thread_tramp.started);
	*handle = OBJECT_TO_HEADER(thread_tramp.thread);
	TRACE2("created thread: %p, %p", thread_tramp.thread, *handle);
	EXIT2(return STATUS_SUCCESS);
}

wstdcall NTSTATUS WIN_FUNC(PsTerminateSystemThread,1)
	(NTSTATUS status)
{
	struct nt_thread *thread;

	TRACE2("%p, %08X", current, status);
	thread = get_current_nt_thread();
	TRACE2("%p", thread);
	if (thread) {
		KeSetEvent((struct nt_event *)&thread->dh, 0, FALSE);
		while (1) {
			struct nt_list *ent;
			struct irp *irp;
			KIRQL irql;
			irql = nt_spin_lock_irql(&thread->lock, DISPATCH_LEVEL);
			ent = RemoveHeadList(&thread->irps);
			nt_spin_unlock_irql(&thread->lock, irql);
			if (!ent)
				break;
			irp = container_of(ent, struct irp, thread_list);
			IOTRACE("%p", irp);
			IoCancelIrp(irp);
		}
		/* the driver may later query this status with
		 * ZwQueryInformationThread */
		thread->status = status;
	} else
		ERROR("couldn't find thread for task: %p", current);

    #if LINUX_VERSION_CODE > KERNEL_VERSION(5,16,0)
		#define complete_and_exit kthread_complete_and_exit
    #endif
    
	complete_and_exit(NULL, status);
	ERROR("oops: %p, %d", thread->task, thread->pid);
	return STATUS_FAILURE;
}

wstdcall BOOLEAN WIN_FUNC(KeRemoveEntryDeviceQueue,2)
	(struct kdevice_queue *dev_queue, struct kdevice_queue_entry *entry)
{
	struct kdevice_queue_entry *e;
	KIRQL irql;

	irql = nt_spin_lock_irql(&dev_queue->lock, DISPATCH_LEVEL);
	nt_list_for_each_entry(e, &dev_queue->list, list) {
		if (e == entry) {
			RemoveEntryList(&e->list);
			nt_spin_unlock_irql(&dev_queue->lock, irql);
			return TRUE;
		}
	}
	nt_spin_unlock_irql(&dev_queue->lock, irql);
	return FALSE;
}

wstdcall BOOLEAN WIN_FUNC(KeSynchronizeExecution,3)
	(struct kinterrupt *interrupt, PKSYNCHRONIZE_ROUTINE synch_routine,
	 void *ctx)
{
	BOOLEAN ret;
	unsigned long flags;

	nt_spin_lock_irqsave(interrupt->actual_lock, flags);
	ret = LIN2WIN1(synch_routine, ctx);
	nt_spin_unlock_irqrestore(interrupt->actual_lock, flags);
	TRACE6("%d", ret);
	return ret;
}

wstdcall BOOLEAN WIN_FUNC(KeRegisterBugCheckReasonCallback,4)
	(void *callback_record, void *callback_routine, UINT reason,
	 char *component)
{
	TRACE1("callback_record: %p, callback_routine: %p, reason: %d, "
	       "component: %s", callback_record, callback_routine, reason,
	       component);
	TODO();
	return FALSE;
}

wstdcall BOOLEAN WIN_FUNC(KeDeregisterBugCheckReasonCallback,1)
	(void *callback_record)
{
	TRACE1("callback_record: %p", callback_record);
	TODO();
	return TRUE;
}

wstdcall void *WIN_FUNC(MmAllocateContiguousMemorySpecifyCache,5)
	(SIZE_T size, PHYSICAL_ADDRESS lowest, PHYSICAL_ADDRESS highest,
	 PHYSICAL_ADDRESS boundary, enum memory_caching_type cache_type)
{
	void *addr;
	gfp_t flags;

	ENTER2("%zu, 0x%llx, 0x%llx, 0x%llx, %d", size, lowest,
	       highest, boundary, cache_type);
	flags = irql_gfp();
	addr = wrap_get_free_pages(flags, size);
	TRACE2("%p, %zu, 0x%x", addr, size, flags);
	if (addr && ((virt_to_phys(addr) + size) <= highest))
		EXIT2(return addr);
#ifdef CONFIG_X86_64
	/* GFP_DMA is really only 16MB even on x86-64, but there is no
	 * other zone available */
	if (highest <= DMA_BIT_MASK(31))
		flags |= __GFP_DMA;
	else if (highest <= DMA_BIT_MASK(32))
		flags |= __GFP_DMA32;
#else
	if (highest <= DMA_BIT_MASK(24))
		flags |= __GFP_DMA;
	else if (highest > DMA_BIT_MASK(30))
		flags |= __GFP_HIGHMEM;
#endif
	if (addr)
		free_pages((unsigned long)addr, get_order(size));
	addr = wrap_get_free_pages(flags, size);
	TRACE2("%p, %zu, 0x%x", addr, size, flags);
	return addr;
}

wstdcall void WIN_FUNC(MmFreeContiguousMemorySpecifyCache,3)
	(void *base, SIZE_T size, enum memory_caching_type cache_type)
{
	TRACE2("%p, %zu", base, size);
	free_pages((unsigned long)base, get_order(size));
}

wstdcall PHYSICAL_ADDRESS WIN_FUNC(MmGetPhysicalAddress,1)
	(void *base)
{
	unsigned long phy = virt_to_phys(base);
	TRACE2("%p, %p", base, (void *)phy);
	return phy;
}

wstdcall void WIN_FUNC(MmGetSystemRoutineAddress,1)
    (struct unicode_string name)
{
    struct ansi_string ansi;
    if (RtlUnicodeStringToAnsiString(&ansi, &name, TRUE) == STATUS_SUCCESS) {
        WARNING("STUB: MmGetSystemRoutineAddress: %s", ansi.buf);
        RtlFreeAnsiString(&ansi);
    }
}

/* Atheros card with pciid 168C:0014 calls this function with 0xf0000
 * and 0xf6ef0 address, and then check for things that seem to be
 * related to ACPI: "_SM_" and "_DMI_". This may be the hack they do
 * to check if this card is installed in IBM thinkpads; we can
 * probably get this device to work if we create a buffer with the
 * strings as required by the driver and return virtual address for
 * that address instead */
wstdcall void __iomem *WIN_FUNC(MmMapIoSpace,3)
	(PHYSICAL_ADDRESS phys_addr, SIZE_T size,
	 enum memory_caching_type cache)
{
	void __iomem *virt;
	ENTER1("cache type: %d", cache);
	if (cache == MmCached) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
		virt = ioremap_cache(phys_addr, size);
#else
		virt = ioremap(phys_addr, size);
#endif
	} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
		virt = ioremap(phys_addr, size);
#else
		virt = ioremap_nocache(phys_addr, size);
#endif
	}
	TRACE1("%llx, %zu, %p", phys_addr, size, virt);
	return virt;
}

wstdcall void WIN_FUNC(MmUnmapIoSpace,2)
	(void __iomem *addr, SIZE_T size)
{
	ENTER1("%p, %zu", addr, size);
	iounmap(addr);
	return;
}

wstdcall ULONG WIN_FUNC(MmSizeOfMdl,2)
	(void *base, ULONG length)
{
	return sizeof(struct mdl) +
	       (sizeof(PFN_NUMBER) * SPAN_PAGES(base, length));
}

struct mdl *allocate_init_mdl(void *virt, ULONG length)
{
	struct wrap_mdl *wrap_mdl;
	struct mdl *mdl;
	int mdl_size = MmSizeOfMdl(virt, length);

	if (mdl_size <= MDL_CACHE_SIZE) {
		wrap_mdl = kmem_cache_alloc(mdl_cache, irql_gfp());
		if (!wrap_mdl)
			return NULL;
		spin_lock_bh(&dispatcher_lock);
		InsertHeadList(&wrap_mdl_list, &wrap_mdl->list);
		spin_unlock_bh(&dispatcher_lock);
		mdl = wrap_mdl->mdl;
		TRACE3("allocated mdl from cache: %p(%p), %p(%d)",
		       wrap_mdl, mdl, virt, length);
		memset(mdl, 0, MDL_CACHE_SIZE);
		MmInitializeMdl(mdl, virt, length);
		/* mark the MDL as allocated from cache pool so when
		 * it is freed, we free it back to the pool */
		mdl->flags = MDL_ALLOCATED_FIXED_SIZE | MDL_CACHE_ALLOCATED;
	} else {
		wrap_mdl =
			kmalloc(sizeof(*wrap_mdl) + mdl_size, irql_gfp());
		if (!wrap_mdl)
			return NULL;
		mdl = wrap_mdl->mdl;
		TRACE3("allocated mdl from memory: %p(%p), %p(%d)",
		       wrap_mdl, mdl, virt, length);
		spin_lock_bh(&dispatcher_lock);
		InsertHeadList(&wrap_mdl_list, &wrap_mdl->list);
		spin_unlock_bh(&dispatcher_lock);
		memset(mdl, 0, mdl_size);
		MmInitializeMdl(mdl, virt, length);
		mdl->flags = MDL_ALLOCATED_FIXED_SIZE;
	}
	return mdl;
}

void free_mdl(struct mdl *mdl)
{
	/* A driver may allocate Mdl with NdisAllocateBuffer and free
	 * with IoFreeMdl (e.g., 64-bit Broadcom). Since we need to
	 * treat buffers allocated with Ndis calls differently, we
	 * must call NdisFreeBuffer if it is allocated with Ndis
	 * function. We set 'pool' field in Ndis functions. */
	if (!mdl)
		return;
	if (mdl->pool)
		NdisFreeBuffer(mdl);
	else {
		struct wrap_mdl *wrap_mdl = (struct wrap_mdl *)
			((char *)mdl - offsetof(struct wrap_mdl, mdl));
		spin_lock_bh(&dispatcher_lock);
		RemoveEntryList(&wrap_mdl->list);
		spin_unlock_bh(&dispatcher_lock);

		if (mdl->flags & MDL_CACHE_ALLOCATED) {
			TRACE3("freeing mdl cache: %p, %p, %p",
			       wrap_mdl, mdl, mdl->mappedsystemva);
			kmem_cache_free(mdl_cache, wrap_mdl);
		} else {
			TRACE3("freeing mdl: %p, %p, %p",
			       wrap_mdl, mdl, mdl->mappedsystemva);
			kfree(wrap_mdl);
		}
	}
	return;
}

wstdcall void WIN_FUNC(IoBuildPartialMdl,4)
	(struct mdl *source, struct mdl *target, void *virt, ULONG length)
{
	MmInitializeMdl(target, virt, length);
	target->flags |= MDL_PARTIAL;
}

wstdcall void WIN_FUNC(MmBuildMdlForNonPagedPool,1)
	(struct mdl *mdl)
{
	PFN_NUMBER *mdl_pages;
	int i, n;

	ENTER4("%p", mdl);
	/* already mapped */
//	mdl->mappedsystemva = MmGetMdlVirtualAddress(mdl);
	mdl->flags |= MDL_SOURCE_IS_NONPAGED_POOL;
	TRACE4("%p, %p, %p, %d, %d", mdl, mdl->mappedsystemva, mdl->startva,
	       mdl->byteoffset, mdl->bytecount);
	n = SPAN_PAGES(MmGetSystemAddressForMdl(mdl), MmGetMdlByteCount(mdl));
	if (n > MDL_CACHE_PAGES)
		WARNING("%p, %d, %d", MmGetSystemAddressForMdl(mdl),
			MmGetMdlByteCount(mdl), n);
	mdl_pages = MmGetMdlPfnArray(mdl);
	for (i = 0; i < n; i++)
		mdl_pages[i] = (ULONG_PTR)mdl->startva + (i * PAGE_SIZE);
	EXIT4(return);
}

wstdcall void *WIN_FUNC(MmMapLockedPages,2)
	(struct mdl *mdl, KPROCESSOR_MODE access_mode)
{
	/* already mapped */
//	mdl->mappedsystemva = MmGetMdlVirtualAddress(mdl);
	mdl->flags |= MDL_MAPPED_TO_SYSTEM_VA;
	/* what is the need for MDL_PARTIAL_HAS_BEEN_MAPPED? */
	if (mdl->flags & MDL_PARTIAL)
		mdl->flags |= MDL_PARTIAL_HAS_BEEN_MAPPED;
	return mdl->mappedsystemva;
}

wstdcall void *WIN_FUNC(MmMapLockedPagesSpecifyCache,6)
	(struct mdl *mdl, KPROCESSOR_MODE access_mode,
	 enum memory_caching_type cache_type, void *base_address,
	 ULONG bug_check, enum mm_page_priority priority)
{
	return MmMapLockedPages(mdl, access_mode);
}

wstdcall void WIN_FUNC(MmUnmapLockedPages,2)
	(void *base, struct mdl *mdl)
{
	mdl->flags &= ~MDL_MAPPED_TO_SYSTEM_VA;
	return;
}

wstdcall void WIN_FUNC(MmProbeAndLockPages,3)
	(struct mdl *mdl, KPROCESSOR_MODE access_mode,
	 enum lock_operation operation)
{
	/* already locked */
	mdl->flags |= MDL_PAGES_LOCKED;
	return;
}

wstdcall void WIN_FUNC(MmUnlockPages,1)
	(struct mdl *mdl)
{
	mdl->flags &= ~MDL_PAGES_LOCKED;
	return;
}

wstdcall BOOLEAN WIN_FUNC(MmIsAddressValid,1)
	(void *virt_addr)
{
	if (virt_addr_valid(virt_addr))
		return TRUE;
	else
		return FALSE;
}

wstdcall void *WIN_FUNC(MmLockPagableDataSection,1)
	(void *address)
{
	return address;
}

wstdcall void WIN_FUNC(MmUnlockPagableImageSection,1)
	(void *handle)
{
	return;
}

wstdcall NTSTATUS WIN_FUNC(ObReferenceObjectByHandle,6)
	(void *handle, ACCESS_MASK desired_access, void *obj_type,
	 KPROCESSOR_MODE access_mode, void **object, void *handle_info)
{
	struct common_object_header *hdr;

	TRACE2("%p", handle);
	hdr = HANDLE_TO_HEADER(handle);
	atomic_inc_var(hdr->ref_count);
	*object = HEADER_TO_OBJECT(hdr);
	TRACE2("%p, %p, %d, %p", hdr, object, hdr->ref_count, *object);
	return STATUS_SUCCESS;
}

/* DDK doesn't say if return value should be before incrementing or
 * after incrementing reference count, but according to #reactos
 * developers, it should be return value after incrementing */
wfastcall LONG WIN_FUNC(ObfReferenceObject,1)
	(void *object)
{
	struct common_object_header *hdr;
	LONG ret;

	hdr = OBJECT_TO_HEADER(object);
	ret = post_atomic_add(hdr->ref_count, 1);
	TRACE2("%p, %d, %p", hdr, hdr->ref_count, object);
	return ret;
}

static int dereference_object(void *object)
{
	struct common_object_header *hdr;
	int ref_count;

	ENTER2("object: %p", object);
	hdr = OBJECT_TO_HEADER(object);
	TRACE2("hdr: %p", hdr);
	ref_count = post_atomic_add(hdr->ref_count, -1);
	TRACE2("object: %p, %d", object, ref_count);
	if (ref_count < 0)
		ERROR("invalid object: %p (%d)", object, ref_count);
	if (ref_count <= 0) {
		free_object(object);
		return 1;
	} else
		return 0;
}

wfastcall void WIN_FUNC(ObfDereferenceObject,1)
	(void *object)
{
	TRACE2("%p", object);
	dereference_object(object);
}

wstdcall NTSTATUS WIN_FUNC(ZwCreateFile,11)
	(void **handle, ACCESS_MASK access_mask,
	 struct object_attributes *obj_attr, struct io_status_block *iosb,
	 LARGE_INTEGER *size, ULONG file_attr, ULONG share_access,
	 ULONG create_disposition, ULONG create_options, void *ea_buffer,
	 ULONG ea_length)
{
	struct common_object_header *coh;
	struct file_object *fo;
	struct ansi_string ansi;
	struct wrap_bin_file *bin_file;
	char *file_basename;
	NTSTATUS status;

	spin_lock_bh(&ntoskernel_lock);
	nt_list_for_each_entry(coh, &object_list, list) {
		if (coh->type != OBJECT_TYPE_FILE)
			continue;
		/* TODO: check if file is opened in shared mode */
		if (!RtlCompareUnicodeString(&coh->name, obj_attr->name, TRUE)) {
			fo = HEADER_TO_OBJECT(coh);
			bin_file = fo->wrap_bin_file;
			*handle = coh;
			spin_unlock_bh(&ntoskernel_lock);
			ObReferenceObject(fo);
			iosb->status = FILE_OPENED;
			iosb->info = bin_file->size;
			EXIT2(return STATUS_SUCCESS);
		}
	}
	spin_unlock_bh(&ntoskernel_lock);

	if (RtlUnicodeStringToAnsiString(&ansi, obj_attr->name, TRUE) !=
	    STATUS_SUCCESS)
		EXIT2(return STATUS_INSUFFICIENT_RESOURCES);

	file_basename = strrchr(ansi.buf, '\\');
	if (file_basename)
		file_basename++;
	else
		file_basename = ansi.buf;
	TRACE2("file: '%s', '%s'", ansi.buf, file_basename);

	fo = allocate_object(sizeof(struct file_object), OBJECT_TYPE_FILE,
			     obj_attr->name);
	if (!fo) {
		RtlFreeAnsiString(&ansi);
		iosb->status = STATUS_INSUFFICIENT_RESOURCES;
		iosb->info = 0;
		EXIT2(return STATUS_FAILURE);
	}
	coh = OBJECT_TO_HEADER(fo);
	bin_file = get_bin_file(file_basename);
	if (bin_file) {
		TRACE2("%s, %s", bin_file->name, file_basename);
		fo->flags = FILE_OPENED;
	} else if (access_mask & FILE_WRITE_DATA) {
		bin_file = kzalloc(sizeof(*bin_file), GFP_KERNEL);
		if (bin_file) {
			strncpy(bin_file->name, file_basename,
				sizeof(bin_file->name));
			bin_file->name[sizeof(bin_file->name)-1] = 0;
			bin_file->data = vmalloc(*size);
			if (bin_file->data) {
				memset(bin_file->data, 0, *size);
				bin_file->size = *size;
				fo->flags = FILE_CREATED;
			} else {
				kfree(bin_file);
				bin_file = NULL;
			}
		}
	} else
		bin_file = NULL;

	RtlFreeAnsiString(&ansi);
	if (!bin_file) {
		iosb->status = FILE_DOES_NOT_EXIST;
		iosb->info = 0;
		free_object(fo);
		EXIT2(return STATUS_FAILURE);
	}

	fo->wrap_bin_file = bin_file;
	fo->current_byte_offset = 0;
	if (access_mask & FILE_READ_DATA)
		fo->read_access = TRUE;
	if (access_mask & FILE_WRITE_DATA)
		fo->write_access = TRUE;
	iosb->status = FILE_OPENED;
	iosb->info = bin_file->size;
	*handle = coh;
	TRACE2("handle: %p", *handle);
	status = STATUS_SUCCESS;
	EXIT2(return status);
}

wstdcall NTSTATUS WIN_FUNC(ZwOpenFile,6)
	(void **handle, ACCESS_MASK access_mask,
	 struct object_attributes *obj_attr, struct io_status_block *iosb,
	 ULONG share_access, ULONG open_options)
{
	LARGE_INTEGER size;
	return ZwCreateFile(handle, access_mask, obj_attr, iosb, &size, 0,
			    share_access, 0, open_options, NULL, 0);
}

wstdcall NTSTATUS WIN_FUNC(ZwReadFile,9)
	(void *handle, struct nt_event *event, void *apc_routine,
	 void *apc_context, struct io_status_block *iosb, void *buffer,
	 ULONG length, LARGE_INTEGER *byte_offset, ULONG *key)
{
	struct file_object *fo;
	struct common_object_header *coh;
	ULONG count;
	size_t offset;
	struct wrap_bin_file *file;

	TRACE2("%p", handle);
	coh = handle;
	if (coh->type != OBJECT_TYPE_FILE) {
		ERROR("handle %p is invalid: %d", handle, coh->type);
		EXIT2(return STATUS_FAILURE);
	}
	fo = HANDLE_TO_OBJECT(coh);
	file = fo->wrap_bin_file;
	TRACE2("file: %s (%zu)", file->name, file->size);
	spin_lock_bh(&ntoskernel_lock);
	if (byte_offset)
		offset = *byte_offset;
	else
		offset = fo->current_byte_offset;
	count = min((size_t)length, file->size - offset);
	TRACE2("count: %u, offset: %zu, length: %u", count, offset, length);
	memcpy(buffer, ((void *)file->data) + offset, count);
	fo->current_byte_offset = offset + count;
	spin_unlock_bh(&ntoskernel_lock);
	iosb->status = STATUS_SUCCESS;
	iosb->info = count;
	EXIT2(return STATUS_SUCCESS);
}

wstdcall NTSTATUS WIN_FUNC(ZwWriteFile,9)
	(void *handle, struct nt_event *event, void *apc_routine,
	 void *apc_context, struct io_status_block *iosb, void *buffer,
	 ULONG length, LARGE_INTEGER *byte_offset, ULONG *key)
{
	struct file_object *fo;
	struct common_object_header *coh;
	struct wrap_bin_file *file;
	unsigned long offset;

	TRACE2("%p", handle);
	coh = handle;
	if (coh->type != OBJECT_TYPE_FILE) {
		ERROR("handle %p is invalid: %d", handle, coh->type);
		EXIT2(return STATUS_FAILURE);
	}
	fo = HANDLE_TO_OBJECT(coh);
	file = fo->wrap_bin_file;
	TRACE2("file: %zu, %u", file->size, length);
	spin_lock_bh(&ntoskernel_lock);
	if (byte_offset)
		offset = *byte_offset;
	else
		offset = fo->current_byte_offset;
	if (length + offset > file->size) {
		WARNING("%lu, %zu", length + offset, file->size);
		/* TODO: implement writing past end of current size */
		iosb->status = STATUS_FAILURE;
		iosb->info = 0;
	} else {
		memcpy(file->data + offset, buffer, length);
		iosb->status = STATUS_SUCCESS;
		iosb->info = length;
		fo->current_byte_offset = offset + length;
	}
	spin_unlock_bh(&ntoskernel_lock);
	EXIT2(return iosb->status);
}

wstdcall NTSTATUS WIN_FUNC(ZwClose,1)
	(void *handle)
{
	struct common_object_header *coh;

	TRACE2("%p", handle);
	if (handle == NULL) {
		TRACE1("");
		EXIT2(return STATUS_SUCCESS);
	}
	coh = handle;
	if (coh->type == OBJECT_TYPE_FILE) {
		struct file_object *fo;
		struct wrap_bin_file *bin_file;
		typeof(fo->flags) flags;

		fo = HANDLE_TO_OBJECT(handle);
		flags = fo->flags;
		bin_file = fo->wrap_bin_file;
		if (dereference_object(fo)) {
			if (flags == FILE_CREATED) {
				vfree(bin_file->data);
				kfree(bin_file);
			} else
				free_bin_file(bin_file);
		}
	} else if (coh->type == OBJECT_TYPE_NT_THREAD) {
		struct nt_thread *thread = HANDLE_TO_OBJECT(handle);
		TRACE2("thread: %p (%p)", thread, handle);
		ObDereferenceObject(thread);
	} else {
		/* TODO: can we just dereference object here? */
		WARNING("closing handle 0x%x not implemented", coh->type);
	}
	EXIT2(return STATUS_SUCCESS);
}

wstdcall NTSTATUS WIN_FUNC(ZwQueryInformationFile,5)
	(void *handle, struct io_status_block *iosb, void *info,
	 ULONG length, enum file_info_class class)
{
	struct file_object *fo;
	struct file_name_info *fni;
	struct file_std_info *fsi;
	struct wrap_bin_file *file;
	struct common_object_header *coh;

	ENTER2("%p", handle);
	coh = handle;
	if (coh->type != OBJECT_TYPE_FILE) {
		ERROR("handle %p is invalid: %d", coh, coh->type);
		EXIT2(return STATUS_FAILURE);
	}
	fo = HANDLE_TO_OBJECT(handle);
	TRACE2("fo: %p, %d", fo, class);
	switch (class) {
	case FileNameInformation:
		fni = info;
		fni->length = min(length, (typeof(length))coh->name.length);
		memcpy(fni->name, coh->name.buf, fni->length);
		iosb->status = STATUS_SUCCESS;
		iosb->info = fni->length;
		break;
	case FileStandardInformation:
		fsi = info;
		file = fo->wrap_bin_file;
		fsi->alloc_size = file->size;
		fsi->eof = file->size;
		fsi->num_links = 1;
		fsi->delete_pending = FALSE;
		fsi->dir = FALSE;
		iosb->status = STATUS_SUCCESS;
		iosb->info = 0;
		break;
	default:
		WARNING("type %d not implemented yet", class);
		iosb->status = STATUS_FAILURE;
		iosb->info = 0;
		break;
	}
	EXIT2(return iosb->status);
}

wstdcall NTSTATUS WIN_FUNC(ZwOpenSection,3)
	(void **handle, ACCESS_MASK access, struct object_attributes *obj_attrs)
{
	INFO("%p, 0x%x, %d", obj_attrs, obj_attrs->attributes, access);
	TODO();
	*handle = obj_attrs;
	return STATUS_SUCCESS;
}

wstdcall NTSTATUS WIN_FUNC(ZwMapViewOfSection,10)
	(void *secn_handle, void *process_handle, void **base_address,
	 ULONG zero_bits, SIZE_T commit_size, LARGE_INTEGER *secn_offset,
	 SIZE_T *view_size, enum section_inherit inherit, ULONG alloc_type,
	 ULONG protect)
{
	INFO("%p, %p, %p", secn_handle, process_handle, base_address);
	TODO();
	*base_address = (void *)0xdeadbeef;
	return STATUS_SUCCESS;
}

wstdcall NTSTATUS WIN_FUNC(ZwUnmapViewOfSection,2)
	(void *process_handle, void *base_address)
{
	INFO("%p, %p", process_handle, base_address);
	TODO();
	return STATUS_SUCCESS;
}

wstdcall NTSTATUS WIN_FUNC(ZwCreateKey,7)
	(void **handle, ACCESS_MASK desired_access,
	 struct object_attributes *attr, ULONG title_index,
	 struct unicode_string *class, ULONG create_options,
	 ULONG *disposition)
{
	struct ansi_string ansi;
	if (RtlUnicodeStringToAnsiString(&ansi, attr->name, TRUE) ==
	    STATUS_SUCCESS) {
		TRACE1("key: %s", ansi.buf);
		RtlFreeAnsiString(&ansi);
	}
	*handle = NULL;
	return STATUS_SUCCESS;
}

wstdcall NTSTATUS WIN_FUNC(ZwOpenKey,3)
	(void **handle, ACCESS_MASK desired_access,
	 struct object_attributes *attr)
{
	struct ansi_string ansi;
	if (RtlUnicodeStringToAnsiString(&ansi, attr->name, TRUE) ==
	    STATUS_SUCCESS) {
		TRACE1("key: %s", ansi.buf);
		RtlFreeAnsiString(&ansi);
	}
	*handle = NULL;
	return STATUS_SUCCESS;
}

wstdcall NTSTATUS WIN_FUNC(ZwSetValueKey,6)
	(void *handle, struct unicode_string *name, ULONG title_index,
	 ULONG type, void *data, ULONG data_size)
{
	struct ansi_string ansi;
	if (RtlUnicodeStringToAnsiString(&ansi, name, TRUE) ==
	    STATUS_SUCCESS) {
		TRACE1("key: %s", ansi.buf);
		RtlFreeAnsiString(&ansi);
	}
	return STATUS_SUCCESS;
}

wstdcall NTSTATUS WIN_FUNC(ZwQueryValueKey,6)
	(void *handle, struct unicode_string *name,
	 enum key_value_information_class class, void *info,
	 ULONG length, ULONG *res_length)
{
	struct ansi_string ansi;
	if (RtlUnicodeStringToAnsiString(&ansi, name, TRUE) == STATUS_SUCCESS) {
		TRACE1("key: %s", ansi.buf);
		RtlFreeAnsiString(&ansi);
	}
	TODO();
	return STATUS_INVALID_PARAMETER;
}

wstdcall NTSTATUS WIN_FUNC(ZwDeleteKey,1)
	(void *handle)
{
	ENTER2("%p", handle);
	return STATUS_SUCCESS;
}

wstdcall NTSTATUS WIN_FUNC(ZwPowerInformation,5)
	(INT info_level, void *in_buf, ULONG in_buf_len, void *out_buf,
	 ULONG out_buf_len)
{
	INFO("%d, %u, %u", info_level, in_buf_len, out_buf_len);
	TODO();
	return STATUS_ACCESS_DENIED;
}

wstdcall NTSTATUS WIN_FUNC(WmiSystemControl,4)
	(struct wmilib_context *info, struct device_object *dev_obj,
	 struct irp *irp, void *irp_disposition)
{
	TODO();
	return STATUS_SUCCESS;
}

wstdcall NTSTATUS WIN_FUNC(WmiCompleteRequest,5)
	(struct device_object *dev_obj, struct irp *irp, NTSTATUS status,
	 ULONG buffer_used, CCHAR priority_boost)
{
	TODO();
	return STATUS_SUCCESS;
}

noregparm NTSTATUS WIN_FUNC(WmiTraceMessage,12)
	(void *tracehandle, ULONG message_flags,
	 void *message_guid, USHORT message_no, ...)
{
	TODO();
	EXIT2(return STATUS_SUCCESS);
}

wstdcall NTSTATUS WIN_FUNC(WmiQueryTraceInformation,4)
	(enum trace_information_class trace_info_class, void *trace_info,
	 ULONG *req_length, void *buf)
{
	TODO();
	EXIT2(return STATUS_SUCCESS);
}

/* this function can't be wstdcall as it takes variable number of args */
__attribute__((format(printf, 1, 2)))
noregparm ULONG WIN_FUNC(DbgPrint,12)
	(char *format, ...)
{
#if DEBUG >= 1
	va_list args;
	static char buf[100];

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	printk(KERN_DEBUG "%s (%s): %s", DRIVER_NAME, __func__, buf);
	va_end(args);
#endif
	return STATUS_SUCCESS;
}

__attribute__((format(printf, 3, 4)))
noregparm ULONG WIN_FUNC(DbgPrintEx,12)
	(ULONG component_id, ULONG severity, char *format, ...)
{
#if DEBUG >= 1
	va_list args;
	static char buf[100];

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	TRACE1("component_id: %d, severity: %d\n", component_id, severity);
	printk(KERN_DEBUG "%s (%s): %s", DRIVER_NAME, __func__, buf);
	va_end(args);
#endif
	return STATUS_SUCCESS;
}

wstdcall void WIN_FUNC(KeBugCheck,1)
	(ULONG code)
{
	ERROR("Unrecoverable error reported by the driver");
	ERROR("code: 0x%x\n", code);
	dump_stack();
	return;
}

wstdcall void WIN_FUNC(KeBugCheckEx,5)
	(ULONG code, ULONG_PTR param1, ULONG_PTR param2,
	 ULONG_PTR param3, ULONG_PTR param4)
{
	ERROR("Unrecoverable error reported by the driver");
	ERROR("code: 0x%x, params: 0x%lx 0x%lx 0x%lx 0x%lx\n", code, param1,
	      param2, param3, param4);
	dump_stack();
	return;
}

wstdcall void WIN_FUNC(ExSystemTimeToLocalTime,2)
	(LARGE_INTEGER *system_time, LARGE_INTEGER *local_time)
{
	*local_time = *system_time;
}

wstdcall ULONG WIN_FUNC(ExSetTimerResolution,2)
	(ULONG time, BOOLEAN set)
{
	/* why a driver should change system wide timer resolution is
	 * beyond me */
	return time;
}

wstdcall void WIN_FUNC(DbgBreakPoint,0)
	(void)
{
	TODO();
}

wstdcall void WIN_FUNC(_except_handler3,0)
	(void)
{
	TODO();
}

wstdcall void WIN_FUNC(__C_specific_handler,0)
	(void)
{
	TODO();
}

wstdcall void WIN_FUNC(_purecall,0)
	(void)
{
	TODO();
}

struct worker_init_struct {
	struct work_struct work;
	struct completion completion;
	struct nt_thread *nt_thread;
};

int ntoskernel_init(void)
{
	spin_lock_init(&dispatcher_lock);
	spin_lock_init(&ntoskernel_lock);
	spin_lock_init(&ntos_work_lock);
	spin_lock_init(&kdpc_list_lock);
	spin_lock_init(&irp_cancel_lock);
	InitializeListHead(&wrap_mdl_list);
	InitializeListHead(&kdpc_list);
	InitializeListHead(&callback_objects);
	InitializeListHead(&bus_driver_list);
	InitializeListHead(&object_list);
	InitializeListHead(&ntos_work_list);

	nt_spin_lock_init(&nt_list_lock);

	INIT_WORK(&kdpc_work, kdpc_worker);
	INIT_WORK(&ntos_work, ntos_work_worker);
	wrap_timer_slist.next = NULL;

    wrap_ticks_to_boot = TICKS_1601_TO_1970;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,0,0)
    do {
        u64 now;
        now = ktime_get_real_ns();
        wrap_ticks_to_boot += now * 10;
    } while (0);
#else
    do {
        struct timeval now;

        do_gettimeofday(&now);
        wrap_ticks_to_boot += (u64)now.tv_sec * TICKSPERSEC;
        wrap_ticks_to_boot += now.tv_usec * 10;
    } while (0);
#endif
	TRACE2("%llu", wrap_ticks_to_boot);
	wrap_ticks_to_boot -= jiffies * TICKSPERJIFFY;
	TRACE2("%llu", wrap_ticks_to_boot);

	cpu_count = num_online_cpus();

#ifdef WRAP_PREEMPT
	do {
		int cpu;
		for_each_possible_cpu(cpu) {
			struct irql_info *info;
			info = &per_cpu(irql_info, cpu);
			mutex_init(&(info->lock));
			info->task = NULL;
			info->count = 0;
#ifdef CONFIG_SMP
			cpumask_setall(tsk_cpus_allowed(info));
#endif
		}
	} while (0);
#endif

	ntos_wq = create_singlethread_workqueue("ntos_wq");
	if (!ntos_wq) {
		WARNING("couldn't create ntos_wq thread");
		return -ENOMEM;
	}
	TRACE1("ntos_wq: %p", ntos_wq);

	if (add_bus_driver("PCI")
#ifdef ENABLE_USB
	    || add_bus_driver("USB")
#endif
		) {
		ntoskernel_exit();
		return -ENOMEM;
	}
	mdl_cache =
		wrap_kmem_cache_create(DRIVER_NAME "_mdl",
				       sizeof(struct wrap_mdl) + MDL_CACHE_SIZE,
				       0, 0);
	TRACE2("%p", mdl_cache);
	if (!mdl_cache) {
		ERROR("couldn't allocate MDL cache");
		ntoskernel_exit();
		return -ENOMEM;
	}

#if defined(CONFIG_X86_64)
	memset(&kuser_shared_data, 0, sizeof(kuser_shared_data));
	*((ULONG64 *)&kuser_shared_data.system_time) = ticks_1601();
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	init_timer(&shared_data_timer);
	shared_data_timer.function = update_user_shared_data_proc;
	shared_data_timer.data = 0;
#else
	timer_setup(&shared_data_timer, update_user_shared_data_proc, 0);
#endif
#endif
	return 0;
}

int ntoskernel_init_device(struct wrap_device *wd)
{
#if defined(CONFIG_X86_64)
	if (kuser_shared_data.reserved1)
		mod_timer(&shared_data_timer, jiffies + MSEC_TO_HZ(30));
#endif
	return 0;
}

void ntoskernel_exit_device(struct wrap_device *wd)
{
	ENTER2("");

	KeFlushQueuedDpcs();
	EXIT2(return);
}

void ntoskernel_exit(void)
{
	struct nt_list *cur;

	ENTER2("");

	/* free kernel (Ke) timers */
	TRACE2("freeing timers");
	while (1) {
		struct wrap_timer *wrap_timer;
		struct nt_slist *slist;

		spin_lock_bh(&ntoskernel_lock);
		if ((slist = wrap_timer_slist.next))
			wrap_timer_slist.next = slist->next;
		spin_unlock_bh(&ntoskernel_lock);
		TIMERTRACE("%p", slist);
		if (!slist)
			break;
		wrap_timer = container_of(slist, struct wrap_timer, slist);
		if (del_timer_sync(&wrap_timer->timer))
			WARNING("Buggy Windows driver left timer %p running",
				wrap_timer->nt_timer);
		memset(wrap_timer, 0, sizeof(*wrap_timer));
		slack_kfree(wrap_timer);
	}

	TRACE2("freeing MDLs");
	if (mdl_cache) {
		spin_lock_bh(&ntoskernel_lock);
		if (!IsListEmpty(&wrap_mdl_list))
			ERROR("Windows driver didn't free all MDLs; "
			      "freeing them now");
		while ((cur = RemoveHeadList(&wrap_mdl_list))) {
			struct wrap_mdl *wrap_mdl;
			wrap_mdl = container_of(cur, struct wrap_mdl, list);
			if (wrap_mdl->mdl->flags & MDL_CACHE_ALLOCATED)
				kmem_cache_free(mdl_cache, wrap_mdl);
			else
				kfree(wrap_mdl);
		}
		spin_unlock_bh(&ntoskernel_lock);
		kmem_cache_destroy(mdl_cache);
		mdl_cache = NULL;
	}

	TRACE2("freeing callbacks");
	spin_lock_bh(&ntoskernel_lock);
	while ((cur = RemoveHeadList(&callback_objects))) {
		struct callback_object *object;
		struct nt_list *ent;
		object = container_of(cur, struct callback_object, list);
		while ((ent = RemoveHeadList(&object->callback_funcs))) {
			struct callback_func *f;
			f = container_of(ent, struct callback_func, list);
			kfree(f);
		}
		kfree(object);
	}
	spin_unlock_bh(&ntoskernel_lock);

	spin_lock_bh(&ntoskernel_lock);
	while ((cur = RemoveHeadList(&bus_driver_list))) {
		struct bus_driver *bus_driver;
		bus_driver = container_of(cur, struct bus_driver, list);
		/* TODO: make sure all all drivers are shutdown/removed */
		kfree(bus_driver);
	}
	spin_unlock_bh(&ntoskernel_lock);

#if defined(CONFIG_X86_64)
	del_timer_sync(&shared_data_timer);
#endif
	if (ntos_wq)
		destroy_workqueue(ntos_wq);
	ENTER2("freeing objects");
	spin_lock_bh(&ntoskernel_lock);
	while ((cur = RemoveHeadList(&object_list))) {
		struct common_object_header *hdr;
		hdr = container_of(cur, struct common_object_header, list);
		if (hdr->type == OBJECT_TYPE_NT_THREAD)
			TRACE1("object %p(%d) was not freed, freeing it now",
			       HEADER_TO_OBJECT(hdr), hdr->type);
		else
			WARNING("object %p(%d) was not freed, freeing it now",
				HEADER_TO_OBJECT(hdr), hdr->type);
		ExFreePool(hdr);
	}
	spin_unlock_bh(&ntoskernel_lock);

	EXIT2(return);
}
