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

#include <linux/inetdevice.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/proc_fs.h>
#include "ndis.h"
#include "iw_ndis.h"
#include "pnp.h"
#include "loader.h"
#include "wrapndis.h"
#include "wrapper.h"

/* Functions callable from the NDIS driver */
wstdcall NTSTATUS NdisDispatchDeviceControl(struct device_object *fdo,
					    struct irp *irp);
wstdcall NTSTATUS NdisDispatchPnp(struct device_object *fdo, struct irp *irp);
wstdcall NTSTATUS NdisDispatchPower(struct device_object *fdo, struct irp *irp);

struct workqueue_struct *wrapndis_wq;

static int set_packet_filter(struct ndis_device *wnd,
			     ULONG packet_filter);
static void add_iw_stats_timer(struct ndis_device *wnd);
static void del_iw_stats_timer(struct ndis_device *wnd);
static NDIS_STATUS ndis_start_device(struct ndis_device *wnd);
static int ndis_remove_device(struct ndis_device *wnd);
static void set_multicast_list(struct ndis_device *wnd);
static int ndis_net_dev_open(struct net_device *net_dev);
static int ndis_net_dev_close(struct net_device *net_dev);

/* MiniportReset */
NDIS_STATUS mp_reset(struct ndis_device *wnd)
{
	NDIS_STATUS res;
	struct miniport *mp;
	BOOLEAN reset_address;
	KIRQL irql;

	ENTER2("wnd: %p", wnd);
	mutex_lock(&wnd->tx_ring_mutex);
	mutex_lock(&wnd->ndis_req_mutex);
	mp = &wnd->wd->driver->ndis_driver->mp;
	prepare_wait_condition(wnd->ndis_req_task, wnd->ndis_req_done, 0);
	WARNING("%s is being reset", wnd->net_dev->name);
	irql = serialize_lock_irql(wnd);
	assert_irql(_irql_ == DISPATCH_LEVEL);
	res = LIN2WIN2(mp->reset, &reset_address, wnd->nmb->mp_ctx);
	serialize_unlock_irql(wnd, irql);

	TRACE2("%08X, %08X", res, reset_address);
	if (res == NDIS_STATUS_PENDING) {
		/* wait for NdisMResetComplete */
		if (wait_condition((wnd->ndis_req_done > 0), 0,
				   TASK_INTERRUPTIBLE) < 0)
			res = NDIS_STATUS_FAILURE;
		else {
			res = wnd->ndis_req_status;
			reset_address = wnd->ndis_req_done - 1;
		}
		TRACE2("%08X, %08X", res, reset_address);
	}
	mutex_unlock(&wnd->ndis_req_mutex);
	if (res == NDIS_STATUS_SUCCESS && reset_address) {
		set_packet_filter(wnd, wnd->packet_filter);
		set_multicast_list(wnd);
	}
	mutex_unlock(&wnd->tx_ring_mutex);
	EXIT3(return res);
}

/* MiniportRequest(Query/Set)Information */
NDIS_STATUS mp_request(enum ndis_request_type request,
		       struct ndis_device *wnd, ndis_oid oid,
		       void *buf, ULONG buflen, ULONG *written, ULONG *needed)
{
	NDIS_STATUS res;
	ULONG w, n;
	struct miniport *mp;
	KIRQL irql;

	mutex_lock(&wnd->ndis_req_mutex);
	if (!written)
		written = &w;
	if (!needed)
		needed = &n;
	mp = &wnd->wd->driver->ndis_driver->mp;
	prepare_wait_condition(wnd->ndis_req_task, wnd->ndis_req_done, 0);
	irql = serialize_lock_irql(wnd);
	assert_irql(_irql_ == DISPATCH_LEVEL);
	switch (request) {
	case NdisRequestQueryInformation:
		TRACE2("%p, %08X, %p", mp->queryinfo, oid, wnd->nmb->mp_ctx);
		res = LIN2WIN6(mp->queryinfo, wnd->nmb->mp_ctx, oid, buf,
			       buflen, written, needed);
		break;
	case NdisRequestSetInformation:
		TRACE2("%p, %08X, %p", mp->setinfo, oid, wnd->nmb->mp_ctx);
		res = LIN2WIN6(mp->setinfo, wnd->nmb->mp_ctx, oid, buf,
			       buflen, written, needed);
		break;
	default:
		WARNING("invalid request %d, %08X", request, oid);
		res = NDIS_STATUS_NOT_SUPPORTED;
		break;
	}
	serialize_unlock_irql(wnd, irql);
	TRACE2("%08X, %08X", res, oid);
	if (res == NDIS_STATUS_PENDING) {
		/* wait for NdisMQueryInformationComplete */
		if (wait_condition((wnd->ndis_req_done > 0), 0,
				   TASK_INTERRUPTIBLE) < 0)
			res = NDIS_STATUS_FAILURE;
		else
			res = wnd->ndis_req_status;
		TRACE2("%08X, %08X", res, oid);
	}
	mutex_unlock(&wnd->ndis_req_mutex);
	DBG_BLOCK(2) {
		if (res || needed)
			TRACE2("%08X, %d, %d, %d", res, buflen, *written,
			       *needed);
	}
	EXIT3(return res);
}

/* MiniportPnPEventNotify */
static NDIS_STATUS mp_pnp_event(struct ndis_device *wnd,
				enum ndis_device_pnp_event event,
				ULONG power_profile)
{
	struct miniport *mp;

	ENTER1("%p, %d", wnd, event);
	mp = &wnd->wd->driver->ndis_driver->mp;
	if (!mp->pnp_event_notify) {
		TRACE1("Windows driver %s doesn't support "
		       "MiniportPnpEventNotify", wnd->wd->driver->name);
		return NDIS_STATUS_FAILURE;
	}
	/* RNDIS driver doesn't like to be notified if device is
	 * already halted */
	if (!test_bit(HW_INITIALIZED, &wnd->wd->hw_status))
		EXIT1(return NDIS_STATUS_SUCCESS);
	switch (event) {
	case NdisDevicePnPEventSurpriseRemoved:
		TRACE1("%u, %p",
		       (wnd->attributes & NDIS_ATTRIBUTE_SURPRISE_REMOVE_OK),
		       mp->pnp_event_notify);
		if ((wnd->attributes & NDIS_ATTRIBUTE_SURPRISE_REMOVE_OK) &&
		    !test_bit(HW_DISABLED, &wnd->wd->hw_status) &&
		    mp->pnp_event_notify) {
			TRACE1("calling surprise_removed");
			LIN2WIN4(mp->pnp_event_notify, wnd->nmb->mp_ctx,
				 NdisDevicePnPEventSurpriseRemoved, NULL, 0);
		} else
			TRACE1("Windows driver %s doesn't support "
			       "MiniportPnpEventNotify for safe unplugging",
			       wnd->wd->driver->name);
		return NDIS_STATUS_SUCCESS;
	case NdisDevicePnPEventPowerProfileChanged:
		if (power_profile)
			power_profile = NdisPowerProfileAcOnLine;
		LIN2WIN4(mp->pnp_event_notify, wnd->nmb->mp_ctx,
			 NdisDevicePnPEventPowerProfileChanged,
			 &power_profile, sizeof(power_profile));
		return NDIS_STATUS_SUCCESS;
	default:
		WARNING("event %d not yet implemented", event);
		return NDIS_STATUS_SUCCESS;
	}
}

/* MiniportInitialize */
static NDIS_STATUS mp_init(struct ndis_device *wnd)
{
	NDIS_STATUS error_status, status;
	UINT medium_index;
	enum ndis_medium medium_array[] = {NdisMedium802_3};
	struct miniport *mp;

	ENTER1("irql: %d", current_irql());
	if (test_bit(HW_INITIALIZED, &wnd->wd->hw_status)) {
		WARNING("device %p already initialized!", wnd);
		return NDIS_STATUS_FAILURE;
	}

	if (!wnd->wd->driver->ndis_driver ||
	    !wnd->wd->driver->ndis_driver->mp.init) {
		WARNING("assuming WDM (non-NDIS) driver");
		EXIT1(return NDIS_STATUS_NOT_RECOGNIZED);
	}
	mp = &wnd->wd->driver->ndis_driver->mp;
	status = LIN2WIN6(mp->init, &error_status, &medium_index, medium_array,
			  ARRAY_SIZE(medium_array), wnd->nmb, wnd->nmb);
	TRACE1("init returns: %08X, irql: %d", status, current_irql());
	if (status != NDIS_STATUS_SUCCESS) {
		WARNING("couldn't initialize device: %08X", status);
		EXIT1(return NDIS_STATUS_FAILURE);
	}

	/* Wait a little to let card power up otherwise ifup might
	 * fail after boot */
	sleep_hz(HZ / 5);
	status = mp_pnp_event(wnd, NdisDevicePnPEventPowerProfileChanged,
			      NdisPowerProfileAcOnLine);
	if (status != NDIS_STATUS_SUCCESS)
		TRACE1("setting power failed: %08X", status);
	set_bit(HW_INITIALIZED, &wnd->wd->hw_status);
	/* the description about NDIS_ATTRIBUTE_NO_HALT_ON_SUSPEND is
	 * misleading/confusing */
	status = mp_query(wnd, OID_PNP_CAPABILITIES,
			  &wnd->pnp_capa, sizeof(wnd->pnp_capa));
	if (status == NDIS_STATUS_SUCCESS) {
		TRACE1("%d, %d", wnd->pnp_capa.wakeup.min_magic_packet_wakeup,
		       wnd->pnp_capa.wakeup.min_pattern_wakeup);
		wnd->attributes |= NDIS_ATTRIBUTE_NO_HALT_ON_SUSPEND;
		status = mp_query_int(wnd, OID_PNP_ENABLE_WAKE_UP,
				      &wnd->ndis_wolopts);
		TRACE1("%08X, %x", status, wnd->ndis_wolopts);
	} else if (status == NDIS_STATUS_NOT_SUPPORTED)
		wnd->attributes &= ~NDIS_ATTRIBUTE_NO_HALT_ON_SUSPEND;
	TRACE1("%d", wnd->pnp_capa.wakeup.min_magic_packet_wakeup);
	/* although some NDIS drivers support suspend, Linux kernel
	 * has issues with suspending USB devices */
	if (wrap_is_usb_bus(wnd->wd->dev_bus)) {
		wnd->attributes &= ~NDIS_ATTRIBUTE_NO_HALT_ON_SUSPEND;
		wnd->ndis_wolopts = 0;
	}
	mp_set_int(wnd, OID_802_11_POWER_MODE, NDIS_POWER_OFF);
	EXIT1(return NDIS_STATUS_SUCCESS);
}

