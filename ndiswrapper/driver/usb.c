/*
 *  Copyright (C) 2004 Jan Kiszka
 *  Copyright (C) 2005 Giridhar Pemmasani
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

#include "ndis.h"
#include "usb.h"
#include "usb_exports.h"

#ifdef USB_DEBUG
static unsigned int urb_id = 0;

#define DUMP_WRAP_URB(wrap_urb, dir)					\
	USBTRACE("urb %p (%d) %s: buf: %p, len: %d, pipe: 0x%x, %d",	\
		 (wrap_urb)->urb, (wrap_urb)->id,			\
		 (dir == USB_DIR_OUT) ? "going down" : "coming back",	\
		 (wrap_urb)->urb->transfer_buffer,			\
		 (wrap_urb)->urb->transfer_buffer_length,		\
		 (wrap_urb)->urb->pipe, (wrap_urb)->urb->status)

#define DUMP_URB_BUFFER(urb, dir)					\
	while (debug >= 2) {						\
		int i;							\
		char msg[20], *t;					\
		if (!urb->transfer_buffer)				\
			break;						\
		if (!((usb_pipein(urb->pipe) && dir == USB_DIR_IN) ||	\
		      (usb_pipeout(urb->pipe) && dir == USB_DIR_OUT)))	\
			break;						\
		t = msg;						\
		t += sprintf(t, "%d: ", (urb)->actual_length);		\
		for (i = 0; i < urb->actual_length &&			\
			     t < &msg[sizeof(msg) - 4]; i++)		\
			t += sprintf(t, "%02X ",			\
				     ((char *)urb->transfer_buffer)[i]); \
		*t = 0;							\
		USBTRACE("%s", msg);					\
		break;							\
	}

#else

#define DUMP_WRAP_URB(wrap_urb, dir) (void)0
#define DUMP_URB_BUFFER(urb, dir) (void)0

#endif

#define CUR_ALT_SETTING(intf) (intf)->cur_altsetting

#ifndef USB_CTRL_SET_TIMEOUT
#define USB_CTRL_SET_TIMEOUT 5000
#endif

#ifndef USB_CTRL_GET_TIMEOUT
#define USB_CTRL_GET_TIMEOUT 5000
#endif

#ifndef URB_NO_TRANSFER_DMA_MAP
#define URB_NO_TRANSFER_DMA_MAP 0
#endif

/* wrap_urb->flags */
/* transfer_buffer for urb is allocated; free it in wrap_free_urb */
#define WRAP_URB_COPY_BUFFER 0x01

static inline int wrap_cancel_urb(struct wrap_urb *wrap_urb)
{
	int ret;
	USBTRACE("%p, %p, %d", wrap_urb, wrap_urb->urb, wrap_urb->state);
	if (wrap_urb->state != URB_SUBMITTED)
		USBEXIT(return -1);
	ret = usb_unlink_urb(wrap_urb->urb);
	USBTRACE("ret: %d", ret);
	if (ret == -EINPROGRESS)
		return 0;
	else {
		WARNING("unlink failed: %d", ret);
		return ret;
	}
}

#define URB_STATUS(wrap_urb) (wrap_urb->urb->status)

static struct nt_list wrap_urb_complete_list;
static spinlock_t wrap_urb_complete_list_lock;

static struct work_struct wrap_urb_complete_work;
static void wrap_urb_complete_worker(struct work_struct *dummy);

static void kill_all_urbs(struct wrap_device *wd, int complete)
{
	struct nt_list *ent;
	struct wrap_urb *wrap_urb;
	KIRQL irql;

	USBTRACE("%d", wd->usb.num_alloc_urbs);
	while (1) {
		IoAcquireCancelSpinLock(&irql);
		ent = RemoveHeadList(&wd->usb.wrap_urb_list);
		IoReleaseCancelSpinLock(irql);
		if (!ent)
			break;
		wrap_urb = container_of(ent, struct wrap_urb, list);
		if (wrap_urb->state == URB_SUBMITTED) {
			WARNING("Windows driver %s didn't free urb: %p",
				wd->driver->name, wrap_urb->urb);
			if (!complete)
				wrap_urb->urb->complete = NULL;
			usb_kill_urb(wrap_urb->urb);
		}
		USBTRACE("%p, %p", wrap_urb, wrap_urb->urb);
		usb_free_urb(wrap_urb->urb);
		kfree(wrap_urb);
	}
	wd->usb.num_alloc_urbs = 0;
}

/* for a given Linux urb status code, return corresponding NT urb status */
static USBD_STATUS wrap_urb_status(int urb_status)
{
	switch (urb_status) {
	case 0:
		return USBD_STATUS_SUCCESS;
	case -EPROTO:
		return USBD_STATUS_TIMEOUT;
	case -EILSEQ:
		return USBD_STATUS_CRC;
	case -EPIPE:
		return USBD_STATUS_INVALID_PIPE_HANDLE;
	case -ECOMM:
		return USBD_STATUS_DATA_OVERRUN;
	case -ENOSR:
		return USBD_STATUS_DATA_UNDERRUN;
	case -EOVERFLOW:
		return USBD_STATUS_BABBLE_DETECTED;
	case -EREMOTEIO:
		return USBD_STATUS_ERROR_SHORT_TRANSFER;;
	case -ENODEV:
	case -ESHUTDOWN:
	case -ENOENT:
		return USBD_STATUS_DEVICE_GONE;
	case -ENOMEM:
		return USBD_STATUS_NO_MEMORY;
	case -EINVAL:
		return USBD_STATUS_REQUEST_FAILED;
	default:
		return USBD_STATUS_NOT_SUPPORTED;
	}
}

/* for a given USBD_STATUS, return its corresponding NTSTATUS (for irp) */
static NTSTATUS nt_urb_irp_status(USBD_STATUS nt_urb_status)
{
	switch (nt_urb_status) {
	case USBD_STATUS_SUCCESS:
		return STATUS_SUCCESS;
	case USBD_STATUS_DEVICE_GONE:
		return STATUS_DEVICE_REMOVED;
	case USBD_STATUS_PENDING:
		return STATUS_PENDING;
	case USBD_STATUS_NOT_SUPPORTED:
		return STATUS_NOT_IMPLEMENTED;
	case USBD_STATUS_NO_MEMORY:
		return STATUS_NO_MEMORY;
	case USBD_STATUS_REQUEST_FAILED:
		return STATUS_NOT_SUPPORTED;
	default:
		return STATUS_FAILURE;
	}
}

static void wrap_free_urb(struct urb *urb)
{
	struct wrap_urb *wrap_urb = urb->context;
	struct irp *irp = wrap_urb->irp;
	struct wrap_device *wd = IRP_WRAP_DEVICE(irp);

	USBTRACE("freeing urb: %p", urb);
	irp->cancel_routine = NULL;
	IRP_WRAP_URB(irp) = NULL;
	if (wrap_urb->flags & WRAP_URB_COPY_BUFFER) {
		USBTRACE("freeing DMA buffer for URB: %p %p",
			 urb, urb->transfer_buffer);
		usb_free_coherent(wd->usb.udev, urb->transfer_buffer_length,
				  urb->transfer_buffer, urb->transfer_dma);
	}
	kfree(urb->setup_packet);
	if (wd->usb.num_alloc_urbs > MAX_ALLOCATED_URBS) {
		IoAcquireCancelSpinLock(&irp->cancel_irql);
		RemoveEntryList(&wrap_urb->list);
		wd->usb.num_alloc_urbs--;
		IoReleaseCancelSpinLock(irp->cancel_irql);
		usb_free_urb(urb);
		kfree(wrap_urb);
	} else {
		wrap_urb->state = URB_FREE;
		wrap_urb->flags = 0;
		wrap_urb->irp = NULL;
	}
	return;
}

