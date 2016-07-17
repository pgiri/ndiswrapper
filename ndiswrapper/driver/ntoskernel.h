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

#ifndef _NTOSKERNEL_H_
#define _NTOSKERNEL_H_

#include <linux/types.h>
#include <linux/timer.h>
#include <linux/time.h>
#include <linux/module.h>
#include <linux/kmod.h>

#include <linux/netdevice.h>
#include <linux/wireless.h>
#include <linux/pci.h>
#include <linux/wait.h>
#include <linux/pm.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/random.h>
#include <linux/ctype.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/usb.h>
#include <linux/spinlock.h>
#include <asm/mman.h>
#include <linux/version.h>
#include <linux/etherdevice.h>
#include <net/iw_handler.h>
#include <linux/ethtool.h>
#include <linux/if_arp.h>
#include <linux/rtnetlink.h>
#include <linux/highmem.h>
#include <linux/percpu.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include <linux/vmalloc.h>


#if !defined(CONFIG_X86) && !defined(CONFIG_X86_64)
#error "this module is for x86 or x86_64 architectures only"
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,14)
#define gfp_t unsigned int __nocast

static inline void *_kzalloc(size_t size, gfp_t flags)
{
	void *p = kmalloc(size, flags);
	if (likely(p != NULL))
		memset(p, 0, size);
	return p;
}

#define kzalloc(size, flags) _kzalloc(size, flags)
#endif

/* Interrupt backwards compatibility stuff */
#include <linux/interrupt.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,29)
#ifndef IRQ_HANDLED
#define IRQ_HANDLED
#define IRQ_NONE
#define irqreturn_t void
#endif
#endif /* Linux < 2.6.29 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16)
#ifndef mutex_init
#define mutex semaphore
#define mutex_init(m) sema_init(m, 1)
#define mutex_lock(m) down(m)
#define mutex_trylock(m) (!down_trylock(m))
#define mutex_unlock(m) up(m)
#define mutex_is_locked(m) (atomic_read(m.count) == 0)
#endif
#endif /* Linux < 2.6.16 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,26)
#define set_cpus_allowed_ptr(task, mask) set_cpus_allowed(task, *mask)
#endif /* Linux < 2.6.26 */

#ifdef CONFIG_SMP
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28)
#define cpumask_copy(dst, src) do { *dst = *src; } while (0)
#define cpumask_equal(mask1, mask2) cpus_equal(*mask1, *mask2)
#define cpumask_setall(mask) cpus_setall(*mask)
static cpumask_t cpumasks[NR_CPUS];
#define cpumask_of(cpu) 			\
({						\
	cpumasks[cpu] = cpumask_of_cpu(cpu);	\
	&cpumasks[cpu];				\
})
#endif /* Linux < 2.6.28 */
#endif /* CONFIG_SMP */

#ifndef tsk_cpus_allowed
#define tsk_cpus_allowed(tsk) (&(tsk)->cpus_allowed)
#endif

#ifndef __packed
#define __packed __attribute__((packed))
#endif

/* pci functions in 2.6 kernels have problems allocating dma buffers,
 * but seem to work fine with dma functions
 */
#include <asm/dma-mapping.h>

#define PCI_DMA_ALLOC_COHERENT(pci_dev,size,dma_handle)			\
	dma_alloc_coherent(&pci_dev->dev,size,dma_handle,		\
			   GFP_KERNEL | __GFP_REPEAT)
#define PCI_DMA_FREE_COHERENT(pci_dev,size,cpu_addr,dma_handle)		\
	dma_free_coherent(&pci_dev->dev,size,cpu_addr,dma_handle)
#define PCI_DMA_MAP_SINGLE(pci_dev,addr,size,direction)		\
	dma_map_single(&pci_dev->dev,addr,size,direction)
#define PCI_DMA_UNMAP_SINGLE(pci_dev,dma_handle,size,direction)		\
	dma_unmap_single(&pci_dev->dev,dma_handle,size,direction)
#define MAP_SG(pci_dev, sglist, nents, direction)		\
	dma_map_sg(&pci_dev->dev, sglist, nents, direction)
#define UNMAP_SG(pci_dev, sglist, nents, direction)		\
	dma_unmap_sg(&pci_dev->dev, sglist, nents, direction)
#define PCI_DMA_MAP_ERROR(dma_addr) dma_mapping_error(dma_addr)


#if defined(CONFIG_NET_RADIO) && !defined(CONFIG_WIRELESS_EXT)
#define CONFIG_WIRELESS_EXT
#endif

#define prepare_wait_condition(task, var, value)	\
do {							\
	var = value;					\
	task = current;					\
	barrier();					\
} while (0)

/* Wait in wait_state (e.g., TASK_INTERRUPTIBLE) for condition to
 * become true; timeout is either jiffies (> 0) to wait or 0 to wait
 * forever.
 * When timeout == 0, return value is
 *    > 0 if condition becomes true, or
 *    < 0 if signal is pending on the thread.
 * When timeout > 0, return value is
 *    > 0 if condition becomes true before timeout,
 *    < 0 if signal is pending on the thread before timeout, or
 *    0 if timedout (condition may have become true at the same time)
 */