/* MiniportHalt */
static void mp_halt(struct ndis_device *wnd)
{
	struct miniport *mp;

	ENTER1("%p", wnd);
	if (!test_and_clear_bit(HW_INITIALIZED, &wnd->wd->hw_status)) {
		WARNING("device %p is not initialized - not halting", wnd);
		return;
	}
	hangcheck_del(wnd);
	del_iw_stats_timer(wnd);
#ifdef CONFIG_WIRELESS_EXT
	if (wnd->physical_medium == NdisPhysicalMediumWirelessLan &&
	    wrap_is_pci_bus(wnd->wd->dev_bus)) {
		mutex_unlock(&wnd->ndis_req_mutex);
		disassociate(wnd, 0);
		mutex_lock(&wnd->ndis_req_mutex);
	}
#endif
	mp = &wnd->wd->driver->ndis_driver->mp;
	TRACE1("halt: %p", mp->mp_halt);
	LIN2WIN1(mp->mp_halt, wnd->nmb->mp_ctx);
	/* if a driver doesn't call NdisMDeregisterInterrupt during
	 * halt, deregister it now */
	if (wnd->mp_interrupt)
		NdisMDeregisterInterrupt(wnd->mp_interrupt);
	/* cancel any timers left by buggy windows driver; also free
	 * the memory for timers */
	while (1) {
		struct nt_slist *slist;
		struct wrap_timer *wrap_timer;

		spin_lock_bh(&ntoskernel_lock);
		if ((slist = wnd->wrap_timer_slist.next))
			wnd->wrap_timer_slist.next = slist->next;
		spin_unlock_bh(&ntoskernel_lock);
		TIMERTRACE("%p", slist);
		if (!slist)
			break;
		wrap_timer = container_of(slist, struct wrap_timer, slist);
		wrap_timer->repeat = 0;
		/* ktimer that this wrap_timer is associated to can't
		 * be touched, as it may have been freed by the driver
		 * already */
		if (del_timer_sync(&wrap_timer->timer))
			WARNING("Buggy Windows driver left timer %p "
				"running", wrap_timer->nt_timer);
		memset(wrap_timer, 0, sizeof(*wrap_timer));
		kfree(wrap_timer);
	}
	EXIT1(return);
}

static NDIS_STATUS mp_set_power_state(struct ndis_device *wnd,
				      enum ndis_power_state state)
{
	NDIS_STATUS status;

	TRACE1("%d", state);
	if (state == NdisDeviceStateD0) {
		status = NDIS_STATUS_SUCCESS;
		mutex_unlock(&wnd->ndis_req_mutex);
		if (test_and_clear_bit(HW_HALTED, &wnd->wd->hw_status)) {
			status = mp_init(wnd);
			if (status == NDIS_STATUS_SUCCESS) {
				set_packet_filter(wnd, wnd->packet_filter);
				set_multicast_list(wnd);
			}
		} else if (test_and_clear_bit(HW_SUSPENDED,
					      &wnd->wd->hw_status)) {
			status = mp_set_int(wnd, OID_PNP_SET_POWER, state);
			if (status != NDIS_STATUS_SUCCESS)
				WARNING("%s: setting power to state %d failed? "
					"%08X", wnd->net_dev->name, state,
					status);
		} else
			return NDIS_STATUS_FAILURE;

		if (wrap_is_pci_bus(wnd->wd->dev_bus)) {
			pci_enable_wake(wnd->wd->pci.pdev, PCI_D3hot, 0);
			pci_enable_wake(wnd->wd->pci.pdev, PCI_D3cold, 0);
		}
		if (status == NDIS_STATUS_SUCCESS) {
			mutex_unlock(&wnd->tx_ring_mutex);
			netif_device_attach(wnd->net_dev);
			hangcheck_add(wnd);
			add_iw_stats_timer(wnd);
		} else
			WARNING("%s: couldn't set power to state %d; device not"
				" resumed", wnd->net_dev->name, state);
		EXIT1(return status);
	} else {
		mutex_lock(&wnd->tx_ring_mutex);
		netif_device_detach(wnd->net_dev);
		hangcheck_del(wnd);
		del_iw_stats_timer(wnd);
		status = NDIS_STATUS_NOT_SUPPORTED;
		if (wnd->attributes & NDIS_ATTRIBUTE_NO_HALT_ON_SUSPEND) {
			status = mp_set_int(wnd, OID_PNP_ENABLE_WAKE_UP,
					    wnd->ndis_wolopts);
			TRACE2("0x%x, 0x%x", status, wnd->ndis_wolopts);
			if (status == NDIS_STATUS_SUCCESS &&
			    wrap_is_pci_bus(wnd->wd->dev_bus)) {
				if (wnd->ndis_wolopts)
					wnd->wd->pci.wake_state =
						PowerDeviceD3;
				else
					wnd->wd->pci.wake_state =
						PowerDeviceUnspecified;
			} else
				WARNING("couldn't set wake-on-lan options: "
					"0x%x, %08X", wnd->ndis_wolopts, status);
			status = mp_set_int(wnd, OID_PNP_SET_POWER, state);
			if (status == NDIS_STATUS_SUCCESS)
				set_bit(HW_SUSPENDED, &wnd->wd->hw_status);
			else
				WARNING("suspend failed: %08X", status);
		}
		if (status != NDIS_STATUS_SUCCESS) {
			WARNING("%s does not support power management; "
				"halting the device", wnd->net_dev->name);
			mp_halt(wnd);
			set_bit(HW_HALTED, &wnd->wd->hw_status);
			status = STATUS_SUCCESS;
		}
		mutex_lock(&wnd->ndis_req_mutex);
		EXIT1(return status);
	}
}

static int ndis_set_mac_address(struct net_device *dev, void *p)
{
	struct ndis_device *wnd = netdev_priv(dev);
	struct sockaddr *addr = p;
	struct ndis_configuration_parameter param;
	struct unicode_string key;
	struct ansi_string ansi;
	NDIS_STATUS res;
	unsigned char mac_string[2 * ETH_ALEN + 1];
	mac_address mac;

	memcpy(mac, addr->sa_data, sizeof(mac));
	memset(mac_string, 0, sizeof(mac_string));
	res = snprintf(mac_string, sizeof(mac_string), MACSTR, MAC2STR(mac));
	if (res != (sizeof(mac_string) - 1))
		EXIT1(return -EINVAL);
	TRACE1("new mac: %s", mac_string);

	RtlInitAnsiString(&ansi, mac_string);
	if (RtlAnsiStringToUnicodeString(&param.data.string, &ansi,
					 TRUE) != STATUS_SUCCESS)
		EXIT1(return -EINVAL);

	param.type = NdisParameterString;
	RtlInitAnsiString(&ansi, "NetworkAddress");
	if (RtlAnsiStringToUnicodeString(&key, &ansi, TRUE) != STATUS_SUCCESS) {
		RtlFreeUnicodeString(&param.data.string);
		EXIT1(return -EINVAL);
	}
	NdisWriteConfiguration(&res, wnd->nmb, &key, &param);
	RtlFreeUnicodeString(&key);
	RtlFreeUnicodeString(&param.data.string);

	if (res != NDIS_STATUS_SUCCESS)
		EXIT1(return -EFAULT);
	if (ndis_reinit(wnd) == NDIS_STATUS_SUCCESS) {
		res = mp_query(wnd, OID_802_3_CURRENT_ADDRESS,
			       mac, sizeof(mac));
		if (res == NDIS_STATUS_SUCCESS) {
			TRACE1("mac:" MACSTRSEP, MAC2STR(mac));
			memcpy(dev->dev_addr, mac, sizeof(mac));
		} else
			ERROR("couldn't get mac address: %08X", res);
	}
	EXIT1(return 0);
}

static int setup_tx_sg_list(struct ndis_device *wnd, struct sk_buff *skb,
			    struct ndis_packet_oob_data *oob_data)
{
	struct ndis_sg_element *sg_element;
	struct ndis_sg_list *sg_list;
	int i;

	ENTER3("%p, %d", skb, skb_shinfo(skb)->nr_frags);
	if (skb_shinfo(skb)->nr_frags <= 1) {
		sg_element = &oob_data->wrap_tx_sg_list.elements[0];
		sg_element->address =
			PCI_DMA_MAP_SINGLE(wnd->wd->pci.pdev, skb->data,
					   skb->len, PCI_DMA_TODEVICE);
		sg_element->length = skb->len;
		oob_data->wrap_tx_sg_list.nent = 1;
		oob_data->ext.info[ScatterGatherListPacketInfo] =
			&oob_data->wrap_tx_sg_list;
		TRACE3("%llx, %u", sg_element->address, sg_element->length);
		return 0;
	}
	sg_list = kmalloc(sizeof(*sg_list) +
			  (skb_shinfo(skb)->nr_frags + 1) * sizeof(*sg_element),
			  GFP_ATOMIC);
	if (!sg_list)
		return -ENOMEM;
	sg_list->nent = skb_shinfo(skb)->nr_frags + 1;
	TRACE3("%p, %d", sg_list, sg_list->nent);
	sg_element = sg_list->elements;
	sg_element->length = skb_headlen(skb);
	sg_element->address =
		PCI_DMA_MAP_SINGLE(wnd->wd->pci.pdev, skb->data,
				   skb_headlen(skb), PCI_DMA_TODEVICE);
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		sg_element++;
		sg_element->length = frag->size;
		sg_element->address =
			pci_map_page(wnd->wd->pci.pdev, skb_frag_page(frag),
				     frag->page_offset, frag->size,
				     PCI_DMA_TODEVICE);
		TRACE3("%llx, %u", sg_element->address, sg_element->length);
	}
	oob_data->ext.info[ScatterGatherListPacketInfo] = sg_list;
	return 0;
}

static void free_tx_sg_list(struct ndis_device *wnd,
			    struct ndis_packet_oob_data *oob_data)
{
	int i;
	struct ndis_sg_element *sg_element;
	struct ndis_sg_list *sg_list =
		oob_data->ext.info[ScatterGatherListPacketInfo];
	sg_element = sg_list->elements;
	TRACE3("%p, %d", sg_list, sg_list->nent);
	PCI_DMA_UNMAP_SINGLE(wnd->wd->pci.pdev, sg_element->address,
			     sg_element->length, PCI_DMA_TODEVICE);
	if (sg_list->nent == 1)
		EXIT3(return);
	for (i = 1; i < sg_list->nent; i++, sg_element++) {
		TRACE3("%llx, %u", sg_element->address, sg_element->length);
		pci_unmap_page(wnd->wd->pci.pdev, sg_element->address,
			       sg_element->length, PCI_DMA_TODEVICE);
	}
	TRACE3("%p", sg_list);
	kfree(sg_list);
}

static struct ndis_packet *alloc_tx_packet(struct ndis_device *wnd,
					   struct sk_buff *skb)
{
	struct ndis_packet *packet;
	ndis_buffer *buffer;
	struct ndis_packet_oob_data *oob_data;
	NDIS_STATUS status;

	NdisAllocatePacket(&status, &packet, wnd->tx_packet_pool);
	if (status != NDIS_STATUS_SUCCESS)
		return NULL;
	NdisAllocateBuffer(&status, &buffer, wnd->tx_buffer_pool,
			   skb->data, skb->len);
	if (status != NDIS_STATUS_SUCCESS) {
		NdisFreePacket(packet);
		return NULL;
	}
	packet->private.buffer_head = buffer;
	packet->private.buffer_tail = buffer;

	oob_data = NDIS_PACKET_OOB_DATA(packet);
	oob_data->tx_skb = skb;
	if (wnd->sg_dma_size) {
		if (setup_tx_sg_list(wnd, skb, oob_data)) {
			NdisFreeBuffer(buffer);
			NdisFreePacket(packet);
			return NULL;
		}
	}
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		struct ndis_tcp_ip_checksum_packet_info csum;
		int protocol;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,21)
		protocol = ntohs(skb->protocol);
#else
		protocol = skb->nh.iph->protocol;