void wrap_suspend_urbs(struct wrap_device *wd)
{
	/* TODO: do we need to cancel urbs? */
	USBTRACE("%p, %d", wd, wd->usb.num_alloc_urbs);
}

void wrap_resume_urbs(struct wrap_device *wd)
{
	/* TODO: do we need to resubmit urbs? */
	USBTRACE("%p, %d", wd, wd->usb.num_alloc_urbs);
}

wstdcall void wrap_cancel_irp(struct device_object *dev_obj, struct irp *irp)
{
	struct urb *urb;
	struct wrap_urb *wrap_urb = IRP_WRAP_URB(irp);

	/* NB: this function is called holding Cancel spinlock */
	USBENTER("irp: %p", irp);
	urb = wrap_urb->urb;
	USBTRACE("canceling urb %p", urb);
	if (wrap_cancel_urb(IRP_WRAP_URB(irp))) {
		irp->cancel = FALSE;
		ERROR("urb %p can't be canceled: %d", urb, wrap_urb->state);
	} else
		USBTRACE("urb %p canceled", urb);
	IoReleaseCancelSpinLock(irp->cancel_irql);
	return;
}
WIN_FUNC_DECL(wrap_cancel_irp,2)

static struct urb *wrap_alloc_urb(struct irp *irp, unsigned int pipe,
				  void *buf, unsigned int buf_len)
{
	struct urb *urb;
	gfp_t alloc_flags;
	struct wrap_urb *wrap_urb;
	struct wrap_device *wd;

	USBENTER("irp: %p", irp);
	wd = IRP_WRAP_DEVICE(irp);

	/* Don't interfere with URB cleanup by the kernel */
	if (test_bit(HW_DISABLED, &wd->hw_status))
		return NULL;

	alloc_flags = irql_gfp();
	IoAcquireCancelSpinLock(&irp->cancel_irql);
	urb = NULL;
	nt_list_for_each_entry(wrap_urb, &wd->usb.wrap_urb_list, list) {
		if (cmpxchg(&wrap_urb->state, URB_FREE,
			    URB_ALLOCATED) == URB_FREE) {
			urb = wrap_urb->urb;
			/* Clean URB but keep the refcount */
			memset((char *)urb + sizeof(urb->kref), 0,
			       sizeof(*urb) - sizeof(urb->kref));
			break;
		}
	}
	if (!urb) {
		IoReleaseCancelSpinLock(irp->cancel_irql);
		wrap_urb = kzalloc(sizeof(*wrap_urb), alloc_flags);
		if (!wrap_urb) {
			WARNING("couldn't allocate memory");
			return NULL;
		}
		urb = usb_alloc_urb(0, alloc_flags);
		if (!urb) {
			WARNING("couldn't allocate urb");
			kfree(wrap_urb);
			return NULL;
		}
		IoAcquireCancelSpinLock(&irp->cancel_irql);
		wrap_urb->urb = urb;
		wrap_urb->state = URB_ALLOCATED;
		InsertTailList(&wd->usb.wrap_urb_list, &wrap_urb->list);
		wd->usb.num_alloc_urbs++;
	}

#ifdef URB_ASYNC_UNLINK
	urb->transfer_flags |= URB_ASYNC_UNLINK;
#elif defined(USB_ASYNC_UNLINK)
	urb->transfer_flags |= USB_ASYNC_UNLINK;
#endif
	urb->context = wrap_urb;
	wrap_urb->irp = irp;
	IRP_WRAP_URB(irp) = wrap_urb;
	/* called as Windows function */
	irp->cancel_routine = WIN_FUNC_PTR(wrap_cancel_irp,2);
	IoReleaseCancelSpinLock(irp->cancel_irql);
	USBTRACE("urb: %p", urb);

	urb->transfer_buffer_length = buf_len;
	if (buf_len && buf && (!virt_addr_valid(buf)
#if defined(CONFIG_HIGHMEM) || defined(CONFIG_HIGHMEM4G)
			       || PageHighMem(virt_to_page(buf))
#endif
		    )) {
		urb->transfer_buffer =
			usb_alloc_coherent(wd->usb.udev, buf_len, alloc_flags,
					 &urb->transfer_dma);
		if (!urb->transfer_buffer) {
			WARNING("couldn't allocate dma buf");
			IoAcquireCancelSpinLock(&irp->cancel_irql);
			irp->cancel_routine = NULL;
			wrap_urb->state = URB_FREE;
			wrap_urb->irp = NULL;
			IRP_WRAP_URB(irp) = NULL;
			IoReleaseCancelSpinLock(irp->cancel_irql);
			return NULL;
		}
		if (urb->transfer_dma)
			urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		wrap_urb->flags |= WRAP_URB_COPY_BUFFER;
		if (usb_pipeout(pipe))
			memcpy(urb->transfer_buffer, buf, buf_len);
		USBTRACE("DMA buf for urb %p: %p", urb, urb->transfer_buffer);
	} else
		urb->transfer_buffer = buf;
	return urb;
}

static USBD_STATUS wrap_submit_urb(struct irp *irp)
{
	int ret;
	struct wrap_urb *wrap_urb = IRP_WRAP_URB(irp);
	struct urb *urb = wrap_urb->urb;
	union nt_urb *nt_urb = IRP_URB(irp);

#ifdef USB_DEBUG
	if (wrap_urb->state != URB_ALLOCATED) {
		ERROR("urb %p is in wrong state: %d",
		      urb, wrap_urb->state);
		NT_URB_STATUS(nt_urb) = USBD_STATUS_REQUEST_FAILED;
		return NT_URB_STATUS(nt_urb);
	}
	wrap_urb->id = pre_atomic_add(urb_id, 1);
#endif
	DUMP_WRAP_URB(IRP_WRAP_URB(irp), USB_DIR_OUT);
	irp->io_status.status = STATUS_PENDING;
	irp->io_status.info = 0;
	NT_URB_STATUS(nt_urb) = USBD_STATUS_PENDING;
	IoMarkIrpPending(irp);
	DUMP_URB_BUFFER(urb, USB_DIR_OUT);
	USBTRACE("%p", urb);
	wrap_urb->state = URB_SUBMITTED;
	ret = usb_submit_urb(urb, irql_gfp());
	if (ret) {
		USBTRACE("ret: %d", ret);
		wrap_free_urb(urb);
		/* we assume that IRP was not in pending state before */
		IoUnmarkIrpPending(irp);
		NT_URB_STATUS(nt_urb) = wrap_urb_status(ret);
		USBEXIT(return NT_URB_STATUS(nt_urb));
	} else
		USBEXIT(return USBD_STATUS_PENDING);
}

static void wrap_urb_complete(struct urb *urb ISR_PT_REGS_PARAM_DECL)
{
	struct irp *irp;
	struct wrap_urb *wrap_urb;

	wrap_urb = urb->context;
	USBTRACE("%p (%p) completed", wrap_urb, urb);
	irp = wrap_urb->irp;
	DUMP_WRAP_URB(wrap_urb, USB_DIR_IN);
	irp->cancel_routine = NULL;
#ifdef USB_DEBUG
	if (wrap_urb->state != URB_SUBMITTED) {
		WARNING("urb %p in wrong state: %d (%d)", urb, wrap_urb->state,
			urb->status);
		return;
	}
#endif
	wrap_urb->state = URB_COMPLETED;
	spin_lock(&wrap_urb_complete_list_lock);
	InsertTailList(&wrap_urb_complete_list, &wrap_urb->complete_list);
	spin_unlock(&wrap_urb_complete_list_lock);
	queue_work(ntos_wq, &wrap_urb_complete_work);
}