#define wait_condition(condition, timeout, wait_state)		\
({								\
	long ret = timeout ? timeout : 1;			\
	while (1) {						\
		if (signal_pending(current)) {			\
			ret = -ERESTARTSYS;			\
			break;					\
		}						\
		set_current_state(wait_state);			\
		if (condition) {				\
			__set_current_state(TASK_RUNNING);	\
			break;					\
		}						\
		if (timeout) {					\
			ret = schedule_timeout(ret);		\
			if (!ret)				\
				break;				\
		} else						\
			schedule();				\
	}							\
	ret;							\
})

#ifdef WRAP_WQ

struct wrap_workqueue_struct;

struct wrap_work_struct {
	struct list_head list;
	void (*func)(struct wrap_work_struct *data);
	void *data;
	/* whether/on which thread scheduled */
	struct workqueue_thread *thread;
};

#define work_struct wrap_work_struct
#define workqueue_struct wrap_workqueue_struct

#undef INIT_WORK
#define INIT_WORK(work, pfunc)					\
	do {							\
		(work)->func = (pfunc);				\
		(work)->data = (work);				\
		(work)->thread = NULL;				\
	} while (0)

#undef create_singlethread_workqueue
#define create_singlethread_workqueue(wq) wrap_create_wq(wq, 1, 0)
#undef create_workqueue
#define create_workqueue(wq) wrap_create_wq(wq, 0, 0)
#undef destroy_workqueue
#define destroy_workqueue(wq) wrap_destroy_wq(wq)
#undef queue_work
#define queue_work(wq, work) wrap_queue_work(wq, work)
#undef flush_workqueue
#define flush_workqueue(wq) wrap_flush_wq(wq)

struct workqueue_struct *wrap_create_wq(const char *name, u8 singlethread,
					u8 freeze);
void wrap_destroy_wq(struct workqueue_struct *workq);
int wrap_queue_work(struct workqueue_struct *workq, struct work_struct *work);
void wrap_cancel_work(struct work_struct *work);
void wrap_flush_wq(struct workqueue_struct *workq);

#else // WRAP_WQ

/* Compatibility for Linux before 2.6.20 where INIT_WORK takes 3 arguments */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20) && \
    !defined(INIT_WORK_NAR) && \
    !defined(INIT_DELAYED_WORK_DEFERRABLE)
typedef void (*compat_work_func_t)(void *work);
typedef void (*work_func_t)(struct work_struct *work);
static inline void (INIT_WORK)(struct work_struct *work, work_func_t func)
{
	INIT_WORK(work, (compat_work_func_t)func, work);
}
#undef INIT_WORK
#endif

#endif // WRAP_WQ

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,18)
#define ISR_PT_REGS_PARAM_DECL
#else
#define ISR_PT_REGS_PARAM_DECL , struct pt_regs *regs
#endif

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,16)
#define for_each_possible_cpu(_cpu) for_each_cpu(_cpu)
#endif

#ifndef CHECKSUM_PARTIAL
#define CHECKSUM_PARTIAL CHECKSUM_HW
#endif

#ifndef IRQF_SHARED
#define IRQF_SHARED SA_SHIRQ
#endif

#ifndef UMH_WAIT_PROC
#define UMH_WAIT_PROC 1
#endif

#define memcpy_skb(skb, from, length)			\
	memcpy(skb_put(skb, length), from, length)

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
#ifndef DMA_BIT_MASK
#define DMA_BIT_MASK(n)	(((n) == 64) ? ~0ULL : ((1ULL<<(n))-1))
#endif
#endif

#ifndef __GFP_DMA32
#define __GFP_DMA32 GFP_DMA
#endif

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,22)
#define wrap_kmem_cache_create(name, size, align, flags)	\
	kmem_cache_create(name, size, align, flags, NULL, NULL)
#else
#define wrap_kmem_cache_create(name, size, align, flags)	\
	kmem_cache_create(name, size, align, flags, NULL)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,34)
#define netdev_mc_count(dev) ((dev)->mc_count)
#define usb_alloc_coherent(dev, size, mem_flags, dma) (usb_buffer_alloc((dev), (size), (mem_flags), (dma)))
#define usb_free_coherent(dev, size, addr, dma) (usb_buffer_free((dev), (size), (addr), (dma)))
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,8,0)
#define daemonize(name, ...) do {} while (0)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,9,0)
#define add_taint(flag, lockdep_ok) add_taint(flag)
#endif