#endif
		csum.value = 0;
		csum.tx.v4 = 1;
		if (protocol == IPPROTO_TCP)
			csum.tx.tcp = 1;
		else if (protocol == IPPROTO_UDP)
			csum.tx.udp = 1;
//		csum->tx.ip = 1;
		packet->private.flags |= NDIS_PROTOCOL_ID_TCP_IP;
		oob_data->ext.info[TcpIpChecksumPacketInfo] =
			(void *)(ULONG_PTR)csum.value;
	}
	DBG_BLOCK(4) {
		dump_bytes(__func__, skb->data, skb->len);
	}
	TRACE4("%p, %p, %p", packet, buffer, skb);
	return packet;
}

void free_tx_packet(struct ndis_device *wnd, struct ndis_packet *packet,
		    NDIS_STATUS status)
{
	ndis_buffer *buffer;
	struct ndis_packet_oob_data *oob_data;
	struct sk_buff *skb;
	struct ndis_packet_pool *pool;

	assert_irql(_irql_ <= DISPATCH_LEVEL);
	assert(packet->private.packet_flags);
	oob_data = NDIS_PACKET_OOB_DATA(packet);
	skb = oob_data->tx_skb;
	buffer = packet->private.buffer_head;
	TRACE4("%p, %p, %p, %08X", packet, buffer, skb, status);
	if (status == NDIS_STATUS_SUCCESS) {
		pre_atomic_add(wnd->net_stats.tx_bytes, packet->private.len);
		atomic_inc_var(wnd->net_stats.tx_packets);
	} else {
		TRACE1("packet dropped: %08X", status);
		atomic_inc_var(wnd->net_stats.tx_dropped);
	}
	if (wnd->sg_dma_size)
		free_tx_sg_list(wnd, oob_data);
	NdisFreeBuffer(buffer);
	dev_kfree_skb_any(skb);
	pool = packet->private.pool;
	NdisFreePacket(packet);
	if (netif_queue_stopped(wnd->net_dev) &&
	    ((pool->max_descr - pool->num_used_descr) >=
	     (wnd->max_tx_packets / 4))) {
		set_bit(NETIF_WAKEQ, &wnd->ndis_pending_work);
		queue_work(wrapndis_wq, &wnd->ndis_work);
	}
	EXIT4(return);
}

/* MiniportSend and MiniportSendPackets */
/* this function is called holding tx_ring_mutex. start and n are such
 * that start + n < TX_RING_SIZE; i.e., packets don't wrap around
 * ring */
static u8 mp_tx_packets(struct ndis_device *wnd, u8 start, u8 n)
{
	NDIS_STATUS res;
	struct miniport *mp;
	struct ndis_packet *packet;
	u8 sent;
	KIRQL irql;

	ENTER3("%d, %d", start, n);
	mp = &wnd->wd->driver->ndis_driver->mp;
	if (mp->send_packets) {
		if (deserialized_driver(wnd)) {
			LIN2WIN3(mp->send_packets, wnd->nmb->mp_ctx,
				 &wnd->tx_ring[start], n);
			sent = n;
		} else {
			irql = serialize_lock_irql(wnd);
			LIN2WIN3(mp->send_packets, wnd->nmb->mp_ctx,
				 &wnd->tx_ring[start], n);
			serialize_unlock_irql(wnd, irql);
			for (sent = 0; sent < n && wnd->tx_ok; sent++) {
				struct ndis_packet_oob_data *oob_data;
				packet = wnd->tx_ring[start + sent];
				oob_data = NDIS_PACKET_OOB_DATA(packet);
				switch ((res =
					 xchg(&oob_data->status,
					      NDIS_STATUS_NOT_RECOGNIZED))) {
				case NDIS_STATUS_SUCCESS:
					free_tx_packet(wnd, packet,
						       NDIS_STATUS_SUCCESS);
					break;
				case NDIS_STATUS_PENDING:
					break;
				case NDIS_STATUS_RESOURCES:
					wnd->tx_ok = 0;
					/* resubmit this packet and
					 * the rest when resources
					 * become available */
					sent--;
					break;
				case NDIS_STATUS_FAILURE:
					free_tx_packet(wnd, packet,
						       NDIS_STATUS_FAILURE);
					break;
				default:
					ERROR("%p: invalid status: %08X",
					      packet, res);
					free_tx_packet(wnd, packet,
						       oob_data->status);
					break;
				}
				TRACE3("%p, %d", packet, res);
			}
		}
		TRACE3("sent: %d(%d)", sent, n);
	} else {
		for (sent = 0; sent < n && wnd->tx_ok; sent++) {
			struct ndis_packet_oob_data *oob_data;
			packet = wnd->tx_ring[start + sent];
			oob_data = NDIS_PACKET_OOB_DATA(packet);
			oob_data->status = NDIS_STATUS_NOT_RECOGNIZED;
			irql = serialize_lock_irql(wnd);
			res = LIN2WIN3(mp->send, wnd->nmb->mp_ctx,
				       packet, packet->private.flags);
			serialize_unlock_irql(wnd, irql);
			switch (res) {
			case NDIS_STATUS_SUCCESS:
				free_tx_packet(wnd, packet, res);
				break;
			case NDIS_STATUS_PENDING:
				break;
			case NDIS_STATUS_RESOURCES:
				wnd->tx_ok = 0;
				/* resend this packet when resources
				 * become available */
				sent--;
				break;
			case NDIS_STATUS_FAILURE:
				free_tx_packet(wnd, packet, res);
				break;
			default:
				ERROR("packet %p: invalid status: %08X",
				      packet, res);
				break;
			}
		}
	}
	EXIT3(return sent);
}

static void tx_worker(struct work_struct *work)
{
	struct ndis_device *wnd;
	s8 n;

	wnd = container_of(work, struct ndis_device, tx_work);
	ENTER3("tx_ok %d", wnd->tx_ok);
	while (wnd->tx_ok) {
		mutex_lock(&wnd->tx_ring_mutex);
		spin_lock_bh(&wnd->tx_ring_lock);
		n = wnd->tx_ring_end - wnd->tx_ring_start;
		TRACE3("%d, %d, %d", wnd->tx_ring_start, wnd->tx_ring_end, n);
		/* end == start if either ring is empty or full; in
		 * the latter case is_tx_ring_full is set */
		if (n == 0) {
			if (wnd->is_tx_ring_full)
				n = TX_RING_SIZE - wnd->tx_ring_start;
			else {
				spin_unlock_bh(&wnd->tx_ring_lock);
				mutex_unlock(&wnd->tx_ring_mutex);
				break;
			}
		} else if (n < 0)
			n = TX_RING_SIZE - wnd->tx_ring_start;
		spin_unlock_bh(&wnd->tx_ring_lock);
		if (unlikely(n > wnd->max_tx_packets))
			n = wnd->max_tx_packets;
		n = mp_tx_packets(wnd, wnd->tx_ring_start, n);
		if (n) {
			netif_trans_update(wnd->net_dev);
			wnd->tx_ring_start =
				(wnd->tx_ring_start + n) % TX_RING_SIZE;
			wnd->is_tx_ring_full = 0;
		}
		mutex_unlock(&wnd->tx_ring_mutex);
		TRACE3("%d, %d, %d", wnd->tx_ring_start, wnd->tx_ring_end, n);
	}
	EXIT3(return);
}

static int tx_skbuff(struct sk_buff *skb, struct net_device *dev)
{
	struct ndis_device *wnd = netdev_priv(dev);
	struct ndis_packet *packet;

	packet = alloc_tx_packet(wnd, skb);
	if (!packet) {
		TRACE2("couldn't allocate packet");
		netif_tx_lock(dev);
		netif_stop_queue(dev);
		netif_tx_unlock(dev);
		return NETDEV_TX_BUSY;
	}
	spin_lock(&wnd->tx_ring_lock);
	wnd->tx_ring[wnd->tx_ring_end++] = packet;
	if (wnd->tx_ring_end == TX_RING_SIZE)
		wnd->tx_ring_end = 0;
	if (wnd->tx_ring_end == wnd->tx_ring_start) {
		netif_tx_lock(dev);
		wnd->is_tx_ring_full = 1;
		netif_stop_queue(dev);
		netif_tx_unlock(dev);
	}
	spin_unlock(&wnd->tx_ring_lock);
	TRACE4("ring: %d, %d", wnd->tx_ring_start, wnd->tx_ring_end);
	queue_work(wrapndis_wq, &wnd->tx_work);
	return NETDEV_TX_OK;
}

static int set_packet_filter(struct ndis_device *wnd, ULONG packet_filter)
{
	NDIS_STATUS res;

	while (1) {
		res = mp_set_int(wnd, OID_GEN_CURRENT_PACKET_FILTER,
				 packet_filter);
		if (res == NDIS_STATUS_SUCCESS)
			break;
		TRACE2("couldn't set filter 0x%08x", packet_filter);
		/* NDIS_PACKET_TYPE_PROMISCUOUS may not work with 802.11 */
		if (packet_filter & NDIS_PACKET_TYPE_PROMISCUOUS) {
			packet_filter &= ~NDIS_PACKET_TYPE_PROMISCUOUS;
			continue;
		}
		if (packet_filter & NDIS_PACKET_TYPE_ALL_LOCAL) {
			packet_filter &= ~NDIS_PACKET_TYPE_ALL_LOCAL;
			continue;
		}
		if (packet_filter & NDIS_PACKET_TYPE_ALL_FUNCTIONAL) {
			packet_filter &= ~NDIS_PACKET_TYPE_ALL_FUNCTIONAL;
			continue;
		}
		if (packet_filter & NDIS_PACKET_TYPE_MULTICAST) {
			packet_filter &= ~NDIS_PACKET_TYPE_MULTICAST;
			packet_filter |= NDIS_PACKET_TYPE_ALL_MULTICAST;
			continue;
		}
		if (packet_filter & NDIS_PACKET_TYPE_ALL_MULTICAST) {
			packet_filter &= ~NDIS_PACKET_TYPE_ALL_MULTICAST;
			continue;
		}
		break;
	}

	wnd->packet_filter = packet_filter;
	res = mp_query_int(wnd, OID_GEN_CURRENT_PACKET_FILTER, &packet_filter);
	if (packet_filter != wnd->packet_filter) {
		WARNING("filter not set: 0x%08x, 0x%08x",
			packet_filter, wnd->packet_filter);
		wnd->packet_filter = packet_filter;
	}
	if (wnd->packet_filter)
		EXIT3(return 0);
	else
		EXIT3(return -1);
}

void set_media_state(struct ndis_device *wnd, enum ndis_media_state state)
{
	struct net_device *net_dev = wnd->net_dev;

	ENTER2("state: 0x%x, carrier %d", state, netif_carrier_ok(net_dev));
	switch (state) {
	case NdisMediaStateConnected:
		if (netif_carrier_ok(net_dev))
			return;
		netif_carrier_on(net_dev);
		wnd->tx_ok = 1;
		if (netif_queue_stopped(net_dev))
			netif_wake_queue(net_dev);
		if (wnd->physical_medium == NdisPhysicalMediumWirelessLan) {
			set_bit(LINK_STATUS_ON, &wnd->ndis_pending_work);
			queue_work(wrapndis_wq, &wnd->ndis_work);
		}
		break;
	case NdisMediaStateDisconnected:
		if (!netif_carrier_ok(net_dev))
			return;
		netif_carrier_off(net_dev);
		netif_stop_queue(net_dev);
		wnd->tx_ok = 0;
		if (wnd->physical_medium == NdisPhysicalMediumWirelessLan) {
			memset(&wnd->essid, 0, sizeof(wnd->essid));
			set_bit(LINK_STATUS_OFF, &wnd->ndis_pending_work);
			queue_work(wrapndis_wq, &wnd->ndis_work);
		}
		break;
	default:
		WARNING("invalid media state: 0x%x", state);
		break;
	}
}