/* one worker for all devices */
static void wrap_urb_complete_worker(struct work_struct *dummy)
{
	struct irp *irp;
	struct urb *urb;
	struct usbd_bulk_or_intr_transfer *bulk_int_tx;
	struct usbd_vendor_or_class_request *vc_req;
	union nt_urb *nt_urb;
	struct wrap_urb *wrap_urb;
	struct nt_list *ent;
	unsigned long flags;

	USBENTER("");
	while (1) {
		spin_lock_irqsave(&wrap_urb_complete_list_lock, flags);
		ent = RemoveHeadList(&wrap_urb_complete_list);
		spin_unlock_irqrestore(&wrap_urb_complete_list_lock, flags);
		if (!ent)
			break;
		wrap_urb = container_of(ent, struct wrap_urb, complete_list);
		urb = wrap_urb->urb;
#ifdef USB_DEBUG
		if (wrap_urb->state != URB_COMPLETED &&
		    wrap_urb->state != URB_INT_UNLINKED)
			WARNING("urb %p in wrong state: %d",
				urb, wrap_urb->state);
#endif
		irp = wrap_urb->irp;
		DUMP_IRP(irp);
		nt_urb = IRP_URB(irp);
		USBTRACE("urb: %p, nt_urb: %p, status: %d",
			 urb, nt_urb, urb->status);
		switch (urb->status) {
		case 0:
			/* successfully transferred */
			irp->io_status.info = urb->actual_length;
			if (nt_urb->header.function ==
			    URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER) {
				bulk_int_tx = &nt_urb->bulk_int_transfer;
				bulk_int_tx->transfer_buffer_length =
					urb->actual_length;
				DUMP_URB_BUFFER(urb, USB_DIR_IN);
				if ((wrap_urb->flags & WRAP_URB_COPY_BUFFER) &&
				    usb_pipein(urb->pipe))
					memcpy(bulk_int_tx->transfer_buffer,
					       urb->transfer_buffer,
					       urb->actual_length);
			} else { // vendor or class request
				vc_req = &nt_urb->vendor_class_request;
				vc_req->transfer_buffer_length =
					urb->actual_length;
				DUMP_URB_BUFFER(urb, USB_DIR_IN);
				if ((wrap_urb->flags & WRAP_URB_COPY_BUFFER) &&
				    usb_pipein(urb->pipe))
					memcpy(vc_req->transfer_buffer,
					       urb->transfer_buffer,
					       urb->actual_length);
			}
			NT_URB_STATUS(nt_urb) = USBD_STATUS_SUCCESS;
			irp->io_status.status = STATUS_SUCCESS;
			break;
		case -ENOENT:
		case -ECONNRESET:
			/* urb canceled */
			irp->io_status.info = 0;
			TRACE2("urb %p canceled", urb);
			NT_URB_STATUS(nt_urb) = USBD_STATUS_SUCCESS;
			irp->io_status.status = STATUS_CANCELLED;
			break;
		default:
			TRACE2("irp: %p, urb: %p, status: %d/%d",
				 irp, urb, urb->status, wrap_urb->state);
			irp->io_status.info = 0;
			NT_URB_STATUS(nt_urb) = wrap_urb_status(urb->status);
			irp->io_status.status =
				nt_urb_irp_status(NT_URB_STATUS(nt_urb));
			break;
		}
		wrap_free_urb(urb);
		IoCompleteRequest(irp, IO_NO_INCREMENT);
	}
	USBEXIT(return);
}

static USBD_STATUS wrap_bulk_or_intr_trans(struct irp *irp)
{
	struct usb_endpoint_descriptor *pipe_handle;
	struct urb *urb;
	unsigned int pipe;
	struct usbd_bulk_or_intr_transfer *bulk_int_tx;
	USBD_STATUS status;
	struct wrap_device *wd = IRP_WRAP_DEVICE(irp);
	struct usb_device *udev = wd->usb.udev;
	union nt_urb *nt_urb = IRP_URB(irp);

	bulk_int_tx = &nt_urb->bulk_int_transfer;
	pipe_handle = bulk_int_tx->pipe_handle;
	USBTRACE("flags: 0x%x, length: %u, buffer: %p, handle: %p",
		 bulk_int_tx->transfer_flags,
		 bulk_int_tx->transfer_buffer_length,
		 bulk_int_tx->transfer_buffer, pipe_handle);

	if (USBD_IS_BULK_PIPE(pipe_handle)) {
		if (bulk_int_tx->transfer_flags & USBD_TRANSFER_DIRECTION_IN)
			pipe = usb_rcvbulkpipe(udev,
					       pipe_handle->bEndpointAddress);
		else
			pipe = usb_sndbulkpipe(udev,
					       pipe_handle->bEndpointAddress);
	} else {
		if (bulk_int_tx->transfer_flags & USBD_TRANSFER_DIRECTION_IN)
			pipe = usb_rcvintpipe(udev,
					      pipe_handle->bEndpointAddress);
		else
			pipe = usb_sndintpipe(udev,
					      pipe_handle->bEndpointAddress);
	}

	DUMP_IRP(irp);
	urb = wrap_alloc_urb(irp, pipe, bulk_int_tx->transfer_buffer,
			     bulk_int_tx->transfer_buffer_length);
	if (!urb) {
		ERROR("couldn't allocate urb");
		return USBD_STATUS_NO_MEMORY;
	}
	if (usb_pipein(pipe) &&
	    (!(bulk_int_tx->transfer_flags & USBD_SHORT_TRANSFER_OK))) {
		USBTRACE("short not ok");
		urb->transfer_flags |= URB_SHORT_NOT_OK;
	}
	if (usb_pipebulk(pipe)) {
		usb_fill_bulk_urb(urb, udev, pipe, urb->transfer_buffer,
				  bulk_int_tx->transfer_buffer_length,
				  wrap_urb_complete, urb->context);
		USBTRACE("submitting bulk urb %p on pipe 0x%x (ep 0x%x)",
			 urb, urb->pipe, pipe_handle->bEndpointAddress);
	} else {
		usb_fill_int_urb(urb, udev, pipe, urb->transfer_buffer,
				 bulk_int_tx->transfer_buffer_length,
				 wrap_urb_complete, urb->context,
				 pipe_handle->bInterval);
		USBTRACE("submitting interrupt urb %p on pipe 0x%x (ep 0x%x), "
			 "intvl: %d", urb, urb->pipe,
			 pipe_handle->bEndpointAddress, pipe_handle->bInterval);
	}
	status = wrap_submit_urb(irp);
	USBTRACE("status: %08X", status);
	USBEXIT(return status);
}

