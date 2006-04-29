/*
 *  Copyright (C) 2006 Giridhar Pemmasani
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

/* workqueue implementation for 2.4 kernels */

static int workqueue_thread(void *data)
{
	struct workqueue_struct *wq = data;
	struct work_struct *ws;

	while (1) {
		if (wait_event_interruptible(wq->wq_head,
					     atomic_read(&wq->pending) != 0))
			break;
		if (atomic_read(&wq->pending) < 0)
			break;
		atomic_dec(&wq->pending);
		spin_lock_bh(&wq->lock);
		if (list_empty(&wq->work_list))
			ws = NULL;
		else {
			struct list_head *entry;
			entry = wq->work_list.next;
			ws = list_entry(entry, struct work_struct, list);
			list_del(entry);
			ws->scheduled = 0;
		}
		spin_unlock_bh(&wq->lock);
		if (!ws)
			continue;
		ws->func(ws->data);
	}
	/* wakeup destroy_workqueue */
	atomic_set(&wq->pending, 1);
	wake_up_interruptible(&wq->wq_head);
	return 0;
}

struct workqueue_struct *create_singlethread_workqueue(const char *name)
{
	struct workqueue_struct *wq = kmalloc(sizeof(*wq), GFP_KERNEL);
	if (!wq) {
		WARNING("couldn't allocate memory");
		return NULL;
	}
	memset(wq, 0, sizeof(*wq));
	init_waitqueue_head(&wq->wq_head);
	spin_lock_init(&wq->lock);
	wq->name = name;
	INIT_LIST_HEAD(&wq->work_list);
	wq->pid = kernel_thread(workqueue_thread, wq, 0);
	/* TODO: how to set the name of task (thread)? */
	if (wq->pid < 0) {
		kfree(wq);
		WARNING("couldn't start thread %s", name);
		return NULL;
	}
	return wq;
}

void destroy_workqueue(struct workqueue_struct *wq)
{
	atomic_set(&wq->pending, -1);
	wake_up_interruptible(&wq->wq_head);
	/* wait for thread to finish before freeing memory */
	wait_event_interruptible(wq->wq_head, atomic_read(&wq->pending) > 0);
	kfree(wq);
}

void queue_work(struct workqueue_struct *wq, struct work_struct *work_struct)
{
	int prev;
	spin_lock_bh(&wq->lock);
	if (!(prev = work_struct->scheduled++))
		list_add_tail(&work_struct->list, &wq->work_list);
	spin_unlock_bh(&wq->lock);
	if (!prev) {
		atomic_inc(&wq->pending);
		wake_up_interruptible(&wq->wq_head);
	}
}