static int ndis_net_dev_init(struct net_device *net_dev)
{
	struct ndis_device *wnd = netdev_priv(net_dev);

	ENTER1("%p", wnd);
	wrap_procfs_add_ndis_device(wnd);
	EXIT1(return 0);
}

static void ndis_net_dev_uninit(struct net_device *net_dev)
{
	struct ndis_device *wnd = netdev_priv(net_dev);

	ENTER1("%p", wnd);
	wrap_procfs_remove_ndis_device(wnd);
	EXIT1(return);
}

static int ndis_net_dev_open(struct net_device *net_dev)
{
	int status, res;
	struct ndis_device *wnd = netdev_priv(net_dev);

	ENTER1("%p", wnd);
	res = mp_query_int(wnd, OID_GEN_MEDIA_CONNECT_STATUS, &status);
	if (res == NDIS_STATUS_SUCCESS && status >= NdisMediaStateConnected &&
	    status <= NdisMediaStateDisconnected)
		set_media_state(wnd, status);
	netif_start_queue(net_dev);
	netif_poll_enable(net_dev);
	EXIT1(return 0);
}

static int ndis_net_dev_close(struct net_device *net_dev)
{
	ENTER1("%p", netdev_priv(net_dev));
	netif_poll_disable(net_dev);
	netif_tx_disable(net_dev);
	EXIT1(return 0);
}

static int ndis_change_mtu(struct net_device *net_dev, int mtu)
{
	struct ndis_device *wnd = netdev_priv(net_dev);
	int max;

	if (mtu < ETH_ZLEN)
		return -EINVAL;
	if (mp_query_int(wnd, OID_GEN_MAXIMUM_TOTAL_SIZE, &max) !=
	    NDIS_STATUS_SUCCESS)
		return -EOPNOTSUPP;
	TRACE1("%d", max);
	max -= ETH_HLEN;
	if (max <= ETH_ZLEN)
		return -EINVAL;
	if (mtu + ETH_HLEN > max)
		return -EINVAL;
	net_dev->mtu = mtu;
	return 0;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void ndis_poll_controller(struct net_device *dev)
{
	struct ndis_device *wnd = netdev_priv(dev);

	disable_irq(dev->irq);
	ndis_isr(wnd->mp_interrupt->kinterrupt, wnd->mp_interrupt);
	enable_irq(dev->irq);
}
#endif

/* called from BH context */
static struct net_device_stats *ndis_get_stats(struct net_device *dev)
{
	struct ndis_device *wnd = netdev_priv(dev);
	return &wnd->net_stats;
}

/* called from BH context */
static void ndis_set_multicast_list(struct net_device *dev)
{
	struct ndis_device *wnd = netdev_priv(dev);
	set_bit(SET_MULTICAST_LIST, &wnd->ndis_pending_work);
	queue_work(wrapndis_wq, &wnd->ndis_work);
}

/* called from BH context */
struct iw_statistics *get_iw_stats(struct net_device *dev)
{
	struct ndis_device *wnd = netdev_priv(dev);
	return &wnd->iw_stats;
}

static void update_iw_stats(struct ndis_device *wnd)
{
	struct iw_statistics *iw_stats = &wnd->iw_stats;
	struct ndis_wireless_stats ndis_stats;
	NDIS_STATUS res;
	ndis_rssi rssi;
	int qual;

	ENTER2("%p", wnd);
	if (wnd->iw_stats_enabled == FALSE || !netif_carrier_ok(wnd->net_dev)) {
		memset(iw_stats, 0, sizeof(*iw_stats));
		EXIT2(return);
	}
	res = mp_query(wnd, OID_802_11_RSSI, &rssi, sizeof(rssi));
	if (res == NDIS_STATUS_SUCCESS)
		iw_stats->qual.level = rssi;

	qual = 100 * (rssi - WL_NOISE) / (WL_SIGMAX - WL_NOISE);
	if (qual < 0)
		qual = 0;
	else if (qual > 100)
		qual = 100;

	iw_stats->qual.noise = WL_NOISE;
	iw_stats->qual.qual = qual;

	res = mp_query(wnd, OID_802_11_STATISTICS,
		       &ndis_stats, sizeof(ndis_stats));
	if (res != NDIS_STATUS_SUCCESS)
		EXIT2(return);
	iw_stats->discard.retries = (unsigned long)ndis_stats.retry +
		(unsigned long)ndis_stats.multi_retry;
	iw_stats->discard.misc = (unsigned long)ndis_stats.fcs_err +
		(unsigned long)ndis_stats.rtss_fail +
		(unsigned long)ndis_stats.ack_fail +
		(unsigned long)ndis_stats.frame_dup;

	EXIT2(return);
}

static void set_multicast_list(struct ndis_device *wnd)
{
	struct net_device *net_dev;
	ULONG packet_filter;
	NDIS_STATUS res;

	net_dev = wnd->net_dev;
	packet_filter = wnd->packet_filter;

	TRACE2("0x%08x", packet_filter);
	if (net_dev->flags & IFF_PROMISC) {
		packet_filter |= NDIS_PACKET_TYPE_PROMISCUOUS |
			NDIS_PACKET_TYPE_ALL_LOCAL;
	} else if (net_dev->flags & IFF_ALLMULTI ||
		   netdev_mc_count(net_dev) > wnd->multicast_size) {
		packet_filter |= NDIS_PACKET_TYPE_ALL_MULTICAST;
		TRACE2("0x%08x", packet_filter);
	} else if (netdev_mc_count(net_dev) > 0) {
		int i, size;
		char *buf;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,35)
		struct netdev_hw_addr *ha;
#else
		struct dev_mc_list *mclist;
#endif
		size = min(wnd->multicast_size, netdev_mc_count(net_dev));
		TRACE2("%d, %d", wnd->multicast_size, netdev_mc_count(net_dev));
		buf = kmalloc(size * ETH_ALEN, GFP_KERNEL);
		if (!buf) {
			WARNING("couldn't allocate memory");
			EXIT2(return);
		}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,35)
		i = 0;
		netdev_for_each_mc_addr(ha, net_dev) {
			if (i >= size)
				break;
			memcpy(buf + i * ETH_ALEN, ha->addr, ETH_ALEN);
			TRACE2(MACSTRSEP, MAC2STR(ha->addr));
			i++;
		}
#else
		mclist = net_dev->mc_list;
		for (i = 0; i < size && mclist; mclist = mclist->next) {
			if (mclist->dmi_addrlen != ETH_ALEN)
				continue;
			memcpy(buf + i * ETH_ALEN, mclist->dmi_addr, ETH_ALEN);
			TRACE2(MACSTRSEP, MAC2STR(mclist->dmi_addr));
			i++;
		}
#endif
		res = mp_set(wnd, OID_802_3_MULTICAST_LIST, buf, i * ETH_ALEN);
		if (res == NDIS_STATUS_SUCCESS && i > 0)
			packet_filter |= NDIS_PACKET_TYPE_MULTICAST;
		else
			packet_filter |= NDIS_PACKET_TYPE_ALL_MULTICAST;
		kfree(buf);
	}
	TRACE2("0x%08x", packet_filter);
	res = set_packet_filter(wnd, packet_filter);
	if (res)
		TRACE1("couldn't set packet filter (%08X)", res);
	EXIT2(return);
}

static void link_status_off(struct ndis_device *wnd)
{
#ifdef CONFIG_WIRELESS_EXT
	union iwreq_data wrqu;

	memset(&wrqu, 0, sizeof(wrqu));
	wrqu.ap_addr.sa_family = ARPHRD_ETHER;
	wireless_send_event(wnd->net_dev, SIOCGIWAP, &wrqu, NULL);
#endif
	EXIT2(return);
}

static void link_status_on(struct ndis_device *wnd)
{
#ifdef CONFIG_WIRELESS_EXT
	struct ndis_assoc_info *ndis_assoc_info;
	union iwreq_data wrqu;
	NDIS_STATUS res;
	const int assoc_size = sizeof(*ndis_assoc_info) + IW_CUSTOM_MAX + 32;
#endif

	ENTER2("");
#ifdef CONFIG_WIRELESS_EXT
	memset(&wrqu, 0, sizeof(wrqu));
	ndis_assoc_info = kzalloc(assoc_size, GFP_KERNEL);
	if (!ndis_assoc_info) {
		ERROR("couldn't allocate memory");
		goto send_assoc_event;
	}
	res = mp_query(wnd, OID_802_11_ASSOCIATION_INFORMATION,
		       ndis_assoc_info, assoc_size);
	if (res) {
		TRACE2("query assoc_info failed (%08X)", res);
		kfree(ndis_assoc_info);
		goto send_assoc_event;
	}
	TRACE2("%u, 0x%x, %u, 0x%x, %u", ndis_assoc_info->length,
	       ndis_assoc_info->req_ies, ndis_assoc_info->req_ie_length,
	       ndis_assoc_info->resp_ies, ndis_assoc_info->resp_ie_length);
	if (ndis_assoc_info->req_ie_length > 0) {
		wrqu.data.length = ndis_assoc_info->req_ie_length;
		wireless_send_event(wnd->net_dev, IWEVASSOCREQIE, &wrqu,
				    ((char *)ndis_assoc_info) +
				    ndis_assoc_info->offset_req_ies);
	}
	if (ndis_assoc_info->resp_ie_length > 0) {
		wrqu.data.length = ndis_assoc_info->resp_ie_length;
		wireless_send_event(wnd->net_dev, IWEVASSOCRESPIE, &wrqu,
				    ((char *)ndis_assoc_info) +
				    ndis_assoc_info->offset_resp_ies);
	}
	kfree(ndis_assoc_info);

send_assoc_event:
	get_ap_address(wnd, wrqu.ap_addr.sa_data);
	wrqu.ap_addr.sa_family = ARPHRD_ETHER;
	TRACE2(MACSTRSEP, MAC2STR(wrqu.ap_addr.sa_data));
	wireless_send_event(wnd->net_dev, SIOCGIWAP, &wrqu, NULL);
#endif
	EXIT2(return);
}

static void iw_stats_timer_proc(unsigned long data)
{
	struct ndis_device *wnd = (struct ndis_device *)data;

	ENTER2("%d", wnd->iw_stats_interval);
	if (wnd->iw_stats_interval > 0) {
		set_bit(COLLECT_IW_STATS, &wnd->ndis_pending_work);
		queue_work(wrapndis_wq, &wnd->ndis_work);
	}
	mod_timer(&wnd->iw_stats_timer, jiffies + wnd->iw_stats_interval);
}