#include "winnt_types.h"
#include "ndiswrapper.h"
#include "pe_linker.h"
#include "wrapmem.h"
#include "lin2win.h"
#include "loader.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18)
static inline void netif_tx_lock(struct net_device *dev)
{
	spin_lock(&dev->xmit_lock);
}
static inline void netif_tx_unlock(struct net_device *dev)
{
	spin_unlock(&dev->xmit_lock);
}
static inline void netif_tx_lock_bh(struct net_device *dev)
{
	spin_lock_bh(&dev->xmit_lock);
}
static inline void netif_tx_unlock_bh(struct net_device *dev)
{
	spin_unlock_bh(&dev->xmit_lock);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
static inline void netif_poll_enable(struct net_device *dev)
{
}
static inline void netif_poll_disable(struct net_device *dev)
{
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
#define proc_net_root init_net.proc_net
#else
#define proc_net_root proc_net
#endif

#if ((LINUX_VERSION_CODE < KERNEL_VERSION(3,2,0)) && \
     (LINUX_VERSION_CODE >= KERNEL_VERSION(3,0,0))) || \
    (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,42))
#ifndef skb_frag_page
#define skb_frag_page(frag) ((frag)->page)
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,11,0)
#define netdev_notifier_info_to_dev(x) ((struct net_device *)(x))
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0)
static inline void reinit_completion(struct completion *x)
{
	INIT_COMPLETION(*x);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,8,0)
#define prandom_seed(seed) net_srandom(seed)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,22)
static int strncasecmp(const char *s1, const char *s2, size_t n)
{
	strnicmp(s1, s2, n);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,7,0)
static inline void netif_trans_update(struct net_device *dev)
{
	dev->trans_start = jiffies;
}
#endif

/* TICK is 100ns */
#define TICKSPERSEC		10000000
#define TICKSPERMSEC		10000
#define SECSPERDAY		86400
#define TICKSPERJIFFY		((TICKSPERSEC + HZ - 1) / HZ)

#define int_div_round(x, y) (((x) + (y - 1)) / (y))

/* 1601 to 1970 is 369 years plus 89 leap days */
#define SECS_1601_TO_1970	((369 * 365 + 89) * (u64)SECSPERDAY)
#define TICKS_1601_TO_1970	(SECS_1601_TO_1970 * TICKSPERSEC)

/* 100ns units to HZ; if sys_time is negative, relative to current
 * clock, otherwise from year 1601 */
#define SYSTEM_TIME_TO_HZ(sys_time)					\
	(((sys_time) <= 0) ? \
	 int_div_round(((u64)HZ * (-(sys_time))), TICKSPERSEC) :	\
	 int_div_round(((s64)HZ * ((sys_time) - ticks_1601())), TICKSPERSEC))

#define MSEC_TO_HZ(ms) int_div_round((ms * HZ), 1000)
#define USEC_TO_HZ(us) int_div_round((us * HZ), 1000000)

extern u64 wrap_ticks_to_boot;

static inline u64 ticks_1601(void)
{
	return wrap_ticks_to_boot + (u64)jiffies * TICKSPERJIFFY;
}

typedef void (*generic_func)(void);

struct wrap_export {
	const char *name;
	generic_func func;
};

#ifdef CONFIG_X86_64