static USBD_STATUS wrap_vendor_or_class_req(struct irp *irp)
{
	u8 req_type;
	unsigned int pipe;
	struct usbd_vendor_or_class_request *vc_req;
	USBD_STATUS status;
	struct urb *urb;
	struct usb_ctrlrequest *dr;
	struct wrap_device *wd = IRP_WRAP_DEVICE(irp);
	struct usb_device *udev = wd->usb.udev;
	union nt_urb *nt_urb = IRP_URB(irp);

	vc_req = &nt_urb->vendor_class_request;
	USBTRACE("bits: %x, req: %x, val: %08x, index: %08x, flags: %x,"
		 "buf: %p, len: %d", vc_req->reserved_bits, vc_req->request,
		 vc_req->value, vc_req->index, vc_req->transfer_flags,
		 vc_req->transfer_buffer, vc_req->transfer_buffer_length);

	USBTRACE("%x", nt_urb->header.function);
	switch (nt_urb->header.function) {
	case URB_FUNCTION_VENDOR_DEVICE:
		req_type = USB_TYPE_VENDOR | USB_RECIP_DEVICE;
		break;
	case URB_FUNCTION_VENDOR_INTERFACE:
		req_type = USB_TYPE_VENDOR | USB_RECIP_INTERFACE;
		break;
	case URB_FUNCTION_VENDOR_ENDPOINT:
		req_type = USB_TYPE_VENDOR | USB_RECIP_ENDPOINT;
		break;
	case URB_FUNCTION_VENDOR_OTHER:
		req_type = USB_TYPE_VENDOR | USB_RECIP_OTHER;
		break;
	case URB_FUNCTION_CLASS_DEVICE:
		req_type = USB_TYPE_CLASS | USB_RECIP_DEVICE;
		break;
	case URB_FUNCTION_CLASS_INTERFACE:
		req_type = USB_TYPE_CLASS | USB_RECIP_INTERFACE;
		break;
	case URB_FUNCTION_CLASS_ENDPOINT:
		req_type = USB_TYPE_CLASS | USB_RECIP_ENDPOINT;
		break;
	case URB_FUNCTION_CLASS_OTHER:
		req_type = USB_TYPE_CLASS | USB_RECIP_OTHER;
		break;
	default:
		ERROR("unknown request type: %x", nt_urb->header.function);
		req_type = 0;
		break;
	}

	req_type |= vc_req->reserved_bits;
	USBTRACE("req type: %08x", req_type);

	if (vc_req->transfer_flags & USBD_TRANSFER_DIRECTION_IN) {
		pipe = usb_rcvctrlpipe(udev, 0);
		req_type |= USB_DIR_IN;
		USBTRACE("pipe: %x, dir in", pipe);
	} else {
		pipe = usb_sndctrlpipe(udev, 0);
		req_type |= USB_DIR_OUT;
		USBTRACE("pipe: %x, dir out", pipe);
	}
	urb = wrap_alloc_urb(irp, pipe, vc_req->transfer_buffer,
			     vc_req->transfer_buffer_length);
	if (!urb) {
		ERROR("couldn't allocate urb");
		return USBD_STATUS_NO_MEMORY;
	}

	if (usb_pipein(pipe) &&
	    (!(vc_req->transfer_flags & USBD_SHORT_TRANSFER_OK))) {
		USBTRACE("short not ok");
		urb->transfer_flags |= URB_SHORT_NOT_OK;
	}

	dr = kzalloc(sizeof(*dr), irql_gfp());
	if (!dr) {
		ERROR("couldn't allocate memory");
		wrap_free_urb(urb);
		return USBD_STATUS_NO_MEMORY;
	}
	dr->bRequestType = req_type;
	dr->bRequest = vc_req->request;
	dr->wValue = cpu_to_le16(vc_req->value);
	dr->wIndex = cpu_to_le16((u16)vc_req->index);
	dr->wLength = cpu_to_le16((u16)urb->transfer_buffer_length);

	usb_fill_control_urb(urb, udev, pipe, (unsigned char *)dr,
			     urb->transfer_buffer, urb->transfer_buffer_length,
			     wrap_urb_complete, urb->context);
	status = wrap_submit_urb(irp);
	USBTRACE("status: %08X", status);
	USBEXIT(return status);
}

static USBD_STATUS wrap_reset_pipe(struct usb_device *udev, struct irp *irp)
{
	int ret;
	union nt_urb *nt_urb;
	struct usb_endpoint_descriptor *pipe_handle;
	unsigned int pipe1, pipe2;

	nt_urb = IRP_URB(irp);
	pipe_handle = nt_urb->pipe_req.pipe_handle;
	/* TODO: not clear if both directions should be cleared? */
	if (USBD_IS_BULK_PIPE(pipe_handle)) {
		pipe1 = usb_rcvbulkpipe(udev, pipe_handle->bEndpointAddress);
		pipe2 = usb_sndbulkpipe(udev, pipe_handle->bEndpointAddress);
	} else if (USBD_IS_INT_PIPE(pipe_handle)) {
		pipe1 = usb_rcvintpipe(udev, pipe_handle->bEndpointAddress);
		pipe2 = pipe1;
	} else {
		WARNING("invalid pipe %d", pipe_handle->bEndpointAddress);
		return USBD_STATUS_INVALID_PIPE_HANDLE;
	}
	USBTRACE("ep: %d, pipe: 0x%x", pipe_handle->bEndpointAddress, pipe1);
	ret = usb_clear_halt(udev, pipe1);
	if (ret)
		USBTRACE("resetting pipe %d failed: %d", pipe1, ret);
	if (pipe2 != pipe1) {
		ret = usb_clear_halt(udev, pipe2);
		if (ret)
			USBTRACE("resetting pipe %d failed: %d", pipe2, ret);
	}
//	return wrap_urb_status(ret);
	return USBD_STATUS_SUCCESS;
}

static USBD_STATUS wrap_abort_pipe(struct usb_device *udev, struct irp *irp)
{
	union nt_urb *nt_urb;
	struct usb_endpoint_descriptor *pipe_handle;
	struct wrap_urb *wrap_urb;
	struct wrap_device *wd;
	KIRQL irql;

	wd = IRP_WRAP_DEVICE(irp);
	nt_urb = IRP_URB(irp);
	pipe_handle = nt_urb->pipe_req.pipe_handle;
	USBENTER("%p, %x", irp, pipe_handle->bEndpointAddress);
	IoAcquireCancelSpinLock(&irql);
	nt_list_for_each_entry(wrap_urb, &wd->usb.wrap_urb_list, list) {
		USBTRACE("%p, %p, %d, %x, %x", wrap_urb, wrap_urb->urb,
			 wrap_urb->state, wrap_urb->urb->pipe,
			 usb_pipeendpoint(wrap_urb->urb->pipe));
		/* for WG111T driver, urbs for endpoint 0 should also
		 * be canceled */
		if ((usb_pipeendpoint(wrap_urb->urb->pipe) ==
		     pipe_handle->bEndpointAddress) ||
		    (usb_pipeendpoint(wrap_urb->urb->pipe) == 0)) {
			if (wrap_cancel_urb(wrap_urb) == 0)
				USBTRACE("canceled wrap_urb: %p", wrap_urb);
		}
	}
	IoReleaseCancelSpinLock(irql);
	NT_URB_STATUS(nt_urb) = USBD_STATUS_CANCELED;
	USBEXIT(return USBD_STATUS_SUCCESS);
}

static USBD_STATUS wrap_set_clear_feature(struct usb_device *udev,
					  struct irp *irp)
{
	union nt_urb *nt_urb;
	struct urb_control_feature_request *feat_req;
	int ret = 0;
	__u8 request, type;
	__u16 feature;