static void add_iw_stats_timer(struct ndis_device *wnd)
{
	if (wnd->physical_medium != NdisPhysicalMediumWirelessLan)
		return;
	if (wnd->iw_stats_interval < 0)
		wnd->iw_stats_interval *= -1;
	wnd->iw_stats_timer.data = (unsigned long)wnd;
	wnd->iw_stats_timer.function = iw_stats_timer_proc;
	mod_timer(&wnd->iw_stats_timer, jiffies + wnd->iw_stats_interval);
}

static void del_iw_stats_timer(struct ndis_device *wnd)
{
	ENTER2("%d", wnd->iw_stats_interval);
	wnd->iw_stats_interval *= -1;
	del_timer_sync(&wnd->iw_stats_timer);
	EXIT2(return);
}

static void hangcheck_proc(unsigned long data)
{
	struct ndis_device *wnd = (struct ndis_device *)data;

	ENTER3("%d", wnd->hangcheck_interval);
	if (wnd->hangcheck_interval > 0) {
		set_bit(HANGCHECK, &wnd->ndis_pending_work);
		queue_work(wrapndis_wq, &wnd->ndis_work);
	}
	mod_timer(&wnd->hangcheck_timer, jiffies + wnd->hangcheck_interval);
	EXIT3(return);
}

void hangcheck_add(struct ndis_device *wnd)
{
	if (!wnd->wd->driver->ndis_driver->mp.hangcheck ||
	    hangcheck_interval < 0)
		EXIT2(return);

	if (hangcheck_interval > 0)
		wnd->hangcheck_interval = hangcheck_interval * HZ;
	if (wnd->hangcheck_interval < 0)
		wnd->hangcheck_interval *= -1;
	wnd->hangcheck_timer.data = (unsigned long)wnd;
	wnd->hangcheck_timer.function = hangcheck_proc;
	mod_timer(&wnd->hangcheck_timer, jiffies + wnd->hangcheck_interval);
	EXIT2(return);
}

void hangcheck_del(struct ndis_device *wnd)
{
	ENTER2("%d", wnd->hangcheck_interval);
	if (wnd->hangcheck_interval > 0)
		wnd->hangcheck_interval *= -1;
	del_timer_sync(&wnd->hangcheck_timer);
	EXIT2(return);
}

/* worker procedure to take care of setting/checking various states */
static void wrapndis_worker(struct work_struct *work)
{
	struct ndis_device *wnd;

	wnd = container_of(work, struct ndis_device, ndis_work);
	WORKTRACE("0x%lx", wnd->ndis_pending_work);

	if (test_and_clear_bit(NETIF_WAKEQ, &wnd->ndis_pending_work)) {
		netif_tx_lock_bh(wnd->net_dev);
		netif_wake_queue(wnd->net_dev);
		netif_tx_unlock_bh(wnd->net_dev);
	}

	if (test_and_clear_bit(LINK_STATUS_OFF, &wnd->ndis_pending_work))
		link_status_off(wnd);

	if (test_and_clear_bit(LINK_STATUS_ON, &wnd->ndis_pending_work))
		link_status_on(wnd);

	if (test_and_clear_bit(COLLECT_IW_STATS, &wnd->ndis_pending_work))
		update_iw_stats(wnd);

	if (test_and_clear_bit(SET_MULTICAST_LIST,
			       &wnd->ndis_pending_work))
		set_multicast_list(wnd);

	if (test_and_clear_bit(HANGCHECK, &wnd->ndis_pending_work)) {
		struct miniport *mp;
		BOOLEAN reset;
		KIRQL irql;

		mp = &wnd->wd->driver->ndis_driver->mp;
		irql = serialize_lock_irql(wnd);
		reset = LIN2WIN1(mp->hangcheck, wnd->nmb->mp_ctx);
		serialize_unlock_irql(wnd, irql);
		if (reset) {
			TRACE2("%s needs reset", wnd->net_dev->name);
			mp_reset(wnd);
		}
	}
	WORKEXIT(return);
}

NDIS_STATUS ndis_reinit(struct ndis_device *wnd)
{
	NDIS_STATUS status;

	wnd->attributes &= ~NDIS_ATTRIBUTE_NO_HALT_ON_SUSPEND;
	status = mp_set_power_state(wnd, NdisDeviceStateD3);
	if (status != NDIS_STATUS_SUCCESS) {
		ERROR("halting device %s failed: %08X", wnd->net_dev->name,
		      status);
		return status;
	}
	status = mp_set_power_state(wnd, NdisDeviceStateD0);
	if (status != NDIS_STATUS_SUCCESS)
		ERROR("starting device %s failed: %08X", wnd->net_dev->name,
		      status);
	return status;
}

#ifdef CONFIG_WIRELESS_EXT
static void get_encryption_capa(struct ndis_device *wnd, char *buf,
				const int buf_len)
{
	int i, mode;
	NDIS_STATUS res;
	struct ndis_assoc_info ndis_assoc_info;
	struct ndis_add_key ndis_key;
	struct ndis_capability *c;

	ENTER1("%p", wnd);
	/* set network type to g, b, or a, in that order */
	res = mp_query(wnd, OID_802_11_NETWORK_TYPES_SUPPORTED, buf, buf_len);
	if (res == NDIS_STATUS_SUCCESS) {
		struct network_type_list *net_types;
		unsigned long types = 0;
		net_types = (typeof(net_types))buf;
		for (i = 0; i < net_types->num; i++) {
			TRACE2("%d", net_types->types[i]);
			set_bit(net_types->types[i], &types);
		}
		if (test_bit(Ndis802_11OFDM24, &types))
			mode = Ndis802_11OFDM24;
		else if (test_bit(Ndis802_11DS, &types))
			mode = Ndis802_11DS;
		else if (test_bit(Ndis802_11OFDM5, &types))
			mode = Ndis802_11OFDM5;
		else
			mode = Ndis802_11DS;
		mp_set_int(wnd, OID_802_11_NETWORK_TYPE_IN_USE, mode);
	}
	/* check if WEP is supported */
	if (set_iw_encr_mode(wnd, IW_AUTH_CIPHER_WEP104,
			     IW_AUTH_CIPHER_NONE) == 0 &&
	    get_ndis_encr_mode(wnd) == Ndis802_11Encryption1KeyAbsent)
		set_bit(Ndis802_11Encryption1Enabled, &wnd->capa.encr);

	/* check if WPA is supported */
	if (set_ndis_auth_mode(wnd, Ndis802_11AuthModeWPA) == 0 &&
	    get_ndis_auth_mode(wnd) == Ndis802_11AuthModeWPA)
		set_bit(Ndis802_11AuthModeWPA, &wnd->capa.encr);
	else
		EXIT1(return);

	if (set_ndis_auth_mode(wnd, Ndis802_11AuthModeWPAPSK) == 0 &&
	    get_ndis_auth_mode(wnd) == Ndis802_11AuthModeWPAPSK)
		set_bit(Ndis802_11AuthModeWPAPSK, &wnd->capa.encr);

	/* check for highest encryption */
	mode = 0;
	if (set_iw_encr_mode(wnd, IW_AUTH_CIPHER_CCMP,
			     IW_AUTH_CIPHER_NONE) == 0 &&
	    (i = get_ndis_encr_mode(wnd)) > 0 &&
	    (i == Ndis802_11Encryption3KeyAbsent ||
	     i == Ndis802_11Encryption3Enabled))
		mode = Ndis802_11Encryption3Enabled;
	else if (set_iw_encr_mode(wnd, IW_AUTH_CIPHER_TKIP,
				  IW_AUTH_CIPHER_NONE) == 0 &&
		 (i = get_ndis_encr_mode(wnd)) > 0 &&
		 (i == Ndis802_11Encryption2KeyAbsent ||
		  i == Ndis802_11Encryption2Enabled))
		mode = Ndis802_11Encryption2Enabled;
	else if (set_iw_encr_mode(wnd, IW_AUTH_CIPHER_WEP104,
				  IW_AUTH_CIPHER_NONE) == 0 &&
		 (i = get_ndis_encr_mode(wnd)) > 0 &&
		 (i == Ndis802_11Encryption1KeyAbsent ||
		  i == Ndis802_11Encryption1Enabled))
		mode = Ndis802_11Encryption1Enabled;

	TRACE1("mode: %d", mode);
	if (mode == 0)
		EXIT1(return);
	set_bit(Ndis802_11Encryption1Enabled, &wnd->capa.encr);
	if (mode == Ndis802_11Encryption1Enabled)
		EXIT1(return);

	ndis_key.length = 32;
	ndis_key.index = 0xC0000001;
	ndis_key.struct_size = sizeof(ndis_key);
	res = mp_set(wnd, OID_802_11_ADD_KEY, &ndis_key, ndis_key.struct_size);
	TRACE2("%08X, %zu", res, sizeof(ndis_key));
	if (res && res != NDIS_STATUS_INVALID_DATA)
		EXIT1(return);
	res = mp_query(wnd, OID_802_11_ASSOCIATION_INFORMATION,
		       &ndis_assoc_info, sizeof(ndis_assoc_info));
	TRACE1("%08X", res);
	if (res == NDIS_STATUS_NOT_SUPPORTED)
		EXIT1(return);

	set_bit(Ndis802_11Encryption2Enabled, &wnd->capa.encr);
	if (mode == Ndis802_11Encryption3Enabled)
		set_bit(Ndis802_11Encryption3Enabled, &wnd->capa.encr);
	/* not all drivers support OID_802_11_CAPABILITY, so we don't
	 * know for sure if driver support WPA or WPAPSK; assume
	 * WPAPSK */
	set_bit(Ndis802_11AuthModeWPAPSK, &wnd->capa.auth);
	wnd->max_pmkids = 1;

	memset(buf, 0, buf_len);
	c = (struct ndis_capability *)buf;
	res = mp_query(wnd, OID_802_11_CAPABILITY, buf, buf_len);
	if (!(res == NDIS_STATUS_SUCCESS && c->version == 2))
		EXIT1(return);
	wnd->max_pmkids = c->num_PMKIDs;

	for (i = 0; i < c->num_auth_encr_pair; i++) {
		struct ndis_auth_encr_pair *ae;

		ae = &c->auth_encr_pair[i];
		if ((char *)(ae + 1) > buf + buf_len)
			break;
		switch (ae->auth_mode) {
		case Ndis802_11AuthModeOpen:
		case Ndis802_11AuthModeShared:
		case Ndis802_11AuthModeWPA:
		case Ndis802_11AuthModeWPAPSK:
		case Ndis802_11AuthModeWPANone:
		case Ndis802_11AuthModeWPA2:
		case Ndis802_11AuthModeWPA2PSK:
			set_bit(ae->auth_mode, &wnd->capa.auth);
			break;
		default:
			WARNING("unknown auth_mode: %d", ae->auth_mode);
			break;
		}
		switch (ae->encr_mode) {
		case Ndis802_11EncryptionDisabled:
		case Ndis802_11Encryption1Enabled:
		case Ndis802_11Encryption2Enabled:
		case Ndis802_11Encryption3Enabled:
			set_bit(ae->encr_mode, &wnd->capa.encr);
			break;
		default:
			WARNING("unknown encr_mode: %d", ae->encr_mode);
			break;
		}
	}
	EXIT1(return);
}
#endif

wstdcall NTSTATUS NdisDispatchDeviceControl(struct device_object *fdo,
					    struct irp *irp)
{
	struct ndis_device *wnd;