#define WIN_SYMBOL(name, argc)					\
	{#name, (generic_func) win2lin_ ## name ## _ ## argc}
#define WIN_WIN_SYMBOL(name, argc)					\
	{#name, (generic_func) win2lin__win_ ## name ## _ ## argc}
#define WIN_FUNC_DECL(name, argc)			\
	extern typeof(name) win2lin_ ## name ## _ ## argc;
#define WIN_FUNC_PTR(name, argc) win2lin_ ## name ## _ ## argc

#else

#define WIN_SYMBOL(name, argc) {#name, (generic_func)name}
#define WIN_WIN_SYMBOL(name, argc) {#name, (generic_func)_win_ ## name}
#define WIN_FUNC_DECL(name, argc)
#define WIN_FUNC_PTR(name, argc) name

#endif

#define WIN_FUNC(name, argc) (name)
/* map name s to f - if f is different from s */
#define WIN_SYMBOL_MAP(s, f)

#define POOL_TAG(A, B, C, D)					\
	((ULONG)((A) + ((B) << 8) + ((C) << 16) + ((D) << 24)))

struct pe_image {
	char name[MAX_DRIVER_NAME_LEN];
	UINT (*entry)(struct driver_object *, struct unicode_string *) wstdcall;
	void *image;
	int size;
	int type;

	IMAGE_NT_HEADERS *nt_hdr;
	IMAGE_OPTIONAL_HEADER *opt_hdr;
};

struct ndis_mp_block;

struct wrap_timer {
	struct nt_slist slist;
	struct timer_list timer;
	struct nt_timer *nt_timer;
	long repeat;
#ifdef TIMER_DEBUG
	unsigned long wrap_timer_magic;
#endif
};

struct ntos_work_item {
	struct nt_list list;
	void *arg1;
	void *arg2;
	NTOS_WORK_FUNC func;
};

struct wrap_device_setting {
	struct nt_list list;
	char name[MAX_SETTING_NAME_LEN];
	char value[MAX_SETTING_VALUE_LEN];
	void *encoded;
};

struct wrap_bin_file {
	char name[MAX_DRIVER_NAME_LEN];
	size_t size;
	void *data;
};

#define WRAP_DRIVER_CLIENT_ID 1

struct wrap_driver {
	struct nt_list list;
	struct driver_object *drv_obj;
	char name[MAX_DRIVER_NAME_LEN];
	char version[MAX_SETTING_VALUE_LEN];
	unsigned short num_pe_images;
	struct pe_image pe_images[MAX_DRIVER_PE_IMAGES];
	unsigned short num_bin_files;
	struct wrap_bin_file *bin_files;
	struct nt_list settings;
	int dev_type;
	struct ndis_driver *ndis_driver;
};

enum hw_status {
	HW_INITIALIZED = 1, HW_SUSPENDED, HW_HALTED, HW_DISABLED,
};

struct wrap_device {
	/* first part is (de)initialized once by loader */
	struct nt_list list;
	int dev_bus;
	int vendor;
	int device;
	int subvendor;
	int subdevice;
	char conf_file_name[MAX_DRIVER_NAME_LEN];
	char driver_name[MAX_DRIVER_NAME_LEN];
	struct wrap_driver *driver;
	struct nt_list settings;

	/* rest should be (de)initialized when a device is
	 * (un)plugged */
	struct cm_resource_list *resource_list;
	unsigned long hw_status;
	struct device_object *pdo;
	union {
		struct {
			struct pci_dev *pdev;
			enum device_power_state wake_state;
		} pci;
		struct {
			struct usb_device *udev;
			struct usb_interface *intf;
			int num_alloc_urbs;
			struct nt_list wrap_urb_list;
		} usb;
	};
};

#define wrap_is_pci_bus(dev_bus)			\
	(WRAP_BUS(dev_bus) == WRAP_PCI_BUS ||		\
	 WRAP_BUS(dev_bus) == WRAP_PCMCIA_BUS)
#ifdef ENABLE_USB
/* earlier versions of ndiswrapper used 0 as USB_BUS */
#define wrap_is_usb_bus(dev_bus)			\
	(WRAP_BUS(dev_bus) == WRAP_USB_BUS ||		\
	 WRAP_BUS(dev_bus) == WRAP_INTERNAL_BUS)
#else
#define wrap_is_usb_bus(dev_bus) 0
#endif
#define wrap_is_bluetooth_device(dev_bus)			\
	(WRAP_DEVICE(dev_bus) == WRAP_BLUETOOTH_DEVICE1 ||	\
	 WRAP_DEVICE(dev_bus) == WRAP_BLUETOOTH_DEVICE2)

extern struct workqueue_struct *ntos_wq;
extern struct workqueue_struct *ndis_wq;
extern struct workqueue_struct *wrapndis_wq;

#define atomic_unary_op(var, size, oper)				\
do {									\
	if (size == 1)							\
		__asm__ __volatile__(					\
			LOCK_PREFIX oper "b %b0\n\t" : "+m" (var));	\
	else if (size == 2)						\
		__asm__ __volatile__(					\
			LOCK_PREFIX oper "w %w0\n\t" : "+m" (var));	\
	else if (size == 4)						\
		__asm__ __volatile__(					\
			LOCK_PREFIX oper "l %0\n\t" : "+m" (var));	\
	else if (size == 8)						\
		__asm__ __volatile__(					\
			LOCK_PREFIX oper "q %q0\n\t" : "+m" (var));	\
	else {								\
		extern void _invalid_op_size_(void);			\
		_invalid_op_size_();					\
	}								\
} while (0)

#define atomic_inc_var_size(var, size) atomic_unary_op(var, size, "inc")

#define atomic_inc_var(var) atomic_inc_var_size(var, sizeof(var))

#define atomic_dec_var_size(var, size) atomic_unary_op(var, size, "dec")

#define atomic_dec_var(var) atomic_dec_var_size(var, sizeof(var))

#define pre_atomic_add(var, i)					\
({								\
	typeof(var) pre;					\
	__asm__ __volatile__(					\
		LOCK_PREFIX "xadd %0, %1\n\t"			\
		: "=r"(pre), "+m"(var)				\
		: "0"(i));					\
	pre;							\
})

#define post_atomic_add(var, i) (pre_atomic_add(var, i) + i)

//#define DEBUG_IRQL 1

#ifdef DEBUG_IRQL
#define assert_irql(cond)						\
do {									\
	KIRQL _irql_ = current_irql();					\
	if (!(cond)) {							\
		WARNING("assertion '%s' failed: %d", #cond, _irql_);	\
		DBG_BLOCK(4) {						\
			dump_stack();					\
		}							\
	}								\
} while (0)
#else
#define assert_irql(cond) do { } while (0)
#endif

/* When preempt is enabled, we should preempt_disable to raise IRQL to
 * DISPATCH_LEVEL, to be consistent with the semantics. However, using
 * a mutex instead, so that only ndiswrapper threads run one at a time
 * on a processor when at DISPATCH_LEVEL seems to be enough. So that
 * is what we will use until we learn otherwise. If
 * preempt_(en|dis)able is required for some reason, comment out
 * following #define. */

#define WRAP_PREEMPT 1

#if !defined(CONFIG_PREEMPT) || defined(CONFIG_PREEMPT_RT)
#ifndef WRAP_PREEMPT
#define WRAP_PREEMPT 1
#endif
#endif

//#undef WRAP_PREEMPT

#ifdef WRAP_PREEMPT

struct irql_info {
	int count;
	struct mutex lock;
#ifdef CONFIG_SMP
	cpumask_t cpus_allowed;
#endif
	struct task_struct *task;
};

DECLARE_PER_CPU(struct irql_info, irql_info);

static inline KIRQL raise_irql(KIRQL newirql)
{
	struct irql_info *info;

	assert(newirql == DISPATCH_LEVEL);
	info = &get_cpu_var(irql_info);
	if (info->task == current) {
		assert(info->count > 0);
		assert(mutex_is_locked(&info->lock));
#if defined(CONFIG_SMP) && DEBUG >= 1
		assert(cpumask_equal(tsk_cpus_allowed(current),
				     cpumask_of(smp_processor_id())));
#endif
		info->count++;
		put_cpu_var(irql_info);
		return DISPATCH_LEVEL;
	}
	/* TODO: is this enough to pin down to current cpu? */
#ifdef CONFIG_SMP
	assert(task_cpu(current) == smp_processor_id());
	cpumask_copy(&info->cpus_allowed, tsk_cpus_allowed(current));
	set_cpus_allowed_ptr(current, cpumask_of(smp_processor_id()));
#endif
	put_cpu_var(irql_info);
	mutex_lock(&info->lock);
	assert(info->count == 0);
	assert(info->task == NULL);
	info->count = 1;
	info->task = current;
	return PASSIVE_LEVEL;
}

static inline void lower_irql(KIRQL oldirql)
{
	struct irql_info *info;

	assert(oldirql <= DISPATCH_LEVEL);
	info = &get_cpu_var(irql_info);
	assert(info->task == current);
	assert(mutex_is_locked(&info->lock));
	assert(info->count > 0);
	if (--info->count == 0) {
		info->task = NULL;
#ifdef CONFIG_SMP
		set_cpus_allowed_ptr(current, &info->cpus_allowed);
#endif
		mutex_unlock(&info->lock);
	}
	put_cpu_var(irql_info);
}

static inline KIRQL current_irql(void)
{
	int count;
	if (in_irq() || irqs_disabled())
		EXIT4(return DIRQL);
	if (in_atomic() || in_interrupt())
		EXIT4(return SOFT_IRQL);
	count = get_cpu_var(irql_info).count;
	put_cpu_var(irql_info);
	if (count)
		EXIT6(return DISPATCH_LEVEL);
	else
		EXIT6(return PASSIVE_LEVEL);
}

#else

static inline KIRQL current_irql(void)
{
	if (in_irq() || irqs_disabled())
		EXIT4(return DIRQL);
	if (in_interrupt())
		EXIT4(return SOFT_IRQL);
	if (in_atomic())
		EXIT6(return DISPATCH_LEVEL);
	else
		EXIT6(return PASSIVE_LEVEL);
}

static inline KIRQL raise_irql(KIRQL newirql)
{
	KIRQL ret = in_atomic() ? DISPATCH_LEVEL : PASSIVE_LEVEL;
	assert(newirql == DISPATCH_LEVEL);
	assert(current_irql() <= DISPATCH_LEVEL);
	preempt_disable();
	return ret;
}

static inline void lower_irql(KIRQL oldirql)
{
	assert(current_irql() == DISPATCH_LEVEL);
	preempt_enable();
}

#endif

#define irql_gfp() (in_atomic() ? GFP_ATOMIC : GFP_KERNEL)

/* Windows spinlocks are of type ULONG_PTR which is not big enough to
 * store Linux spinlocks; so we implement Windows spinlocks using
 * ULONG_PTR space with our own functions/macros */

/* Windows seems to use 0 for unlocked state of spinlock - if Linux
 * convention of 1 for unlocked state is used, at least prism54 driver
 * crashes */

#define NT_SPIN_LOCK_UNLOCKED 0
#define NT_SPIN_LOCK_LOCKED 1

static inline void nt_spin_lock_init(NT_SPIN_LOCK *lock)
{
	*lock = NT_SPIN_LOCK_UNLOCKED;
}

#ifdef CONFIG_SMP

static inline void nt_spin_lock(NT_SPIN_LOCK *lock)
{
	while (1) {
		unsigned long lockval = xchg(lock, NT_SPIN_LOCK_LOCKED);

		if (likely(lockval == NT_SPIN_LOCK_UNLOCKED))
			break;
		if (unlikely(lockval > NT_SPIN_LOCK_LOCKED)) {
			ERROR("bad spinlock: 0x%lx at %p", lockval, lock);
			return;
		}
		/* "rep; nop" doesn't change cx register, it's a "pause" */
		__asm__ __volatile__("rep; nop");
	}
}

static inline void nt_spin_unlock(NT_SPIN_LOCK *lock)
{
	unsigned long lockval = xchg(lock, NT_SPIN_LOCK_UNLOCKED);

	if (likely(lockval == NT_SPIN_LOCK_LOCKED))
		return;
	WARNING("unlocking unlocked spinlock: 0x%lx at %p", lockval, lock);
}

#else // CONFIG_SMP

#define nt_spin_lock(lock) do { } while (0)

#define nt_spin_unlock(lock) do { } while (0)

#endif // CONFIG_SMP

/* When kernel would've disabled preempt (e.g., in interrupt
 * handlers), we need to fake preempt so driver thinks it is running
 * at right IRQL */

/* raise IRQL to given (higher) IRQL if necessary before locking */
static inline KIRQL nt_spin_lock_irql(NT_SPIN_LOCK *lock, KIRQL newirql)
{
	KIRQL oldirql = raise_irql(newirql);
	nt_spin_lock(lock);
	return oldirql;
}

/* lower IRQL to given (lower) IRQL if necessary after unlocking */
static inline void nt_spin_unlock_irql(NT_SPIN_LOCK *lock, KIRQL oldirql)
{
	nt_spin_unlock(lock);
	lower_irql(oldirql);
}

#define nt_spin_lock_irqsave(lock, flags)				\
do {									\
	local_irq_save(flags);						\
	preempt_disable();						\
	nt_spin_lock(lock);						\
} while (0)

#define nt_spin_unlock_irqrestore(lock, flags)				\
do {									\
	nt_spin_unlock(lock);						\
	preempt_enable();						\
	local_irq_restore(flags);					\
} while (0)

static inline ULONG SPAN_PAGES(void *ptr, SIZE_T length)
{
	return PAGE_ALIGN(((unsigned long)ptr & (PAGE_SIZE - 1)) + length)
		>> PAGE_SHIFT;
}

#ifdef CONFIG_X86_64

/* TODO: can these be implemented without using spinlock? */

static inline struct nt_slist *PushEntrySList(nt_slist_header *head,
					      struct nt_slist *entry,
					      NT_SPIN_LOCK *lock)
{
	KIRQL irql = nt_spin_lock_irql(lock, DISPATCH_LEVEL);
	entry->next = head->next;
	head->next = entry;
	head->depth++;
	nt_spin_unlock_irql(lock, irql);
	TRACE4("%p, %p, %p", head, entry, entry->next);
	return entry->next;
}

static inline struct nt_slist *PopEntrySList(nt_slist_header *head,
					     NT_SPIN_LOCK *lock)
{
	struct nt_slist *entry;
	KIRQL irql = nt_spin_lock_irql(lock, DISPATCH_LEVEL);
	entry = head->next;
	if (entry) {
		head->next = entry->next;
		head->depth--;
	}
	nt_spin_unlock_irql(lock, irql);
	TRACE4("%p, %p", head, entry);
	return entry;
}

#else

#define u64_low_32(x) ((u32)x)
#define u64_high_32(x) ((u32)(x >> 32))

static inline u64 nt_cmpxchg8b(volatile u64 *ptr, u64 old, u64 new)
{
	u64 prev;

	__asm__ __volatile__(
		"\n"
		LOCK_PREFIX "cmpxchg8b %0\n"
		: "+m" (*ptr), "=A" (prev)
		: "A" (old), "b" (u64_low_32(new)), "c" (u64_high_32(new)));
	return prev;
}

/* slist routines below update slist atomically - no need for
 * spinlocks */

static inline struct nt_slist *PushEntrySList(nt_slist_header *head,
					      struct nt_slist *entry,
					      NT_SPIN_LOCK *lock)
{
	nt_slist_header old, new;
	do {
		old.align = head->align;
		entry->next = old.next;
		new.next = entry;
		new.depth = old.depth + 1;
	} while (nt_cmpxchg8b(&head->align, old.align, new.align) != old.align);
	TRACE4("%p, %p, %p", head, entry, old.next);
	return old.next;
}

static inline struct nt_slist *PopEntrySList(nt_slist_header *head,
					     NT_SPIN_LOCK *lock)
{
	struct nt_slist *entry;
	nt_slist_header old, new;
	do {
		old.align = head->align;
		entry = old.next;
		if (!entry)
			break;
		new.next = entry->next;
		new.depth = old.depth - 1;
	} while (nt_cmpxchg8b(&head->align, old.align, new.align) != old.align);
	TRACE4("%p, %p", head, entry);
	return entry;
}

#endif

#define sleep_hz(n)					\
do {							\
	set_current_state(TASK_INTERRUPTIBLE);		\
	schedule_timeout(n);				\
} while (0)

int ntoskernel_init(void);
void ntoskernel_exit(void);
int ntoskernel_init_device(struct wrap_device *wd);
void ntoskernel_exit_device(struct wrap_device *wd);
void *allocate_object(ULONG size, enum common_object_type type,
		      struct unicode_string *name);

#ifdef ENABLE_USB
int usb_init(void);
void usb_exit(void);
#else
static inline int usb_init(void) { return 0; }
static inline void usb_exit(void) {}
#endif
int usb_init_device(struct wrap_device *wd);
void usb_exit_device(struct wrap_device *wd);

int wrap_procfs_init(void);
void wrap_procfs_remove(void);

int link_pe_images(struct pe_image *pe_image, unsigned short n);

int stricmp(const char *s1, const char *s2);
void dump_bytes(const char *name, const u8 *from, int len);
struct mdl *allocate_init_mdl(void *virt, ULONG length);
void free_mdl(struct mdl *mdl);
struct driver_object *find_bus_driver(const char *name);
void free_custom_extensions(struct driver_extension *drv_obj_ext);
struct nt_thread *get_current_nt_thread(void);
u64 ticks_1601(void);
int schedule_ntos_work_item(NTOS_WORK_FUNC func, void *arg1, void *arg2);
void wrap_init_timer(struct nt_timer *nt_timer, enum timer_type type,
		     struct ndis_mp_block *nmb);
BOOLEAN wrap_set_timer(struct nt_timer *nt_timer, unsigned long expires_hz,
		       unsigned long repeat_hz, struct kdpc *kdpc);

LONG InterlockedDecrement(LONG volatile *val) wfastcall;
LONG InterlockedIncrement(LONG volatile *val) wfastcall;
struct nt_list *ExInterlockedInsertHeadList
	(struct nt_list *head, struct nt_list *entry,
	 NT_SPIN_LOCK *lock) wfastcall;
struct nt_list *ExInterlockedInsertTailList
	(struct nt_list *head, struct nt_list *entry,
	 NT_SPIN_LOCK *lock) wfastcall;
struct nt_list *ExInterlockedRemoveHeadList
	(struct nt_list *head, NT_SPIN_LOCK *lock) wfastcall;
NTSTATUS IofCallDriver(struct device_object *dev_obj, struct irp *irp) wfastcall;
KIRQL KfRaiseIrql(KIRQL newirql) wfastcall;
void KfLowerIrql(KIRQL oldirql) wfastcall;
KIRQL KfAcquireSpinLock(NT_SPIN_LOCK *lock) wfastcall;
void KfReleaseSpinLock(NT_SPIN_LOCK *lock, KIRQL oldirql) wfastcall;
void IofCompleteRequest(struct irp *irp, CHAR prio_boost) wfastcall;
void KefReleaseSpinLockFromDpcLevel(NT_SPIN_LOCK *lock) wfastcall;

LONG ObfReferenceObject(void *object) wfastcall;
void ObfDereferenceObject(void *object) wfastcall;

#define ObReferenceObject(object) ObfReferenceObject(object)
#define ObDereferenceObject(object) ObfDereferenceObject(object)

/* prevent expansion of ExAllocatePoolWithTag macro */
void *(ExAllocatePoolWithTag)(enum pool_type pool_type, SIZE_T size,
			      ULONG tag) wstdcall;

void ExFreePool(void *p) wstdcall;
ULONG MmSizeOfMdl(void *base, ULONG length) wstdcall;
void __iomem *MmMapIoSpace(PHYSICAL_ADDRESS phys_addr, SIZE_T size,
		   enum memory_caching_type cache) wstdcall;
void MmUnmapIoSpace(void __iomem *addr, SIZE_T size) wstdcall;
void MmProbeAndLockPages(struct mdl *mdl, KPROCESSOR_MODE access_mode,
			 enum lock_operation operation) wstdcall;
void MmUnlockPages(struct mdl *mdl) wstdcall;
void KeInitializeEvent(struct nt_event *nt_event,
		       enum event_type type, BOOLEAN state) wstdcall;
LONG KeSetEvent(struct nt_event *nt_event, KPRIORITY incr,
		BOOLEAN wait) wstdcall;
LONG KeResetEvent(struct nt_event *nt_event) wstdcall;
BOOLEAN queue_kdpc(struct kdpc *kdpc);
BOOLEAN dequeue_kdpc(struct kdpc *kdpc);

NTSTATUS IoConnectInterrupt(struct kinterrupt **kinterrupt,
			    PKSERVICE_ROUTINE service_routine,
			    void *service_context, NT_SPIN_LOCK *lock,
			    ULONG vector, KIRQL irql, KIRQL synch_irql,
			    enum kinterrupt_mode interrupt_mode,
			    BOOLEAN shareable, KAFFINITY processor_enable_mask,
			    BOOLEAN floating_save) wstdcall;
void IoDisconnectInterrupt(struct kinterrupt *interrupt) wstdcall;
BOOLEAN KeSynchronizeExecution(struct kinterrupt *interrupt,
			       PKSYNCHRONIZE_ROUTINE synch_routine,
			       void *ctx) wstdcall;

NTSTATUS KeWaitForSingleObject(void *object, KWAIT_REASON reason,
			       KPROCESSOR_MODE waitmode, BOOLEAN alertable,
			       LARGE_INTEGER *timeout) wstdcall;
void MmBuildMdlForNonPagedPool(struct mdl *mdl) wstdcall;
NTSTATUS IoCreateDevice(struct driver_object *driver, ULONG dev_ext_length,
			struct unicode_string *dev_name, DEVICE_TYPE dev_type,
			ULONG dev_chars, BOOLEAN exclusive,
			struct device_object **dev_obj) wstdcall;
NTSTATUS IoCreateSymbolicLink(struct unicode_string *link,
			      struct unicode_string *dev_name) wstdcall;
void IoDeleteDevice(struct device_object *dev) wstdcall;
void IoDetachDevice(struct device_object *topdev) wstdcall;
struct device_object *IoGetAttachedDevice(struct device_object *dev) wstdcall;
struct device_object *IoGetAttachedDeviceReference
	(struct device_object *dev) wstdcall;
NTSTATUS IoAllocateDriverObjectExtension
	(struct driver_object *drv_obj, void *client_id, ULONG extlen,
	 void **ext) wstdcall;
void *IoGetDriverObjectExtension(struct driver_object *drv,
				 void *client_id) wstdcall;
struct device_object *IoAttachDeviceToDeviceStack
	(struct device_object *src, struct device_object *dst) wstdcall;
BOOLEAN IoCancelIrp(struct irp *irp) wstdcall;
struct irp *IoBuildSynchronousFsdRequest
	(ULONG major_func, struct device_object *dev_obj, void *buf,
	 ULONG length, LARGE_INTEGER *offset, struct nt_event *event,
	 struct io_status_block *status) wstdcall;

NTSTATUS IoPassIrpDown(struct device_object *dev_obj, struct irp *irp) wstdcall;
WIN_FUNC_DECL(IoPassIrpDown,2);
NTSTATUS IoSyncForwardIrp(struct device_object *dev_obj,
			  struct irp *irp) wstdcall;
NTSTATUS IoAsyncForwardIrp(struct device_object *dev_obj,
			   struct irp *irp) wstdcall;
NTSTATUS IoInvalidDeviceRequest(struct device_object *dev_obj,
				struct irp *irp) wstdcall;

void KeInitializeSpinLock(NT_SPIN_LOCK *lock) wstdcall;
void IoAcquireCancelSpinLock(KIRQL *irql) wstdcall;
void IoReleaseCancelSpinLock(KIRQL irql) wstdcall;

NTSTATUS RtlUnicodeStringToAnsiString
	(struct ansi_string *dst, const struct unicode_string *src,
	 BOOLEAN dup) wstdcall;
NTSTATUS RtlAnsiStringToUnicodeString
	(struct unicode_string *dst, const struct ansi_string *src,
	 BOOLEAN dup) wstdcall;
void RtlInitAnsiString(struct ansi_string *dst, const char *src) wstdcall;
void RtlInitUnicodeString(struct unicode_string *dest,
			  const wchar_t *src) wstdcall;
void RtlFreeUnicodeString(struct unicode_string *string) wstdcall;
void RtlFreeAnsiString(struct ansi_string *string) wstdcall;
LONG RtlCompareUnicodeString(const struct unicode_string *s1,
			     const struct unicode_string *s2,
			     BOOLEAN case_insensitive) wstdcall;
NTSTATUS RtlUpcaseUnicodeString(struct unicode_string *dst,
				struct unicode_string *src,
				BOOLEAN alloc) wstdcall;
BOOLEAN KeCancelTimer(struct nt_timer *nt_timer) wstdcall;
void KeInitializeDpc(struct kdpc *kdpc, void *func, void *ctx) wstdcall;

extern spinlock_t ntoskernel_lock;
extern spinlock_t irp_cancel_lock;
extern struct nt_list object_list;
extern CCHAR cpu_count;
#ifdef CONFIG_X86_64
extern struct kuser_shared_data kuser_shared_data;
#endif

#define IoCompleteRequest(irp, prio) IofCompleteRequest(irp, prio)
#define IoCallDriver(dev, irp) IofCallDriver(dev, irp)

#if defined(IO_DEBUG)
#define DUMP_IRP(_irp)							\
do {									\
	struct io_stack_location *_irp_sl;				\
	_irp_sl = IoGetCurrentIrpStackLocation(_irp);			\
	IOTRACE("irp: %p, stack size: %d, cl: %d, sl: %p, dev_obj: %p, " \
		"mj_fn: %d, minor_fn: %d, nt_urb: %p, event: %p",	\
		_irp, _irp->stack_count, (_irp)->current_location,	\
		_irp_sl, _irp_sl->dev_obj, _irp_sl->major_fn,		\
		_irp_sl->minor_fn, IRP_URB(_irp),			\
		(_irp)->user_event);					\
} while (0)
#else
#define DUMP_IRP(_irp) do { } while (0)
#endif

#endif // _NTOSKERNEL_H_