	nt_urb = IRP_URB(irp);
	feat_req = &nt_urb->feat_req;
	feature = feat_req->feature_selector;
	switch (nt_urb->header.function) {
	case URB_FUNCTION_SET_FEATURE_TO_DEVICE:
		request = USB_REQ_SET_FEATURE;
		type = USB_DT_DEVICE;
		break;
	case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:
		request = USB_REQ_SET_FEATURE;
		type = USB_DT_INTERFACE;
		break;
	case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:
		request = USB_REQ_SET_FEATURE;
		type = USB_DT_ENDPOINT;
		break;
	case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:
		request = USB_REQ_CLEAR_FEATURE;
		type = USB_DT_DEVICE;
		break;
	case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:
		request = USB_REQ_CLEAR_FEATURE;
		type = USB_DT_INTERFACE;
		break;
	case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:
		request = USB_REQ_CLEAR_FEATURE;
		type = USB_DT_ENDPOINT;
		break;
	default:
		WARNING("invalid function: %x", nt_urb->header.function);
		NT_URB_STATUS(nt_urb) = USBD_STATUS_NOT_SUPPORTED;
		return NT_URB_STATUS(nt_urb);
	}
	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0), request, type,
			      feature, feat_req->index, NULL, 0, 1000);
	NT_URB_STATUS(nt_urb) = wrap_urb_status(ret);
	USBEXIT(return NT_URB_STATUS(nt_urb));
}

static USBD_STATUS wrap_get_status_request(struct usb_device *udev,
					   struct irp *irp)
{
	union nt_urb *nt_urb;
	struct urb_control_get_status_request *status_req;
	int ret = 0;
	__u8 type;

	nt_urb = IRP_URB(irp);
	status_req = &nt_urb->status_req;
	switch (nt_urb->header.function) {
	case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
		type = USB_RECIP_DEVICE;
		break;
	case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
		type = USB_RECIP_INTERFACE;
		break;
	case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
		type = USB_RECIP_ENDPOINT;
		break;
	default:
		WARNING("invalid function: %x", nt_urb->header.function);
		NT_URB_STATUS(nt_urb) = USBD_STATUS_NOT_SUPPORTED;
		return NT_URB_STATUS(nt_urb);
	}
	assert(status_req->transfer_buffer_length == sizeof(u16));
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
	ret = usb_get_std_status(udev, type, status_req->index,
			         status_req->transfer_buffer);
#else
	ret = usb_get_status(udev, type, status_req->index,
			     status_req->transfer_buffer);
#endif
	if (ret >= 0) {
		assert(ret <= status_req->transfer_buffer_length);
		status_req->transfer_buffer_length = ret;
		NT_URB_STATUS(nt_urb) = USBD_STATUS_SUCCESS;
	} else
		NT_URB_STATUS(nt_urb) = wrap_urb_status(ret);
	USBEXIT(return NT_URB_STATUS(nt_urb));
}

static void set_intf_pipe_info(struct wrap_device *wd,
			       struct usb_interface *usb_intf,
			       struct usbd_interface_information *intf)
{
	int i;
	struct usb_endpoint_descriptor *ep;
	struct usbd_pipe_information *pipe;

	for (i = 0; i < CUR_ALT_SETTING(usb_intf)->desc.bNumEndpoints; i++) {
		ep = &(CUR_ALT_SETTING(usb_intf)->endpoint[i]).desc;
		if (i >= intf->bNumEndpoints) {
			ERROR("intf %p has only %d endpoints, "
			      "ignoring endpoints above %d",
			      intf, intf->bNumEndpoints, i);
			break;
		}
		pipe = &intf->pipes[i];

		if (pipe->flags & USBD_PF_CHANGE_MAX_PACKET)
			USBTRACE("pkt_sz: %d: %d", pipe->wMaxPacketSize,
				 pipe->max_tx_size);
		USBTRACE("driver wants max_tx_size to %d",
			 pipe->max_tx_size);

		pipe->wMaxPacketSize = le16_to_cpu(ep->wMaxPacketSize);
		pipe->bEndpointAddress = ep->bEndpointAddress;
		pipe->type = ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;
		if (pipe->type == UsbdPipeTypeInterrupt) {
			/* Windows and Linux differ in how the
			 * bInterval is interpreted */
			/* for low speed:
			   interval (Windows) -> frames per ms (Linux)
			   0 to 15    -> 8
			   16 to 35   -> 16
			   36 to 255  -> 32

			   for full speed: interval -> frames per ms
			   1          -> 1
			   2 to 3     -> 2
			   4 to 7     -> 4
			   8 to 15    -> 8
			   16 to 31   -> 16
			   32 to 255  -> 32

			   for high speed: interval -> microframes
			   1          -> 1
			   2          -> 2
			   3          -> 4
			   4          -> 8
			   5          -> 16
			   6          -> 32
			   7 to 255   -> 32
			*/
			if (wd->usb.udev->speed == USB_SPEED_LOW)
				pipe->bInterval = ep->bInterval + 5;
			else if (wd->usb.udev->speed == USB_SPEED_FULL)
				pipe->bInterval = ep->bInterval;
			else {
				int j, k;
				for (j = k = 1; j < ep->bInterval; k++)
					j *= 2;
				pipe->bInterval = k;
			}
		}
		pipe->handle = ep;
		USBTRACE("%d: ep 0x%x, type %d, pkt_sz %d, intv %d (%d),"
			 "type: %d, handle %p", i, ep->bEndpointAddress,
			 ep->bmAttributes, pipe->wMaxPacketSize, ep->bInterval,
			 pipe->bInterval, pipe->type, pipe->handle);
	}
}

static USBD_STATUS wrap_select_configuration(struct wrap_device *wd,
					     union nt_urb *nt_urb,
					     struct irp *irp)
{
	int i, ret;
	struct usbd_select_configuration *sel_conf;
	struct usb_device *udev;
	struct usbd_interface_information *intf;
	struct usb_config_descriptor *config;
	struct usb_interface *usb_intf;

	udev = wd->usb.udev;
	sel_conf = &nt_urb->select_conf;
	config = sel_conf->config;
	USBTRACE("%p", config);
	if (config == NULL) {
		kill_all_urbs(wd, 1);
		ret = usb_reset_configuration(udev);
		return wrap_urb_status(ret);
	}

	USBTRACE("conf: %d, type: %d, length: %d, numif: %d, attr: %08x",
		 config->bConfigurationValue, config->bDescriptorType,
		 config->wTotalLength, config->bNumInterfaces,
		 config->bmAttributes);
	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      USB_REQ_SET_CONFIGURATION, 0,
			      config->bConfigurationValue, 0,
			      NULL, 0, USB_CTRL_SET_TIMEOUT);
	if (ret < 0) {
		ERROR("ret: %d", ret);
		return wrap_urb_status(ret);
	}
	sel_conf->handle = udev->actconfig;
	intf = &sel_conf->intf;
	for (i = 0; i < config->bNumInterfaces && intf->bLength > 0;
	     i++, intf = (((void *)intf) + intf->bLength)) {

		USBTRACE("intf: %d, alt setting: %d",
			 intf->bInterfaceNumber, intf->bAlternateSetting);
		ret = usb_set_interface(udev, intf->bInterfaceNumber,
					intf->bAlternateSetting);
		if (ret < 0) {
			ERROR("failed with %d", ret);
			return wrap_urb_status(ret);
		}
		usb_intf = usb_ifnum_to_if(udev, intf->bInterfaceNumber);
		if (!usb_intf) {
			ERROR("couldn't obtain ifnum");
			return USBD_STATUS_REQUEST_FAILED;
		}
		USBTRACE("intf: %p, num ep: %d", intf, intf->bNumEndpoints);
		set_intf_pipe_info(wd, usb_intf, intf);
	}
	return USBD_STATUS_SUCCESS;
}