	TRACE3("fdo: %p", fdo);
	/* for now, we don't have anything interesting here, so pass it
	 * down to bus driver */
	wnd = fdo->reserved;
	return IoPassIrpDown(wnd->nmb->pdo, irp);
}
WIN_FUNC_DECL(NdisDispatchDeviceControl,2)

wstdcall NTSTATUS NdisDispatchPower(struct device_object *fdo, struct irp *irp)
{
	struct io_stack_location *irp_sl;
	struct ndis_device *wnd;
	enum ndis_power_state state;
	NTSTATUS status;
	NDIS_STATUS ndis_status;

	irp_sl = IoGetCurrentIrpStackLocation(irp);
	wnd = fdo->reserved;
	IOTRACE("fdo: %p, fn: %d:%d, wnd: %p", fdo, irp_sl->major_fn,
		irp_sl->minor_fn, wnd);
	if ((irp_sl->params.power.type == SystemPowerState &&
	     irp_sl->params.power.state.system_state > PowerSystemWorking) ||
	    (irp_sl->params.power.type == DevicePowerState &&
	     irp_sl->params.power.state.device_state > PowerDeviceD0))
		state = NdisDeviceStateD3;
	else
		state = NdisDeviceStateD0;
	switch (irp_sl->minor_fn) {
	case IRP_MN_SET_POWER:
		if (state == NdisDeviceStateD0) {
			status = IoSyncForwardIrp(wnd->nmb->pdo, irp);
			if (status != STATUS_SUCCESS)
				break;
			ndis_status = mp_set_power_state(wnd, state);
			if (ndis_status != NDIS_STATUS_SUCCESS)
				WARNING("couldn't set power to %d: %08X",
					state, ndis_status);
			TRACE2("%s: device resumed", wnd->net_dev->name);
			irp->io_status.status = status = STATUS_SUCCESS;
			IoCompleteRequest(irp, IO_NO_INCREMENT);
			break;
		} else {
			ndis_status = mp_set_power_state(wnd, state);
			/* TODO: handle error case */
			if (ndis_status != NDIS_STATUS_SUCCESS)
				WARNING("setting power to %d failed: %08X",
					state, ndis_status);
			status = IoAsyncForwardIrp(wnd->nmb->pdo, irp);
		}
		break;
	case IRP_MN_QUERY_POWER:
		if (wnd->attributes & NDIS_ATTRIBUTE_NO_HALT_ON_SUSPEND) {
			ndis_status = mp_query(wnd, OID_PNP_QUERY_POWER,
					       &state, sizeof(state));
			TRACE2("%d, %08X", state, ndis_status);
			/* this OID must always succeed */
			if (ndis_status != NDIS_STATUS_SUCCESS)
				TRACE1("query power returns %08X", ndis_status);
			irp->io_status.status = STATUS_SUCCESS;
		} else
			irp->io_status.status = STATUS_SUCCESS;
		status = IoPassIrpDown(wnd->nmb->pdo, irp);
		break;
	case IRP_MN_WAIT_WAKE:
	case IRP_MN_POWER_SEQUENCE:
		/* TODO: implement WAIT_WAKE */
		status = IoPassIrpDown(wnd->nmb->pdo, irp);
		break;
	default:
		status = IoPassIrpDown(wnd->nmb->pdo, irp);
		break;
	}
	IOEXIT(return status);
}
WIN_FUNC_DECL(NdisDispatchPower,2)

wstdcall NTSTATUS NdisDispatchPnp(struct device_object *fdo, struct irp *irp)
{
	struct io_stack_location *irp_sl;
	struct ndis_device *wnd;
	struct device_object *pdo;
	NTSTATUS status;

	IOTRACE("fdo: %p, irp: %p", fdo, irp);
	irp_sl = IoGetCurrentIrpStackLocation(irp);
	wnd = fdo->reserved;
	pdo = wnd->nmb->pdo;
	switch (irp_sl->minor_fn) {
	case IRP_MN_START_DEVICE:
		status = IoSyncForwardIrp(pdo, irp);
		if (status != STATUS_SUCCESS)
			break;
		if (ndis_start_device(wnd) == NDIS_STATUS_SUCCESS)
			status = STATUS_SUCCESS;
		else
			status = STATUS_FAILURE;
		irp->io_status.status = status;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		break;
	case IRP_MN_QUERY_STOP_DEVICE:
		/* TODO: implement in NDIS */
		status = IoPassIrpDown(wnd->nmb->pdo, irp);
		break;
	case IRP_MN_STOP_DEVICE:
		mp_halt(wnd);
		irp->io_status.status = STATUS_SUCCESS;
		status = IoAsyncForwardIrp(pdo, irp);
		break;
	case IRP_MN_REMOVE_DEVICE:
		TRACE1("%s", wnd->net_dev->name);
		mp_pnp_event(wnd, NdisDevicePnPEventSurpriseRemoved, 0);
		if (ndis_remove_device(wnd)) {
			status = STATUS_FAILURE;
			break;
		}
		/* wnd is already freed */
		status = IoAsyncForwardIrp(pdo, irp);
		IoDetachDevice(fdo);
		IoDeleteDevice(fdo);
		break;
	default:
		status = IoAsyncForwardIrp(pdo, irp);
		break;
	}
	IOTRACE("status: %08X", status);
	IOEXIT(return status);
}
WIN_FUNC_DECL(NdisDispatchPnp,2)

static void set_task_offload(struct ndis_device *wnd, void *buf,
			     const int buf_size)
{
	struct ndis_task_offload_header *task_offload_header;
	struct ndis_task_offload *task_offload;
	struct ndis_task_tcp_ip_checksum *csum = NULL;
	struct ndis_task_tcp_large_send *tso = NULL;
	NDIS_STATUS status;

	memset(buf, 0, buf_size);
	task_offload_header = buf;
	task_offload_header->version = NDIS_TASK_OFFLOAD_VERSION;
	task_offload_header->size = sizeof(*task_offload_header);
	task_offload_header->encap_format.flags.fixed_header_size = 1;
	task_offload_header->encap_format.header_size = sizeof(struct ethhdr);
	task_offload_header->encap_format.encap = IEEE_802_3_Encapsulation;
	status = mp_query(wnd, OID_TCP_TASK_OFFLOAD, buf, buf_size);
	TRACE1("%08X", status);
	if (status != NDIS_STATUS_SUCCESS)
		EXIT1(return);
	if (task_offload_header->offset_first_task == 0)
		EXIT1(return);
	task_offload = ((void *)task_offload_header +
			task_offload_header->offset_first_task);
	while (1) {
		TRACE1("%d, %d", task_offload->version, task_offload->task);
		switch (task_offload->task) {
		case TcpIpChecksumNdisTask:
			csum = (void *)task_offload->task_buf;
			break;
		case TcpLargeSendNdisTask:
			tso = (void *)task_offload->task_buf;
			break;
		default:
			TRACE1("%d", task_offload->task);
			break;
		}
		if (task_offload->offset_next_task == 0)
			break;
		task_offload = (void *)task_offload +
			task_offload->offset_next_task;
	}
	if (tso)
		TRACE1("%u, %u, %d, %d", tso->max_size, tso->min_seg_count,
		       tso->tcp_opts, tso->ip_opts);
	if (!csum)
		EXIT1(return);
	TRACE1("%08x, %08x", csum->v4_tx.value, csum->v4_rx.value);
	task_offload_header->encap_format.flags.fixed_header_size = 1;
	task_offload_header->encap_format.header_size = sizeof(struct ethhdr);
	task_offload_header->offset_first_task = sizeof(*task_offload_header);
	task_offload = ((void *)task_offload_header +
			task_offload_header->offset_first_task);
	task_offload->offset_next_task = 0;
	task_offload->size = sizeof(*task_offload);
	task_offload->task = TcpIpChecksumNdisTask;
	memcpy(task_offload->task_buf, csum, sizeof(*csum));
	task_offload->task_buf_length = sizeof(*csum);
	status = mp_set(wnd, OID_TCP_TASK_OFFLOAD, task_offload_header,
			sizeof(*task_offload_header) +
			sizeof(*task_offload) + sizeof(*csum));
	TRACE1("%08X", status);
	if (status != NDIS_STATUS_SUCCESS)
		EXIT2(return);
	wnd->tx_csum = csum->v4_tx;
	if (csum->v4_tx.tcp_csum && csum->v4_tx.udp_csum) {
		if (csum->v4_tx.ip_csum) {
			wnd->net_dev->features |= NETIF_F_HW_CSUM;
			TRACE1("hw checksum enabled");
		} else {
			wnd->net_dev->features |= NETIF_F_IP_CSUM;
			TRACE1("IP checksum enabled");
		}
		if (wnd->sg_dma_size)
			wnd->net_dev->features |= NETIF_F_SG;
	}
	wnd->rx_csum = csum->v4_rx;
	EXIT1(return);
}

static void get_supported_oids(struct ndis_device *wnd)
{
	NDIS_STATUS res;
	int i, n, needed;
	ndis_oid *oids;

	res = mp_query_info(wnd, OID_GEN_SUPPORTED_LIST, NULL, 0, NULL,
			    &needed);
	if (!(res == NDIS_STATUS_BUFFER_TOO_SHORT ||
	      res == NDIS_STATUS_INVALID_LENGTH))
		EXIT1(return);
	oids = kmalloc(needed, GFP_KERNEL);
	if (!oids) {
		TRACE1("couldn't allocate memory");
		EXIT1(return);
	}
	res = mp_query(wnd, OID_GEN_SUPPORTED_LIST, oids, needed);
	if (res) {
		TRACE1("failed: %08X", res);
		kfree(oids);
		EXIT1(return);
	}
	for (i = 0, n = needed / sizeof(*oids); i < n; i++) {
		TRACE1("oid: %08X", oids[i]);
		/* if a wireless device didn't say so for
		 * OID_GEN_PHYSICAL_MEDIUM (they should, but in case) */
		if (wnd->physical_medium != NdisPhysicalMediumWirelessLan &&
		    oids[i] == OID_802_11_SSID)
			wnd->physical_medium = NdisPhysicalMediumWirelessLan;
	}
	kfree(oids);
	EXIT1(return);
}

static void ndis_get_drvinfo(struct net_device *dev,
			     struct ethtool_drvinfo *info)
{
	struct ndis_device *wnd = netdev_priv(dev);
	strncpy(info->driver, DRIVER_NAME, sizeof(info->driver) - 2);
	strcat(info->driver, "+");
	strncat(info->driver, wnd->wd->driver->name,
		sizeof(info->driver) - strlen(DRIVER_NAME) - 1);
	strncpy(info->version, DRIVER_VERSION, sizeof(info->version) - 2);
	strcat(info->version, "+");
	strncat(info->version, wnd->wd->driver->version,
		sizeof(info->version) - strlen(DRIVER_VERSION) - 1);
	if (wrap_is_pci_bus(wnd->wd->dev_bus))
		strncpy(info->bus_info, pci_name(wnd->wd->pci.pdev),
			sizeof(info->bus_info) - 1);
#ifdef ENABLE_USB
	else
		usb_make_path(wnd->wd->usb.udev, info->bus_info,
			      sizeof(info->bus_info) - 1);
#endif
	return;
}

static u32 ndis_get_link(struct net_device *dev)
{
	struct ndis_device *wnd = netdev_priv(dev);
	return netif_carrier_ok(wnd->net_dev);
}