static USBD_STATUS wrap_select_interface(struct wrap_device *wd,
					 union nt_urb *nt_urb,
					 struct irp *irp)
{
	int ret;
	struct usbd_select_interface *sel_intf;
	struct usb_device *udev;
	struct usbd_interface_information *intf;
	struct usb_interface *usb_intf;

	udev = wd->usb.udev;
	sel_intf = &nt_urb->select_intf;
	intf = &sel_intf->intf;

	ret = usb_set_interface(udev, intf->bInterfaceNumber,
				intf->bAlternateSetting);
	if (ret < 0) {
		ERROR("failed with %d", ret);
		return wrap_urb_status(ret);
	}
	usb_intf = usb_ifnum_to_if(udev, intf->bInterfaceNumber);
	if (!usb_intf) {
		ERROR("couldn't get interface information");
		return USBD_STATUS_REQUEST_FAILED;
	}
	USBTRACE("intf: %p, num ep: %d", usb_intf, intf->bNumEndpoints);
	set_intf_pipe_info(wd, usb_intf, intf);
	return USBD_STATUS_SUCCESS;
}

static int wrap_usb_get_string(struct usb_device *udev, unsigned short langid,
			       unsigned char index, void *buf, int size)
{
	int i, ret;
	/* if langid is 0, return array of languages supported in
	 * buf */
	for (i = 0; i < 3; i++) {
		ret = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
				      USB_REQ_GET_DESCRIPTOR, USB_DIR_IN,
				      (USB_DT_STRING << 8) + index, langid,
				      buf, size, USB_CTRL_GET_TIMEOUT);
		if (ret > 0 || ret == -EPIPE)
			break;
	}
	return ret;
}

static USBD_STATUS wrap_get_descriptor(struct wrap_device *wd,
				       union nt_urb *nt_urb, struct irp *irp)
{
	struct usbd_control_descriptor_request *control_desc;
	int ret = 0;
	struct usb_device *udev;

	udev = wd->usb.udev;
	control_desc = &nt_urb->control_desc;
	USBTRACE("desctype = %d, descindex = %d, transfer_buffer = %p,"
		 "transfer_buffer_length = %d", control_desc->desc_type,
		 control_desc->index, control_desc->transfer_buffer,
		 control_desc->transfer_buffer_length);

	if (control_desc->desc_type == USB_DT_STRING) {
		USBTRACE("langid: %x", control_desc->language_id);
		ret = wrap_usb_get_string(udev, control_desc->language_id,
					  control_desc->index,
					  control_desc->transfer_buffer,
					  control_desc->transfer_buffer_length);
	} else {
		ret = usb_get_descriptor(udev, control_desc->desc_type,
					 control_desc->index,
					 control_desc->transfer_buffer,
					 control_desc->transfer_buffer_length);
	}
	if (ret < 0) {
		USBTRACE("request %d failed: %d", control_desc->desc_type, ret);
		control_desc->transfer_buffer_length = 0;
		return wrap_urb_status(ret);
	} else {
		USBTRACE("ret: %08x", ret);
		control_desc->transfer_buffer_length = ret;
		irp->io_status.info = ret;
		return USBD_STATUS_SUCCESS;
	}
}

static USBD_STATUS wrap_process_nt_urb(struct irp *irp)
{
	USBD_STATUS status;
	struct wrap_device *wd = IRP_WRAP_DEVICE(irp);
	struct usb_device *udev = wd->usb.udev;
	union nt_urb *nt_urb = IRP_URB(irp);

	USBENTER("nt_urb = %p, irp = %p, length = %d, function = %x",
		 nt_urb, irp, nt_urb->header.length, nt_urb->header.function);

	if (test_bit(HW_DISABLED, &wd->hw_status)) {
		status = USBD_STATUS_DEVICE_GONE;
		NT_URB_STATUS(nt_urb) = status;
		return status;
	}

	DUMP_IRP(irp);
	switch (nt_urb->header.function) {
		/* bulk/int and vendor/class urbs are submitted to
		 * Linux USB core; if the call is successful, urb's
		 * completion worker will return IRP later */
	case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
		USBTRACE("submitting bulk/int irp: %p", irp);
		status = wrap_bulk_or_intr_trans(irp);
		break;

	case URB_FUNCTION_VENDOR_DEVICE:
	case URB_FUNCTION_VENDOR_INTERFACE:
	case URB_FUNCTION_VENDOR_ENDPOINT:
	case URB_FUNCTION_VENDOR_OTHER:
	case URB_FUNCTION_CLASS_DEVICE:
	case URB_FUNCTION_CLASS_INTERFACE:
	case URB_FUNCTION_CLASS_ENDPOINT:
	case URB_FUNCTION_CLASS_OTHER:
		USBTRACE("submitting vendor/class irp: %p", irp);
		status = wrap_vendor_or_class_req(irp);
		break;

		/* rest are synchronous */
	case URB_FUNCTION_SELECT_CONFIGURATION:
		status = wrap_select_configuration(wd, nt_urb, irp);
		NT_URB_STATUS(nt_urb) = status;
		break;

	case URB_FUNCTION_SELECT_INTERFACE:
		status = wrap_select_interface(wd, nt_urb, irp);
		NT_URB_STATUS(nt_urb) = status;
		break;

	case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
		status = wrap_get_descriptor(wd, nt_urb, irp);
		NT_URB_STATUS(nt_urb) = status;
		break;

	case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
		status = wrap_reset_pipe(udev, irp);
		NT_URB_STATUS(nt_urb) = status;
		break;

	case URB_FUNCTION_ABORT_PIPE:
		status = wrap_abort_pipe(udev, irp);
		break;

	case URB_FUNCTION_SET_FEATURE_TO_DEVICE:
	case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:
	case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:
	case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:
	case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:
	case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:
		status = wrap_set_clear_feature(udev, irp);
		break;

	case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
	case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
	case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
		status = wrap_get_status_request(udev, irp);
		break;

	default:
		ERROR("function %x not implemented", nt_urb->header.function);
		status = NT_URB_STATUS(nt_urb) = USBD_STATUS_NOT_SUPPORTED;
		break;
	}
	USBTRACE("status: %08X", status);
	return status;
}

static USBD_STATUS wrap_reset_port(struct irp *irp)
{
	int ret, lock = 0;
	struct wrap_device *wd;

	wd = IRP_WRAP_DEVICE(irp);
	USBENTER("%p, %p", wd, wd->usb.udev);
	lock = usb_lock_device_for_reset(wd->usb.udev, wd->usb.intf);
	if (lock < 0) {
		WARNING("locking failed: %d", lock);
//		return wrap_urb_status(lock);
		return USBD_STATUS_SUCCESS;
	}
	ret = usb_reset_device(wd->usb.udev);
	if (ret < 0)
		USBTRACE("reset failed: %d", ret);
	/* TODO: should reconfigure? */
	if (lock)
		usb_unlock_device(wd->usb.udev);
//	return wrap_urb_status(ret);
	return USBD_STATUS_SUCCESS;
}

static USBD_STATUS wrap_get_port_status(struct irp *irp)
{
	struct wrap_device *wd;
	ULONG *status;
	enum usb_device_state state;

	wd = IRP_WRAP_DEVICE(irp);
	USBENTER("%p, %p", wd, wd->usb.udev);
	status = IoGetCurrentIrpStackLocation(irp)->params.others.arg1;
	state = wd->usb.udev->state;
	if (state != USB_STATE_NOTATTACHED &&
	    state != USB_STATE_SUSPENDED) {
		*status |= USBD_PORT_CONNECTED;
		if (state == USB_STATE_CONFIGURED)
			*status |= USBD_PORT_ENABLED;
	}
	USBTRACE("state: %d, *status: %08X", state, *status);
	return USBD_STATUS_SUCCESS;
}

NTSTATUS wrap_submit_irp(struct device_object *pdo, struct irp *irp)
{
	struct io_stack_location *irp_sl;
	struct wrap_device *wd;
	USBD_STATUS status;
	struct usbd_idle_callback *idle_callback;

	USBENTER("%p, %p", pdo, irp);
	wd = pdo->reserved;
	if (wd->usb.intf == NULL) {
		USBTRACE("%p", irp);
		irp->io_status.status = STATUS_DEVICE_REMOVED;
		irp->io_status.info = 0;
		USBEXIT(return STATUS_DEVICE_REMOVED);
	}
	IRP_WRAP_DEVICE(irp) = wd;
	irp_sl = IoGetCurrentIrpStackLocation(irp);
	switch (irp_sl->params.dev_ioctl.code) {
	case IOCTL_INTERNAL_USB_SUBMIT_URB:
		status = wrap_process_nt_urb(irp);
		break;
	case IOCTL_INTERNAL_USB_RESET_PORT:
		status = wrap_reset_port(irp);
		break;
	case IOCTL_INTERNAL_USB_GET_PORT_STATUS:
		status = wrap_get_port_status(irp);
		break;
	case IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION:
		idle_callback = irp_sl->params.dev_ioctl.type3_input_buf;
		(void)idle_callback;
		USBTRACE("suspend function: %p", idle_callback->callback);
		status = USBD_STATUS_NOT_SUPPORTED;
		break;
	default:
		ERROR("ioctl %08X NOT IMPLEMENTED",
		      irp_sl->params.dev_ioctl.code);
		status = USBD_STATUS_NOT_SUPPORTED;
		break;
	}

	USBTRACE("status: %08X", status);
	if (status == USBD_STATUS_PENDING) {
		/* don't touch this IRP - it may have been already
		 * completed/returned */
		return STATUS_PENDING;
	} else {
		irp->io_status.status = nt_urb_irp_status(status);
		if (status != USBD_STATUS_SUCCESS)
			irp->io_status.info = 0;
		USBEXIT(return irp->io_status.status);
	}
}

/* TODO: The example on msdn in reference section suggests that second
 * argument should be an array of usbd_interface_information, but
 * description and examples elsewhere suggest that it should be
 * usbd_interface_list_entry structre. Which is correct? */

wstdcall union nt_urb *WIN_FUNC(USBD_CreateConfigurationRequestEx,2)
	(struct usb_config_descriptor *config,
	 struct usbd_interface_list_entry *intf_list)
{
	int size, i, n;
	struct usbd_interface_information *intf;
	struct usbd_pipe_information *pipe;
	struct usb_interface_descriptor *intf_desc;
	struct usbd_select_configuration *select_conf;

	USBENTER("config = %p, intf_list = %p", config, intf_list);

	/* calculate size required; select_conf already has space for
	 * one intf structure */
	size = sizeof(*select_conf) - sizeof(*intf);
	for (n = 0; n < config->bNumInterfaces; n++) {
		i = intf_list[n].intf_desc->bNumEndpoints;
		/* intf already has space for one pipe */
		size += sizeof(*intf) + (i - 1) * sizeof(*pipe);
	}
	/* don't use kmalloc - driver frees it with ExFreePool */
	select_conf = ExAllocatePoolWithTag(NonPagedPool, size,
					    POOL_TAG('L', 'U', 'S', 'B'));
	if (!select_conf) {
		WARNING("couldn't allocate memory");
		return NULL;
	}
	memset(select_conf, 0, size);
	intf = &select_conf->intf;
	select_conf->handle = config;
	for (n = 0; n < config->bNumInterfaces && intf_list[n].intf_desc; n++) {
		/* initialize 'intf' fields in intf_list so they point
		 * to appropriate entry; these may be read/written by
		 * driver after this function returns */
		intf_list[n].intf = intf;
		intf_desc = intf_list[n].intf_desc;

		i = intf_desc->bNumEndpoints;
		intf->bLength = sizeof(*intf) + (i - 1) * sizeof(*pipe);

		intf->bInterfaceNumber = intf_desc->bInterfaceNumber;
		intf->bAlternateSetting = intf_desc->bAlternateSetting;
		intf->bInterfaceClass = intf_desc->bInterfaceClass;
		intf->bInterfaceSubClass = intf_desc->bInterfaceSubClass;
		intf->bInterfaceProtocol = intf_desc->bInterfaceProtocol;
		intf->bNumEndpoints = intf_desc->bNumEndpoints;

		pipe = &intf->pipes[0];
		for (i = 0; i < intf->bNumEndpoints; i++) {
			memset(&pipe[i], 0, sizeof(*pipe));
			pipe[i].max_tx_size =
				USBD_DEFAULT_MAXIMUM_TRANSFER_SIZE;
		}
		intf->handle = intf_desc;
		intf = (((void *)intf) + intf->bLength);
	}
	select_conf->header.function = URB_FUNCTION_SELECT_CONFIGURATION;
	select_conf->header.length = size;
	select_conf->config = config;
	USBEXIT(return (union nt_urb *)select_conf);
}

WIN_SYMBOL_MAP("_USBD_CreateConfigurationRequestEx@8", USBD_CreateConfigurationRequestEx)

wstdcall struct usb_interface_descriptor *
WIN_FUNC(USBD_ParseConfigurationDescriptorEx,7)
	(struct usb_config_descriptor *config, void *start,
	 LONG bInterfaceNumber, LONG bAlternateSetting, LONG bInterfaceClass,
	 LONG bInterfaceSubClass, LONG bInterfaceProtocol)
{
	void *pos;
	struct usb_interface_descriptor *intf;

	USBENTER("config = %p, start = %p, ifnum = %d, alt_setting = %d,"
		 " class = %d, subclass = %d, proto = %d", config, start,
		 bInterfaceNumber, bAlternateSetting, bInterfaceClass,
		 bInterfaceSubClass, bInterfaceProtocol);

	for (pos = start;
	     pos < ((void *)config + le16_to_cpu(config->wTotalLength));
	     pos += intf->bLength) {

		intf = pos;

		if ((intf->bDescriptorType == USB_DT_INTERFACE) &&
		    ((bInterfaceNumber == -1) ||
		     (intf->bInterfaceNumber == bInterfaceNumber)) &&
		    ((bAlternateSetting == -1) ||
		     (intf->bAlternateSetting == bAlternateSetting)) &&
		    ((bInterfaceClass == -1) ||
		     (intf->bInterfaceClass == bInterfaceClass)) &&
		    ((bInterfaceSubClass == -1) ||
		     (intf->bInterfaceSubClass == bInterfaceSubClass)) &&
		    ((bInterfaceProtocol == -1) ||
		     (intf->bInterfaceProtocol == bInterfaceProtocol))) {
			USBTRACE("selected interface = %p", intf);
			USBEXIT(return intf);
		}
	}
	USBEXIT(return NULL);
}