static void ndis_get_wol(struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct ndis_device *wnd = netdev_priv(dev);

	wol->supported = 0;
	wol->wolopts = 0;
	if (!(wnd->attributes & NDIS_ATTRIBUTE_NO_HALT_ON_SUSPEND))
		EXIT2(return);
	if (!wrap_is_pci_bus(wnd->wd->dev_bus))
		EXIT2(return);
	/* we always suspend to D3 */
	if (wnd->pnp_capa.wakeup.min_magic_packet_wakeup < NdisDeviceStateD3)
		return;
	wol->supported |= WAKE_MAGIC;
	if (wnd->ndis_wolopts & NDIS_PNP_WAKE_UP_MAGIC_PACKET)
		wol->wolopts |= WAKE_MAGIC;
	return;
}

static int ndis_set_wol(struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct ndis_device *wnd = netdev_priv(dev);

	if (!(wnd->attributes & NDIS_ATTRIBUTE_NO_HALT_ON_SUSPEND))
		return -EOPNOTSUPP;
	if (wnd->pnp_capa.wakeup.min_magic_packet_wakeup < NdisDeviceStateD3)
		EXIT2(return -EOPNOTSUPP);
	TRACE2("0x%x", wol->wolopts);
	if (wol->wolopts & WAKE_MAGIC) {
		wnd->ndis_wolopts |= NDIS_PNP_WAKE_UP_MAGIC_PACKET;
		if (wol->wolopts != WAKE_MAGIC)
			WARNING("ignored wake-on-lan options: 0x%x",
				wol->wolopts & ~WAKE_MAGIC);
	} else if (!wol->wolopts)
		wnd->ndis_wolopts = 0;
	else
		return -EOPNOTSUPP;
	TRACE2("0x%x", wnd->ndis_wolopts);
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)
static u32 ndis_get_tx_csum(struct net_device *dev)
{
	struct ndis_device *wnd = netdev_priv(dev);
	if (wnd->tx_csum.tcp_csum && wnd->tx_csum.udp_csum)
		return 1;
	else
		return 0;
}

static u32 ndis_get_rx_csum(struct net_device *dev)
{
	struct ndis_device *wnd = netdev_priv(dev);
	if (wnd->rx_csum.value)
		return 1;
	else
		return 0;
}

static int ndis_set_tx_csum(struct net_device *dev, u32 data)
{
	struct ndis_device *wnd = netdev_priv(dev);

	if (data && (wnd->tx_csum.value == 0))
		return -EOPNOTSUPP;

	if (wnd->tx_csum.ip_csum)
		ethtool_op_set_tx_hw_csum(dev, data);
	else
		ethtool_op_set_tx_csum(dev, data);
	return 0;
}

static int ndis_set_rx_csum(struct net_device *dev, u32 data)
{
	struct ndis_device *wnd = netdev_priv(dev);

	if (data && (wnd->tx_csum.value == 0))
		return -EOPNOTSUPP;

	/* TODO: enable/disable rx csum through NDIS */
	return 0;
}

static u32 ndis_get_sg(struct net_device *dev)
{
	struct ndis_device *wnd = netdev_priv(dev);
	if (wnd->sg_dma_size)
		return ethtool_op_get_sg(dev);
	else
		return 0;
}

static int ndis_set_sg(struct net_device *dev, u32 data)
{
	struct ndis_device *wnd = netdev_priv(dev);
	if (wnd->sg_dma_size)
		return ethtool_op_set_sg(dev, data);
	else
		return -EOPNOTSUPP;
}
#endif

static struct ethtool_ops ndis_ethtool_ops = {
	.get_drvinfo	= ndis_get_drvinfo,
	.get_link	= ndis_get_link,
	.get_wol	= ndis_get_wol,
	.set_wol	= ndis_set_wol,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)
	.get_tx_csum	= ndis_get_tx_csum,
	.get_rx_csum	= ndis_get_rx_csum,
	.set_tx_csum	= ndis_set_tx_csum,
	.set_rx_csum	= ndis_set_rx_csum,
	.get_sg		= ndis_get_sg,
	.set_sg		= ndis_set_sg,
#endif
};

static int notifier_event(struct notifier_block *notifier, unsigned long event,
			  void *ptr)
{
	struct net_device *net_dev = netdev_notifier_info_to_dev(ptr);

	ENTER2("0x%lx", event);
	if (net_dev->ethtool_ops == &ndis_ethtool_ops
	    && event == NETDEV_CHANGENAME) {
		struct ndis_device *wnd = netdev_priv(net_dev);

		/* called with rtnl lock held, so no need to lock */
		if (likely(wnd->procfs_iface)) {
			printk(KERN_INFO "%s: interface renamed to '%s'\n",
			       DRIVER_NAME, net_dev->name);
			wrap_procfs_remove_ndis_device(wnd);
			wrap_procfs_add_ndis_device(wnd);
		}
	}
	return NOTIFY_DONE;
}

static struct notifier_block netdev_notifier = {
	.notifier_call = notifier_event,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
static const struct net_device_ops ndis_netdev_ops = {
	.ndo_init = ndis_net_dev_init,
	.ndo_uninit = ndis_net_dev_uninit,
	.ndo_open = ndis_net_dev_open,
	.ndo_stop = ndis_net_dev_close,
	.ndo_start_xmit = tx_skbuff,
	.ndo_change_mtu = ndis_change_mtu,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0)
	.ndo_set_rx_mode = ndis_set_multicast_list,
#else
	.ndo_set_multicast_list = ndis_set_multicast_list,
#endif
	.ndo_set_mac_address = ndis_set_mac_address,
	.ndo_get_stats = ndis_get_stats,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller = ndis_poll_controller,
#endif
};
#endif

static NDIS_STATUS ndis_start_device(struct ndis_device *wnd)
{
	struct wrap_device *wd;
	struct net_device *net_dev;
	NDIS_STATUS status;
	char *buf;
	const int buf_len = 256;
	mac_address mac;
	struct transport_header_offset *tx_header_offset;
	int n;

	ENTER2("%d", in_atomic());
	status = mp_init(wnd);
	if (status == NDIS_STATUS_NOT_RECOGNIZED)
		EXIT1(return NDIS_STATUS_SUCCESS);
	if (status != NDIS_STATUS_SUCCESS)
		EXIT1(return status);
	wd = wnd->wd;
	net_dev = wnd->net_dev;

	get_supported_oids(wnd);
	memset(mac, 0, sizeof(mac));
	status = mp_query(wnd, OID_802_3_CURRENT_ADDRESS, mac, sizeof(mac));
	if (memcmp(mac, "\x00\x00\x00\x00\x00\x00", sizeof(mac)) == 0) {
		status = mp_query(wnd, OID_802_3_PERMANENT_ADDRESS, mac,
				  sizeof(mac));
		if (status != NDIS_STATUS_SUCCESS) {
			ERROR("couldn't get mac address: %08X", status);
			goto err_start;
		}
	}
	TRACE1("mac:" MACSTRSEP, MAC2STR(mac));
	memcpy(net_dev->dev_addr, mac, ETH_ALEN);

	strncpy(net_dev->name, if_name, IFNAMSIZ - 1);
	net_dev->name[IFNAMSIZ - 1] = 0;

	wnd->packet_filter = NDIS_PACKET_TYPE_DIRECTED |
		NDIS_PACKET_TYPE_BROADCAST | NDIS_PACKET_TYPE_MULTICAST;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
	net_dev->netdev_ops = &ndis_netdev_ops;
#else
	net_dev->init = ndis_net_dev_init;
	net_dev->uninit = ndis_net_dev_uninit;
	net_dev->open = ndis_net_dev_open;
	net_dev->hard_start_xmit = tx_skbuff;
	net_dev->stop = ndis_net_dev_close;
	net_dev->get_stats = ndis_get_stats;
	net_dev->change_mtu = ndis_change_mtu;
	net_dev->set_multicast_list = ndis_set_multicast_list;
	net_dev->set_mac_address = ndis_set_mac_address;
#ifdef CONFIG_NET_POLL_CONTROLLER
	net_dev->poll_controller = ndis_poll_controller;
#endif
#endif
#ifdef CONFIG_WIRELESS_EXT
	if (wnd->physical_medium == NdisPhysicalMediumWirelessLan) {
		net_dev->wireless_handlers = &ndis_handler_def;
	}
#endif
	net_dev->ethtool_ops = &ndis_ethtool_ops;
	if (wnd->mp_interrupt)
		net_dev->irq = wnd->mp_interrupt->irq;
	net_dev->mem_start = wnd->mem_start;
	net_dev->mem_end = wnd->mem_end;
	status = mp_query_int(wnd, OID_802_3_MAXIMUM_LIST_SIZE,
			      &wnd->multicast_size);
	if (status != NDIS_STATUS_SUCCESS || wnd->multicast_size < 0)
		wnd->multicast_size = 0;
	if (wnd->multicast_size > 0)
		net_dev->flags |= IFF_MULTICAST;
	else
		net_dev->flags &= ~IFF_MULTICAST;

	buf = kmalloc(buf_len, GFP_KERNEL);
	if (!buf) {
		WARNING("couldn't allocate memory");
		goto err_start;
	}

	set_task_offload(wnd, buf, buf_len);
#ifdef NETIF_F_LLTX
	net_dev->features |= NETIF_F_LLTX;
#endif

	if (register_netdev(net_dev)) {
		ERROR("cannot register net device %s", net_dev->name);
		goto err_register;
	}
	memset(buf, 0, buf_len);
	status = mp_query(wnd, OID_GEN_VENDOR_DESCRIPTION, buf, buf_len);
	if (status != NDIS_STATUS_SUCCESS) {
		WARNING("couldn't get vendor information: 0x%x", status);
		buf[0] = 0;
	}
	wnd->drv_ndis_version = n = 0;
	mp_query_int(wnd, OID_GEN_DRIVER_VERSION, &wnd->drv_ndis_version);
	mp_query_int(wnd, OID_GEN_VENDOR_DRIVER_VERSION, &n);

	printk(KERN_INFO "%s: ethernet device " MACSTRSEP " using %sNDIS "
	       "driver: %s, version: 0x%x, NDIS version: 0x%x, vendor: '%s', "
	       "%s\n", net_dev->name, MAC2STR(net_dev->dev_addr),
	       deserialized_driver(wnd) ? "" : "serialized ",
	       wd->driver->name, n, wnd->drv_ndis_version, buf,
	       wd->conf_file_name);

	if (deserialized_driver(wnd)) {
		/* deserialized drivers don't have a limit, but we
		 * keep max at TX_RING_SIZE */
		wnd->max_tx_packets = TX_RING_SIZE;
	} else {
		status = mp_query_int(wnd, OID_GEN_MAXIMUM_SEND_PACKETS,
				      &wnd->max_tx_packets);
		if (status != NDIS_STATUS_SUCCESS)
			wnd->max_tx_packets = 1;
		if (wnd->max_tx_packets > TX_RING_SIZE)
			wnd->max_tx_packets = TX_RING_SIZE;
	}
	TRACE2("maximum send packets: %d", wnd->max_tx_packets);
	NdisAllocatePacketPoolEx(&status, &wnd->tx_packet_pool,
				 wnd->max_tx_packets, 0,
				 PROTOCOL_RESERVED_SIZE_IN_PACKET);
	if (status != NDIS_STATUS_SUCCESS) {
		ERROR("couldn't allocate packet pool");
		goto packet_pool_err;
	}
	NdisAllocateBufferPool(&status, &wnd->tx_buffer_pool,
			       wnd->max_tx_packets + 4);
	if (status != NDIS_STATUS_SUCCESS) {
		ERROR("couldn't allocate buffer pool");
		goto buffer_pool_err;
	}
	TRACE1("pool: %p", wnd->tx_buffer_pool);

	if (mp_query_int(wnd, OID_GEN_MAXIMUM_TOTAL_SIZE, &n) ==
	    NDIS_STATUS_SUCCESS && n > ETH_HLEN)
		ndis_change_mtu(wnd->net_dev, n - ETH_HLEN);

	if (mp_query_int(wnd, OID_GEN_MAC_OPTIONS, &n) == NDIS_STATUS_SUCCESS)
		TRACE2("mac options supported: 0x%x", n);

	tx_header_offset = (typeof(tx_header_offset))buf;
	tx_header_offset->protocol_type = NDIS_PROTOCOL_ID_TCP_IP;
	tx_header_offset->header_offset = sizeof(ETH_HLEN);
	status = mp_set(wnd, OID_GEN_TRANSPORT_HEADER_OFFSET,
			tx_header_offset, sizeof(*tx_header_offset));
	TRACE2("%08X", status);

	status = mp_query_int(wnd, OID_GEN_PHYSICAL_MEDIUM,
			      &wnd->physical_medium);
	if (status != NDIS_STATUS_SUCCESS)
		wnd->physical_medium = NdisPhysicalMediumUnspecified;

#ifdef CONFIG_WIRELESS_EXT
	if (wnd->physical_medium == NdisPhysicalMediumWirelessLan) {
		mp_set_int(wnd, OID_802_11_POWER_MODE, NDIS_POWER_OFF);
		get_encryption_capa(wnd, buf, buf_len);
		TRACE1("capabilities = %ld", wnd->capa.encr);
		printk(KERN_INFO "%s: encryption modes supported: "
		       "%s%s%s%s%s%s%s\n", net_dev->name,
		       test_bit(Ndis802_11Encryption1Enabled, &wnd->capa.encr) ?
		       "WEP" : "none",

		       test_bit(Ndis802_11Encryption2Enabled, &wnd->capa.encr) ?
		       "; TKIP with WPA" : "",
		       test_bit(Ndis802_11AuthModeWPA2, &wnd->capa.auth) ?
		       ", WPA2" : "",
		       test_bit(Ndis802_11AuthModeWPA2PSK, &wnd->capa.auth) ?
		       ", WPA2-PSK" : "",

		       test_bit(Ndis802_11Encryption3Enabled, &wnd->capa.encr) ?
		       "; AES/CCMP with WPA" : "",
		       test_bit(Ndis802_11AuthModeWPA2, &wnd->capa.auth) ?
		       ", WPA2" : "",
		       test_bit(Ndis802_11AuthModeWPA2PSK, &wnd->capa.auth) ?
		       ", WPA2-PSK" : "");

		set_default_iw_params(wnd);
	}
#endif
	kfree(buf);
	hangcheck_add(wnd);
	add_iw_stats_timer(wnd);
	EXIT1(return NDIS_STATUS_SUCCESS);

buffer_pool_err:
	wnd->tx_buffer_pool = NULL;
	if (wnd->tx_packet_pool) {
		NdisFreePacketPool(wnd->tx_packet_pool);
		wnd->tx_packet_pool = NULL;
	}
packet_pool_err:
	unregister_netdev(net_dev);
	wnd->max_tx_packets = 0;
err_register:
	kfree(buf);
err_start:
	mp_halt(wnd);
	EXIT1(return NDIS_STATUS_FAILURE);
}