WIN_SYMBOL_MAP("_USBD_ParseConfigurationDescriptorEx@28", USBD_ParseConfigurationDescriptorEx)

wstdcall union nt_urb *WIN_FUNC(USBD_CreateConfigurationRequest,2)
	(struct usb_config_descriptor *config, USHORT *size)
{
	union nt_urb *nt_urb;
	struct usbd_interface_list_entry intf_list[2];
	struct usb_interface_descriptor *intf_desc;

	USBENTER("config = %p, urb_size = %p", config, size);

	intf_desc = USBD_ParseConfigurationDescriptorEx(config, config, -1, -1,
							-1, -1, -1);
	intf_list[0].intf_desc = intf_desc;
	intf_list[0].intf = NULL;
	intf_list[1].intf_desc = NULL;
	intf_list[1].intf = NULL;
	nt_urb = USBD_CreateConfigurationRequestEx(config, intf_list);
	if (!nt_urb)
		return NULL;

	*size = nt_urb->select_conf.header.length;
	USBEXIT(return nt_urb);
}

wstdcall struct usb_interface_descriptor *
WIN_FUNC(USBD_ParseConfigurationDescriptor,3)
	(struct usb_config_descriptor *config, UCHAR bInterfaceNumber,
	 UCHAR bAlternateSetting)
{
	return USBD_ParseConfigurationDescriptorEx(config, config,
						   bInterfaceNumber,
						   bAlternateSetting,
						   -1, -1, -1);
}

wstdcall struct usb_descriptor_header *WIN_FUNC(USBD_ParseDescriptors,4)
	(void *buf, ULONG length, struct usb_descriptor_header *descr,
	 LONG type)
{
	while ((void *)descr < buf + length) {
		if (descr->bDescriptorType == type)
			return descr;
		if (descr->bLength == 0)
			break;
		descr = (void *)descr + descr->bLength;
	}
	USBEXIT(return NULL);
}

WIN_SYMBOL_MAP("_USBD_ParseDescriptors@16", USBD_ParseDescriptors)

wstdcall void WIN_FUNC(USBD_GetUSBDIVersion,1)
	(struct usbd_version_info *version_info)
{
	/* this function is obsolete in Windows XP */
	if (version_info) {
		version_info->usbdi_version = USBDI_VERSION_XP;
		/* TODO: how do we get this correctly? */
		version_info->supported_usb_version = 0x110;
	}
	USBEXIT(return);
}

wstdcall void
USBD_InterfaceGetUSBDIVersion(void *context,
			      struct usbd_version_info *version_info,
			      ULONG *hcd_capa)
{
	struct wrap_device *wd = context;

	if (version_info) {
		version_info->usbdi_version = USBDI_VERSION_XP;
		if (wd->usb.udev->speed == USB_SPEED_HIGH)
			version_info->supported_usb_version = 0x200;
		else
			version_info->supported_usb_version = 0x110;
	}
	*hcd_capa = USB_HCD_CAPS_SUPPORTS_RT_THREADS;
	USBEXIT(return);
}

wstdcall BOOLEAN USBD_InterfaceIsDeviceHighSpeed(void *context)
{
	struct wrap_device *wd = context;

	USBTRACE("wd: %p", wd);
	if (wd->usb.udev->speed == USB_SPEED_HIGH)
		USBEXIT(return TRUE);
	else
		USBEXIT(return FALSE);
}

wstdcall void USBD_InterfaceReference(void *context)
{
	USBTRACE("%p", context);
	TODO();
}

wstdcall void USBD_InterfaceDereference(void *context)
{
	USBTRACE("%p", context);
	TODO();
}

wstdcall NTSTATUS USBD_InterfaceQueryBusTime(void *context, ULONG *frame)
{
	struct wrap_device *wd = context;

	*frame = usb_get_current_frame_number(wd->usb.udev);
	USBEXIT(return STATUS_SUCCESS);
}

wstdcall NTSTATUS USBD_InterfaceSubmitIsoOutUrb(void *context,
					       union nt_urb *nt_urb)
{
	/* TODO: implement this */
	TODO();
	USBEXIT(return STATUS_NOT_IMPLEMENTED);
}

wstdcall NTSTATUS
USBD_InterfaceQueryBusInformation(void *context, ULONG level, void *buf,
				  ULONG *buf_length, ULONG *buf_actual_length)
{
#if 0
	struct wrap_device *wd = context;
	struct usb_bus *bus = wd->usb.udev->bus;
	struct usb_bus_information_level *bus_info = buf;
#endif

	TODO();
	USBEXIT(return STATUS_NOT_IMPLEMENTED);
}

wstdcall NTSTATUS
USBD_InterfaceLogEntry(void *context, ULONG driver_tag, ULONG enum_tag,
		       ULONG p1, ULONG p2)
{
	ERROR("%p, %x, %x, %x, %x", context, driver_tag, enum_tag, p1, p2);
	USBEXIT(return STATUS_SUCCESS);
}

NTSTATUS
usb_query_interface(struct wrap_device *wd, struct io_stack_location *irp_sl)
{
	struct usbd_bus_interface_usbdi *intf =
		(struct usbd_bus_interface_usbdi *)
		irp_sl->params.query_intf.intf;

	TRACE2("type: %x, size: %d, version: %d",
	       irp_sl->params.query_intf.type->data1,
	       irp_sl->params.query_intf.size,
	       irp_sl->params.query_intf.version);
	intf->Context = wd;
	intf->InterfaceReference =
		WIN_FUNC_PTR(USBD_InterfaceReference, 1);
	intf->InterfaceDereference =
		WIN_FUNC_PTR(USBD_InterfaceDereference, 1);
	intf->GetUSBDIVersion =
		WIN_FUNC_PTR(USBD_InterfaceGetUSBDIVersion, 3);
	intf->QueryBusTime =
		WIN_FUNC_PTR(USBD_InterfaceQueryBusTime, 2);
	intf->SubmitIsoOutUrb =
		WIN_FUNC_PTR(USBD_InterfaceSubmitIsoOutUrb, 2);
	intf->QueryBusInformation =
		WIN_FUNC_PTR(USBD_InterfaceQueryBusInformation, 5);
	if (irp_sl->params.query_intf.version >= USB_BUSIF_USBDI_VERSION_1)
		intf->IsDeviceHighSpeed =
			WIN_FUNC_PTR(USBD_InterfaceIsDeviceHighSpeed, 1);
	if (irp_sl->params.query_intf.version >= USB_BUSIF_USBDI_VERSION_2)
		intf->LogEntry = WIN_FUNC_PTR(USBD_InterfaceLogEntry, 5);
	return STATUS_SUCCESS;
}

int usb_init(void)
{
	InitializeListHead(&wrap_urb_complete_list);
	spin_lock_init(&wrap_urb_complete_list_lock);
	INIT_WORK(&wrap_urb_complete_work, wrap_urb_complete_worker);
#ifdef USB_DEBUG
	urb_id = 0;
#endif
	return 0;
}

void usb_exit(void)
{
	USBEXIT(return);
}

int usb_init_device(struct wrap_device *wd)
{
	InitializeListHead(&wd->usb.wrap_urb_list);
	wd->usb.num_alloc_urbs = 0;
	USBEXIT(return 0);
}

void usb_exit_device(struct wrap_device *wd)
{
	kill_all_urbs(wd, 0);
	USBEXIT(return);
}