static int ndis_remove_device(struct ndis_device *wnd)
{
	s8 tx_pending;
	int our_mutex;

	/* prevent setting essid during disassociation */
	memset(&wnd->essid, 0, sizeof(wnd->essid));
	wnd->tx_ok = 0;
	netif_carrier_off(wnd->net_dev);
	if (wnd->max_tx_packets)
		unregister_netdev(wnd->net_dev);
	/* if device is suspended, but resume failed, tx_ring_mutex
	 * may already be locked */
	our_mutex = mutex_trylock(&wnd->tx_ring_mutex);
	if (!our_mutex)
		WARNING("couldn't obtain tx_ring_mutex");
	spin_lock_bh(&wnd->tx_ring_lock);
	tx_pending = wnd->tx_ring_end - wnd->tx_ring_start;
	if (tx_pending < 0)
		tx_pending += TX_RING_SIZE;
	else if (tx_pending == 0 && wnd->is_tx_ring_full)
		tx_pending = TX_RING_SIZE - 1;
	wnd->is_tx_ring_full = 0;
	/* throw away pending packets */
	while (tx_pending-- > 0) {
		struct ndis_packet *packet;

		packet = wnd->tx_ring[wnd->tx_ring_start];
		free_tx_packet(wnd, packet, NDIS_STATUS_CLOSING);
		wnd->tx_ring_start = (wnd->tx_ring_start + 1) % TX_RING_SIZE;
	}
	spin_unlock_bh(&wnd->tx_ring_lock);
	if (our_mutex)
		mutex_unlock(&wnd->tx_ring_mutex);
	mp_halt(wnd);
	ndis_exit_device(wnd);

	if (wnd->tx_packet_pool) {
		NdisFreePacketPool(wnd->tx_packet_pool);
		wnd->tx_packet_pool = NULL;
	}
	if (wnd->tx_buffer_pool) {
		NdisFreeBufferPool(wnd->tx_buffer_pool);
		wnd->tx_buffer_pool = NULL;
	}
	kfree(wnd->pmkids);
	printk(KERN_INFO "%s: device %s removed\n", DRIVER_NAME,
	       wnd->net_dev->name);
	kfree(wnd->nmb);
	free_netdev(wnd->net_dev);
	EXIT2(return 0);
}

static NTSTATUS ndis_add_device(struct driver_object *drv_obj,
				struct device_object *pdo)
{
	struct device_object *fdo;
	struct ndis_mp_block *nmb;
	NTSTATUS status;
	struct ndis_device *wnd;
	struct net_device *net_dev;
	struct wrap_device *wd;
	unsigned long i;

	ENTER2("%p, %p", drv_obj, pdo);
	if (strlen(if_name) >= IFNAMSIZ) {
		ERROR("interface name '%s' is too long", if_name);
		return STATUS_INVALID_PARAMETER;
	}
	net_dev = alloc_etherdev(sizeof(*wnd));
	if (!net_dev) {
		ERROR("couldn't allocate device");
		return STATUS_RESOURCES;
	}
	wd = pdo->reserved;
	if (wrap_is_pci_bus(wd->dev_bus))
		SET_NETDEV_DEV(net_dev, &wd->pci.pdev->dev);
	if (wrap_is_usb_bus(wd->dev_bus))
		SET_NETDEV_DEV(net_dev, &wd->usb.intf->dev);
	status = IoCreateDevice(drv_obj, 0, NULL, FILE_DEVICE_UNKNOWN, 0,
				FALSE, &fdo);
	if (status != STATUS_SUCCESS) {
		free_netdev(net_dev);
		EXIT2(return status);
	}
	wnd = netdev_priv(net_dev);
	TRACE1("wnd: %p", wnd);

	nmb = kmalloc(sizeof(*nmb), GFP_KERNEL);
	if (!nmb) {
		WARNING("couldn't allocate memory");
		IoDeleteDevice(fdo);
		free_netdev(net_dev);
		return STATUS_RESOURCES;
	}
#if DEBUG >= 6
	/* poison nmb so if a driver accesses uninitialized pointers, we
	 * know what it is */
	for (i = 0; i < sizeof(*nmb) / sizeof(unsigned long); i++)
		((unsigned long *)nmb)[i] = i + 0x8a3fc1;
#endif

	wnd->nmb = nmb;
	nmb->wnd = wnd;
	nmb->pdo = pdo;
	wnd->wd = wd;
	wnd->net_dev = net_dev;
	fdo->reserved = wnd;
	nmb->fdo = fdo;
	if (ndis_init_device(wnd)) {
		IoDeleteDevice(fdo);
		kfree(nmb);
		free_netdev(net_dev);
		EXIT1(return STATUS_RESOURCES);
	}
	nmb->next_device = IoAttachDeviceToDeviceStack(fdo, pdo);
	spin_lock_init(&wnd->tx_ring_lock);
	mutex_init(&wnd->tx_ring_mutex);
	mutex_init(&wnd->ndis_req_mutex);
	wnd->ndis_req_done = 0;
	INIT_WORK(&wnd->tx_work, tx_worker);
	wnd->tx_ring_start = 0;
	wnd->tx_ring_end = 0;
	wnd->is_tx_ring_full = 0;
	wnd->capa.encr = 0;
	wnd->capa.auth = 0;
	wnd->attributes = 0;
	wnd->dma_map_count = 0;
	wnd->dma_map_addr = NULL;
	wnd->nick[0] = 0;
	init_timer(&wnd->hangcheck_timer);
	wnd->scan_timestamp = 0;
	init_timer(&wnd->iw_stats_timer);
	wnd->iw_stats_interval = 10 * HZ;
	wnd->ndis_pending_work = 0;
	memset(&wnd->essid, 0, sizeof(wnd->essid));
	memset(&wnd->encr_info, 0, sizeof(wnd->encr_info));
	wnd->infrastructure_mode = Ndis802_11Infrastructure;
	INIT_WORK(&wnd->ndis_work, wrapndis_worker);
	wnd->iw_stats_enabled = TRUE;

	TRACE1("nmb: %p, pdo: %p, fdo: %p, attached: %p, next: %p",
	       nmb, pdo, fdo, fdo->attached, nmb->next_device);

	/* dispatch routines are called as Windows functions */
	for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
		drv_obj->major_func[i] = WIN_FUNC_PTR(IoPassIrpDown,2);

	drv_obj->major_func[IRP_MJ_PNP] = WIN_FUNC_PTR(NdisDispatchPnp,2);
	drv_obj->major_func[IRP_MJ_POWER] = WIN_FUNC_PTR(NdisDispatchPower,2);
	drv_obj->major_func[IRP_MJ_INTERNAL_DEVICE_CONTROL] =
		WIN_FUNC_PTR(NdisDispatchDeviceControl,2);
//	drv_obj->major_func[IRP_MJ_DEVICE_CONTROL] =
//		WIN_FUNC_PTR(NdisDispatchDeviceControl,2);
	EXIT2(return STATUS_SUCCESS);
}

int init_ndis_driver(struct driver_object *drv_obj)
{
	ENTER1("%p", drv_obj);
	drv_obj->drv_ext->add_device = ndis_add_device;
	return 0;
}

int wrapndis_init(void)
{
	wrapndis_wq = create_singlethread_workqueue("wrapndis_wq");
	if (!wrapndis_wq)
		EXIT1(return -ENOMEM);
	TRACE1("wrapndis_wq: %p", wrapndis_wq);
	register_netdevice_notifier(&netdev_notifier);
	return 0;
}

void wrapndis_exit(void)
{
	unregister_netdevice_notifier(&netdev_notifier);
	if (wrapndis_wq)
		destroy_workqueue(wrapndis_wq);
}
