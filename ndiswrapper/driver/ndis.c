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

#include "ndis.h"
#include "iw_ndis.h"
#include "wrapndis.h"
#include "pnp.h"
#include "loader.h"
#include <linux/kernel_stat.h>
#include <asm/dma.h>
#include "ndis_exports.h"

#define MAX_ALLOCATED_NDIS_PACKETS TX_RING_SIZE
#define MAX_ALLOCATED_NDIS_BUFFERS TX_RING_SIZE

static struct work_struct ndis_work;
static struct nt_list ndis_work_list;
static spinlock_t ndis_work_list_lock;

struct workqueue_struct *ndis_wq;

static void *ndis_get_routine_address(char *name);

wstdcall void WIN_FUNC(NdisInitializeWrapper,4)
	(void **driver_handle, struct driver_object *driver,
	 struct unicode_string *reg_path, void *unused)
{
	ENTER1("handle: %p, driver: %p", driver_handle, driver);
	*driver_handle = driver;
	EXIT1(return);
}

wstdcall void WIN_FUNC(NdisTerminateWrapper,2)
	(struct device_object *dev_obj, void *system_specific)
{
	EXIT1(return);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMRegisterMiniport,3)
	(struct driver_object *drv_obj, struct miniport *mp, UINT length)
{
	int min_length;
	struct wrap_driver *wrap_driver;
	struct ndis_driver *ndis_driver;

	min_length = ((char *)&mp->co_create_vc) - ((char *)mp);

	ENTER1("%p %p %d", drv_obj, mp, length);

	if (mp->major_version < 4) {
		ERROR("Driver is using ndis version %d which is too old.",
		      mp->major_version);
		EXIT1(return NDIS_STATUS_BAD_VERSION);
	}

	if (length < min_length) {
		ERROR("Characteristics length %d is too small", length);
		EXIT1(return NDIS_STATUS_BAD_CHARACTERISTICS);
	}

	TRACE1("%d.%d, %d, %u", mp->major_version, mp->minor_version, length,
	       (u32)sizeof(struct miniport));
	wrap_driver = IoGetDriverObjectExtension(drv_obj,
						 (void *)WRAP_DRIVER_CLIENT_ID);
	if (!wrap_driver) {
		ERROR("couldn't get wrap_driver");
		EXIT1(return NDIS_STATUS_RESOURCES);
	}
	if (IoAllocateDriverObjectExtension(
		    drv_obj, (void *)NDIS_DRIVER_CLIENT_ID,
		    sizeof(*ndis_driver), (void **)&ndis_driver) !=
	    STATUS_SUCCESS)
		EXIT1(return NDIS_STATUS_RESOURCES);
	wrap_driver->ndis_driver = ndis_driver;
	TRACE1("driver: %p", ndis_driver);
	memcpy(&ndis_driver->mp, mp, min_t(int, sizeof(*mp), length));

	DBG_BLOCK(2) {
		int i;
		void **func;
		char *mp_funcs[] = {
			"queryinfo", "reconfig", "reset", "send", "setinfo",
			"tx_data", "return_packet", "send_packets",
			"alloc_complete", "co_create_vc", "co_delete_vc",
			"co_activate_vc", "co_deactivate_vc",
			"co_send_packets", "co_request", "cancel_send_packets",
			"pnp_event_notify", "shutdown",
		};
		func = (void **)&ndis_driver->mp.queryinfo;
		for (i = 0; i < ARRAY_SIZE(mp_funcs); i++)
			TRACE0("function '%s' is at %p", mp_funcs[i], func[i]);
	}
	EXIT1(return NDIS_STATUS_SUCCESS);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMRegisterDevice,6)
	(struct driver_object *drv_obj, struct unicode_string *dev_name,
	 struct unicode_string *link, void **funcs,
	 struct device_object **dev_obj, void **dev_obj_handle)
{
	NTSTATUS status;
	struct device_object *tmp;
	int i;

	ENTER1("%p, %p, %p", drv_obj, dev_name, link);
	status = IoCreateDevice(drv_obj, 0, dev_name, FILE_DEVICE_NETWORK, 0,
				FALSE, &tmp);

	if (status != STATUS_SUCCESS)
		EXIT1(return NDIS_STATUS_RESOURCES);
	if (link)
		status = IoCreateSymbolicLink(link, dev_name);
	if (status != STATUS_SUCCESS) {
		IoDeleteDevice(tmp);
		EXIT1(return NDIS_STATUS_RESOURCES);
	}

	*dev_obj = tmp;
	*dev_obj_handle = *dev_obj;
	for (i = 0; i < IRP_MJ_MAXIMUM_FUNCTION; i++)
		if (funcs[i] && i != IRP_MJ_PNP && i != IRP_MJ_POWER) {
			drv_obj->major_func[i] = funcs[i];
			TRACE1("mj_fn for 0x%x is at %p", i, funcs[i]);
		}
	EXIT1(return NDIS_STATUS_SUCCESS);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMDeregisterDevice,1)
	(struct device_object *dev_obj)
{
	ENTER2("%p", dev_obj);
	IoDeleteDevice(dev_obj);
	return NDIS_STATUS_SUCCESS;
}

wstdcall NDIS_STATUS WIN_FUNC(NdisAllocateMemoryWithTag,3)
	(void **dest, UINT length, ULONG tag)
{
	void *addr;

	assert_irql(_irql_ <= DISPATCH_LEVEL);
	addr = ExAllocatePoolWithTag(NonPagedPool, length, tag);
	TRACE4("%p", addr);
	if (addr) {
		*dest = addr;
		EXIT4(return NDIS_STATUS_SUCCESS);
	} else
		EXIT4(return NDIS_STATUS_FAILURE);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisAllocateMemory,4)
	(void **dest, UINT length, UINT flags, NDIS_PHY_ADDRESS highest_address)
{
	return NdisAllocateMemoryWithTag(dest, length, 0);
}

/* length_tag is either length or tag, depending on if
 * NdisAllocateMemory or NdisAllocateMemoryTag is used to allocate
 * memory */
wstdcall void WIN_FUNC(NdisFreeMemory,3)
	(void *addr, UINT length_tag, UINT flags)
{
	TRACE4("%p", addr);
	ExFreePool(addr);
}

noregparm void WIN_FUNC(NdisWriteErrorLogEntry,12)
	(struct driver_object *drv_obj, ULONG error, ULONG count, ...)
{
	va_list args;
	int i;
	ULONG code;

	va_start(args, count);
	ERROR("log: %08X, count: %d, return_address: %p",
	      error, count, __builtin_return_address(0));
	for (i = 0; i < count; i++) {
		code = va_arg(args, ULONG);
		ERROR("code: 0x%x", code);
	}
	va_end(args);
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisOpenConfiguration,3)
	(NDIS_STATUS *status, struct ndis_mp_block **conf_handle,
	 struct ndis_mp_block *handle)
{
	ENTER2("%p", conf_handle);
	*conf_handle = handle;
	*status = NDIS_STATUS_SUCCESS;
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisOpenProtocolConfiguration,3)
	(NDIS_STATUS *status, void **confhandle,
	 struct unicode_string *section)
{
	ENTER2("%p", confhandle);
	*status = NDIS_STATUS_SUCCESS;
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisOpenConfigurationKeyByName,4)
	(NDIS_STATUS *status, void *handle,
	 struct unicode_string *key, void **subkeyhandle)
{
	struct ansi_string ansi;
	ENTER2("");
	if (RtlUnicodeStringToAnsiString(&ansi, key, TRUE) == STATUS_SUCCESS) {
		TRACE2("%s", ansi.buf);
		RtlFreeAnsiString(&ansi);
	}
	*subkeyhandle = handle;
	*status = NDIS_STATUS_SUCCESS;
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisOpenConfigurationKeyByIndex,5)
	(NDIS_STATUS *status, void *handle, ULONG index,
	 struct unicode_string *key, void **subkeyhandle)
{
	ENTER2("%u", index);
//	*subkeyhandle = handle;
	*status = NDIS_STATUS_FAILURE;
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisCloseConfiguration,1)
	(void *handle)
{
	/* instead of freeing all configuration parameters as we are
	 * supposed to do here, we free them when the device is
	 * removed */
	ENTER2("%p", handle);
	return;
}

wstdcall void WIN_FUNC(NdisOpenFile,5)
	(NDIS_STATUS *status, struct wrap_bin_file **file,
	 UINT *filelength, struct unicode_string *filename,
	 NDIS_PHY_ADDRESS highest_address)
{
	struct ansi_string ansi;
	struct wrap_bin_file *bin_file;

	ENTER2("%p, %d, %llx, %p", status, *filelength, highest_address, *file);
	if (RtlUnicodeStringToAnsiString(&ansi, filename, TRUE) !=
	    STATUS_SUCCESS) {
		*status = NDIS_STATUS_RESOURCES;
		EXIT2(return);
	}
	TRACE2("%s", ansi.buf);
	bin_file = get_bin_file(ansi.buf);
	if (bin_file) {
		*file = bin_file;
		*filelength = bin_file->size;
		*status = NDIS_STATUS_SUCCESS;
	} else
		*status = NDIS_STATUS_FILE_NOT_FOUND;

	RtlFreeAnsiString(&ansi);
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisMapFile,3)
	(NDIS_STATUS *status, void **mappedbuffer, struct wrap_bin_file *file)
{
	ENTER2("%p", file);

	if (!file) {
		*status = NDIS_STATUS_ALREADY_MAPPED;
		EXIT2(return);
	}

	*status = NDIS_STATUS_SUCCESS;
	*mappedbuffer = file->data;
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisUnmapFile,1)
	(struct wrap_bin_file *file)
{
	ENTER2("%p", file);
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisCloseFile,1)
	(struct wrap_bin_file *file)
{
	ENTER2("%p", file);
	free_bin_file(file);
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisGetSystemUpTime,1)
	(ULONG *ms)
{
	*ms = 1000 * jiffies / HZ;
	EXIT5(return);
}

wstdcall ULONG WIN_FUNC(NDIS_BUFFER_TO_SPAN_PAGES,1)
	(ndis_buffer *buffer)
{
	ULONG n, length;

	if (buffer == NULL)
		EXIT2(return 0);
	if (MmGetMdlByteCount(buffer) == 0)
		EXIT2(return 1);

	length = MmGetMdlByteCount(buffer);
	n = SPAN_PAGES(MmGetMdlVirtualAddress(buffer), length);
	TRACE4("%p, %p, %d, %d", buffer->startva, buffer->mappedsystemva,
	       length, n);
	EXIT3(return n);
}

wstdcall void WIN_FUNC(NdisGetBufferPhysicalArraySize,2)
	(ndis_buffer *buffer, UINT *arraysize)
{
	ENTER3("%p", buffer);
	*arraysize = NDIS_BUFFER_TO_SPAN_PAGES(buffer);
	EXIT3(return);
}

static struct ndis_configuration_parameter *
ndis_encode_setting(struct wrap_device_setting *setting,
		    enum ndis_parameter_type type)
{
	struct ansi_string ansi;
	struct ndis_configuration_parameter *param;

	param = setting->encoded;
	if (param) {
		if (param->type == type)
			EXIT2(return param);
		if (param->type == NdisParameterString)
			RtlFreeUnicodeString(&param->data.string);
		setting->encoded = NULL;
	} else
		param = ExAllocatePoolWithTag(NonPagedPool, sizeof(*param), 0);
	if (!param) {
		ERROR("couldn't allocate memory");
		return NULL;
	}
	switch (type) {
	case NdisParameterInteger:
		param->data.integer = simple_strtol(setting->value, NULL, 0);
		TRACE2("0x%x", param->data.integer);
		break;
	case NdisParameterHexInteger:
		param->data.integer = simple_strtol(setting->value, NULL, 16);
		TRACE2("0x%x", param->data.integer);
		break;
	case NdisParameterString:
		RtlInitAnsiString(&ansi, setting->value);
		TRACE2("'%s'", ansi.buf);
		if (RtlAnsiStringToUnicodeString(&param->data.string,
						 &ansi, TRUE)) {
			ExFreePool(param);
			EXIT2(return NULL);
		}
		break;
	case NdisParameterBinary:
		param->data.integer = simple_strtol(setting->value, NULL, 2);
		TRACE2("0x%x", param->data.integer);
		break;
	default:
		ERROR("unknown type: %d", type);
		ExFreePool(param);
		return NULL;
	}
	param->type = type;
	setting->encoded = param;
	EXIT2(return param);
}

static int ndis_decode_setting(struct wrap_device_setting *setting,
			       struct ndis_configuration_parameter *param)
{
	struct ansi_string ansi;
	struct ndis_configuration_parameter *prev;

	ENTER2("%p, %p", setting, param);
	prev = setting->encoded;
	if (prev && prev->type == NdisParameterString) {
		RtlFreeUnicodeString(&prev->data.string);
		setting->encoded = NULL;
	}
	switch (param->type) {
	case NdisParameterInteger:
		snprintf(setting->value, MAX_SETTING_VALUE_LEN, "%u",
			 param->data.integer);
		break;
	case NdisParameterHexInteger:
		snprintf(setting->value, MAX_SETTING_VALUE_LEN, "%x",
			 param->data.integer);
		break;
	case NdisParameterString:
		ansi.buf = setting->value;
		ansi.max_length = MAX_SETTING_VALUE_LEN;
		if ((RtlUnicodeStringToAnsiString(&ansi, &param->data.string,
						  FALSE) != STATUS_SUCCESS)
		    || ansi.length >= MAX_SETTING_VALUE_LEN) {
			EXIT1(return -1);
		}
		if (ansi.length == ansi.max_length)
			ansi.length--;
		setting->value[ansi.length] = 0;
		break;
	default:
		TRACE2("unknown setting type: %d", param->type);
		return -1;
	}
	TRACE2("setting changed %s='%s', %d", setting->name, setting->value,
	       ansi.length);
	return 0;
}

static int read_setting(struct nt_list *setting_list, char *keyname, int length,
			struct ndis_configuration_parameter **param,
			enum ndis_parameter_type type)
{
	struct wrap_device_setting *setting;
	mutex_lock(&loader_mutex);
	nt_list_for_each_entry(setting, setting_list, list) {
		if (strncasecmp(keyname, setting->name, length) == 0) {
			TRACE2("setting %s='%s'", keyname, setting->value);
			mutex_unlock(&loader_mutex);
			*param = ndis_encode_setting(setting, type);
			if (*param)
				EXIT2(return 0);
			else
				EXIT2(return -1);
		}
	}
	mutex_unlock(&loader_mutex);
	EXIT2(return -1);
}

wstdcall void WIN_FUNC(NdisReadConfiguration,5)
	(NDIS_STATUS *status, struct ndis_configuration_parameter **param,
	 struct ndis_mp_block *nmb, struct unicode_string *key,
	 enum ndis_parameter_type type)
{
	struct ansi_string ansi;
	int ret;

	ENTER2("nmb: %p", nmb);
	ret = RtlUnicodeStringToAnsiString(&ansi, key, TRUE);
	if (ret != STATUS_SUCCESS || ansi.buf == NULL) {
		*param = NULL;
		*status = NDIS_STATUS_FAILURE;
		RtlFreeAnsiString(&ansi);
		EXIT2(return);
	}
	TRACE2("%d, %s", type, ansi.buf);

	if (read_setting(&nmb->wnd->wd->settings, ansi.buf,
			 ansi.length, param, type) == 0 ||
	    read_setting(&nmb->wnd->wd->driver->settings, ansi.buf,
			 ansi.length, param, type) == 0)
		*status = NDIS_STATUS_SUCCESS;
	else {
		TRACE2("setting %s not found (type:%d)", ansi.buf, type);
		*status = NDIS_STATUS_FAILURE;
	}
	RtlFreeAnsiString(&ansi);
	EXIT2(return);

}

wstdcall void WIN_FUNC(NdisWriteConfiguration,4)
	(NDIS_STATUS *status, struct ndis_mp_block *nmb,
	 struct unicode_string *key, struct ndis_configuration_parameter *param)
{
	struct ansi_string ansi;
	char *keyname;
	struct wrap_device_setting *setting;

	ENTER2("nmb: %p", nmb);
	if (RtlUnicodeStringToAnsiString(&ansi, key, TRUE)) {
		*status = NDIS_STATUS_FAILURE;
		EXIT2(return);
	}
	keyname = ansi.buf;
	TRACE2("%s", keyname);

	mutex_lock(&loader_mutex);
	nt_list_for_each_entry(setting, &nmb->wnd->wd->settings, list) {
		if (strncasecmp(keyname, setting->name, ansi.length) == 0) {
			mutex_unlock(&loader_mutex);
			if (ndis_decode_setting(setting, param))
				*status = NDIS_STATUS_FAILURE;
			else
				*status = NDIS_STATUS_SUCCESS;
			RtlFreeAnsiString(&ansi);
			EXIT2(return);
		}
	}
	mutex_unlock(&loader_mutex);
	setting = kzalloc(sizeof(*setting), GFP_KERNEL);
	if (setting) {
		if (ansi.length == ansi.max_length)
			ansi.length--;
		memcpy(setting->name, keyname, ansi.length);
		setting->name[ansi.length] = 0;
		if (ndis_decode_setting(setting, param))
			*status = NDIS_STATUS_FAILURE;
		else {
			*status = NDIS_STATUS_SUCCESS;
			mutex_lock(&loader_mutex);
			InsertTailList(&nmb->wnd->wd->settings, &setting->list);
			mutex_unlock(&loader_mutex);
		}
	} else
		*status = NDIS_STATUS_RESOURCES;

	RtlFreeAnsiString(&ansi);
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisReadNetworkAddress,4)
	(NDIS_STATUS *status, void **addr, UINT *len,
	 struct ndis_mp_block *nmb)
{
	struct ndis_device *wnd = nmb->wnd;
	struct ndis_configuration_parameter *param;
	struct unicode_string key;
	struct ansi_string ansi;
	typeof(wnd->mac) mac;
	int i, ret;

	ENTER2("%p", nmb);
	RtlInitAnsiString(&ansi, "NetworkAddress");
	*status = NDIS_STATUS_FAILURE;
	if (RtlAnsiStringToUnicodeString(&key, &ansi, TRUE) != STATUS_SUCCESS)
		EXIT1(return);

	NdisReadConfiguration(&ret, &param, nmb, &key, NdisParameterString);
	RtlFreeUnicodeString(&key);
	if (ret != NDIS_STATUS_SUCCESS)
		EXIT1(return);
	ret = RtlUnicodeStringToAnsiString(&ansi, &param->data.string, TRUE);
	if (ret != STATUS_SUCCESS)
		EXIT1(return);

	i = 0;
	if (ansi.length >= 2 * sizeof(mac)) {
		for (i = 0; i < sizeof(mac); i++) {
			char c[3];
			int x;
			c[0] = ansi.buf[i*2];
			c[1] = ansi.buf[i*2+1];
			c[2] = 0;
			ret = sscanf(c, "%x", &x);
			if (ret != 1)
				break;
			mac[i] = x;
		}
	}
	TRACE2("%s, %d, " MACSTR, ansi.buf, i, MAC2STR(mac));
	RtlFreeAnsiString(&ansi);
	if (i == sizeof(mac)) {
		memcpy(wnd->mac, mac, sizeof(wnd->mac));
		*len = sizeof(mac);
		*addr = wnd->mac;
		*status = NDIS_STATUS_SUCCESS;
	}
	EXIT1(return);
}

wstdcall void WIN_FUNC(NdisInitializeString,2)
	(struct unicode_string *dest, UCHAR *src)
{
	struct ansi_string ansi;

	ENTER2("");
	if (src == NULL) {
		dest->length = dest->max_length = 0;
		dest->buf = NULL;
	} else {
		RtlInitAnsiString(&ansi, src);
		/* the string is freed with NdisFreeMemory */
		RtlAnsiStringToUnicodeString(dest, &ansi, TRUE);
	}
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisInitAnsiString,2)
	(struct ansi_string *dst, CHAR *src)
{
	RtlInitAnsiString(dst, src);
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisInitUnicodeString,2)
	(struct unicode_string *dest, const wchar_t *src)
{
	RtlInitUnicodeString(dest, src);
	return;
}

wstdcall NDIS_STATUS WIN_FUNC(NdisAnsiStringToUnicodeString,2)
	(struct unicode_string *dst, struct ansi_string *src)
{
	ENTER2("");
	if (dst == NULL || src == NULL)
		EXIT2(return NDIS_STATUS_FAILURE);
	if (RtlAnsiStringToUnicodeString(dst, src, FALSE) == STATUS_SUCCESS)
		return NDIS_STATUS_SUCCESS;
	else
		return NDIS_STATUS_FAILURE;
}

wstdcall NDIS_STATUS WIN_FUNC(NdisUnicodeStringToAnsiString,2)
	(struct ansi_string *dst, struct unicode_string *src)
{
	ENTER2("");
	if (dst == NULL || src == NULL)
		EXIT2(return NDIS_STATUS_FAILURE);
	if (RtlUnicodeStringToAnsiString(dst, src, FALSE) == STATUS_SUCCESS)
		return NDIS_STATUS_SUCCESS;
	else
		return NDIS_STATUS_FAILURE;
}

wstdcall NTSTATUS WIN_FUNC(NdisUpcaseUnicodeString,2)
	(struct unicode_string *dst, struct unicode_string *src)
{
	EXIT2(return RtlUpcaseUnicodeString(dst, src, FALSE));
}

wstdcall void WIN_FUNC(NdisMSetAttributesEx,5)
	(struct ndis_mp_block *nmb, void *mp_ctx,
	 UINT hangcheck_interval, UINT attributes, ULONG adaptertype)
{
	struct ndis_device *wnd;

	ENTER1("%p, %p, %d, %08x, %d", nmb, mp_ctx, hangcheck_interval,
	       attributes, adaptertype);
	wnd = nmb->wnd;
	nmb->mp_ctx = mp_ctx;
	wnd->attributes = attributes;

	if ((attributes & NDIS_ATTRIBUTE_BUS_MASTER) &&
	    wrap_is_pci_bus(wnd->wd->dev_bus))
		pci_set_master(wnd->wd->pci.pdev);

	if (hangcheck_interval > 0)
		wnd->hangcheck_interval = 2 * hangcheck_interval * HZ;
	else
		wnd->hangcheck_interval = 2 * HZ;

	EXIT1(return);
}

wstdcall ULONG WIN_FUNC(NdisReadPciSlotInformation,5)
	(struct ndis_mp_block *nmb, ULONG slot,
	 ULONG offset, char *buf, ULONG len)
{
	struct wrap_device *wd = nmb->wnd->wd;
	ULONG i;
	if (!wrap_is_pci_bus(wd->dev_bus)) {
		ERROR("used on a non-PCI device");
		return 0;
	}
	for (i = 0; i < len; i++)
		if (pci_read_config_byte(wd->pci.pdev, offset + i, &buf[i]) !=
		    PCIBIOS_SUCCESSFUL)
			break;
	DBG_BLOCK(2) {
		if (i != len)
			WARNING("%u, %u", i, len);
	}
	return i;
}

wstdcall ULONG WIN_FUNC(NdisImmediateReadPciSlotInformation,5)
	(struct ndis_mp_block *nmb, ULONG slot,
	 ULONG offset, char *buf, ULONG len)
{
	return NdisReadPciSlotInformation(nmb, slot, offset, buf, len);
}

wstdcall ULONG WIN_FUNC(NdisWritePciSlotInformation,5)
	(struct ndis_mp_block *nmb, ULONG slot,
	 ULONG offset, char *buf, ULONG len)
{
	struct wrap_device *wd = nmb->wnd->wd;
	ULONG i;
	if (!wrap_is_pci_bus(wd->dev_bus)) {
		ERROR("used on a non-PCI device");
		return 0;
	}
	for (i = 0; i < len; i++)
		if (pci_write_config_byte(wd->pci.pdev, offset + i, buf[i]) !=
		    PCIBIOS_SUCCESSFUL)
			break;
	DBG_BLOCK(2) {
		if (i != len)
			WARNING("%u, %u", i, len);
	}
	return i;
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMRegisterIoPortRange,4)
	(void **virt, struct ndis_mp_block *nmb, UINT start, UINT len)
{
	ENTER3("%08x %08x", start, len);
	*virt = (void *)(ULONG_PTR)start;
	return NDIS_STATUS_SUCCESS;
}

wstdcall void WIN_FUNC(NdisMDeregisterIoPortRange,4)
	(struct ndis_mp_block *nmb, UINT start, UINT len, void* virt)
{
	ENTER1("%08x %08x", start, len);
}

wstdcall void WIN_FUNC(NdisReadPortUchar,3)
	(struct ndis_mp_block *nmb, ULONG port, char *data)
{
	*data = inb(port);
}

wstdcall void WIN_FUNC(NdisImmediateReadPortUchar,3)
	(struct ndis_mp_block *nmb, ULONG port, char *data)
{
	*data = inb(port);
}

wstdcall void WIN_FUNC(NdisWritePortUchar,3)
	(struct ndis_mp_block *nmb, ULONG port, char data)
{
	outb(data, port);
}

wstdcall void WIN_FUNC(NdisImmediateWritePortUchar,3)
	(struct ndis_mp_block *nmb, ULONG port, char data)
{
	outb(data, port);
}

wstdcall void WIN_FUNC(NdisMQueryAdapterResources,4)
	(NDIS_STATUS *status, struct ndis_mp_block *nmb,
	 NDIS_RESOURCE_LIST *resource_list, UINT *size)
{
	struct ndis_device *wnd = nmb->wnd;
	NDIS_RESOURCE_LIST *list;
	UINT resource_length;

	list = &wnd->wd->resource_list->list->partial_resource_list;
	resource_length = sizeof(struct cm_partial_resource_list) +
		sizeof(struct cm_partial_resource_descriptor) *
		(list->count - 1);
	TRACE2("%p, %p,%d (%d), %p %d %d", wnd, resource_list, *size,
	       resource_length, &list->partial_descriptors[list->count-1],
	       list->partial_descriptors[list->count-1].u.interrupt.level,
	       list->partial_descriptors[list->count-1].u.interrupt.vector);
	if (*size < sizeof(*list)) {
		*size = resource_length;
		*status = NDIS_STATUS_BUFFER_TOO_SHORT;
	} else {
		ULONG count;
		if (*size >= resource_length) {
			*size = resource_length;
			count = list->count;
		} else {
			UINT n = sizeof(*list);
			count = 1;
			while (count++ < list->count && n < *size)
				n += sizeof(list->partial_descriptors);
			*size = n;
		}
		memcpy(resource_list, list, *size);
		resource_list->count = count;
		*status = NDIS_STATUS_SUCCESS;
	}
	EXIT2(return);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMPciAssignResources,3)
	(struct ndis_mp_block *nmb, ULONG slot_number,
	 NDIS_RESOURCE_LIST **resources)
{
	struct ndis_device *wnd = nmb->wnd;

	ENTER2("%p, %p", wnd, wnd->wd->resource_list);
	*resources = &wnd->wd->resource_list->list->partial_resource_list;
	EXIT2(return NDIS_STATUS_SUCCESS);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMMapIoSpace,4)
	(void __iomem **virt, struct ndis_mp_block *nmb,
	 NDIS_PHY_ADDRESS phy_addr, UINT len)
{
	struct ndis_device *wnd = nmb->wnd;

	ENTER2("%llx, %d", phy_addr, len);
	*virt = MmMapIoSpace(phy_addr, len, MmCached);
	if (*virt == NULL) {
		ERROR("ioremap failed");
		EXIT2(return NDIS_STATUS_FAILURE);
	}
	wnd->mem_start = phy_addr;
	wnd->mem_end = phy_addr + len;
	TRACE2("%p", *virt);
	EXIT2(return NDIS_STATUS_SUCCESS);
}

wstdcall void WIN_FUNC(NdisMUnmapIoSpace,3)
	(struct ndis_mp_block *nmb, void __iomem *virt, UINT len)
{
	ENTER2("%p, %d", virt, len);
	MmUnmapIoSpace(virt, len);
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisAllocateSpinLock,1)
	(struct ndis_spinlock *lock)
{
	TRACE4("lock %p, %p", lock, &lock->klock);
	KeInitializeSpinLock(&lock->klock);
	lock->irql = PASSIVE_LEVEL;
	return;
}

wstdcall void WIN_FUNC(NdisFreeSpinLock,1)
	(struct ndis_spinlock *lock)
{
	TRACE4("lock %p, %p", lock, &lock->klock);
	return;
}

wstdcall void WIN_FUNC(NdisAcquireSpinLock,1)
	(struct ndis_spinlock *lock)
{
	ENTER6("lock %p, %p", lock, &lock->klock);
//	assert_irql(_irql_ <= DISPATCH_LEVEL);
	lock->irql = nt_spin_lock_irql(&lock->klock, DISPATCH_LEVEL);
	return;
}

wstdcall void WIN_FUNC(NdisReleaseSpinLock,1)
	(struct ndis_spinlock *lock)
{
	ENTER6("lock %p, %p", lock, &lock->klock);
//	assert_irql(_irql_ == DISPATCH_LEVEL);
	nt_spin_unlock_irql(&lock->klock, lock->irql);
	return;
}

wstdcall void WIN_FUNC(NdisDprAcquireSpinLock,1)
	(struct ndis_spinlock *lock)
{
	ENTER6("lock %p", &lock->klock);
//	assert_irql(_irql_ == DISPATCH_LEVEL);
	nt_spin_lock(&lock->klock);
	return;
}

wstdcall void WIN_FUNC(NdisDprReleaseSpinLock,1)
	(struct ndis_spinlock *lock)
{
	ENTER6("lock %p", &lock->klock);
//	assert_irql(_irql_ == DISPATCH_LEVEL);
	nt_spin_unlock(&lock->klock);
	return;
}

wstdcall void WIN_FUNC(NdisInitializeReadWriteLock,1)
	(struct ndis_rw_lock *rw_lock)
{
	ENTER3("%p", rw_lock);
	memset(rw_lock, 0, sizeof(*rw_lock));
	KeInitializeSpinLock(&rw_lock->klock);
	return;
}

/* read/write locks are implemented in a rather simplistic way - we
 * should probably use Linux's rw_lock implementation */

wstdcall void WIN_FUNC(NdisAcquireReadWriteLock,3)
	(struct ndis_rw_lock *rw_lock, BOOLEAN write,
	 struct lock_state *lock_state)
{
	if (write) {
		while (1) {
			if (cmpxchg(&rw_lock->count, 0, -1) == 0)
				return;
			while (rw_lock->count)
				cpu_relax();
		}
		return;
	}
	while (1) {
		typeof(rw_lock->count) count;
		while ((count = rw_lock->count) < 0)
			cpu_relax();
		if (cmpxchg(&rw_lock->count, count, count + 1) == count)
			return;
	}
}

wstdcall void WIN_FUNC(NdisReleaseReadWriteLock,2)
	(struct ndis_rw_lock *rw_lock, struct lock_state *lock_state)
{
	if (rw_lock->count > 0)
		pre_atomic_add(rw_lock->count, -1);
	else if (rw_lock->count == -1)
		rw_lock->count = 0;
	else
		WARNING("invalid state: %d", rw_lock->count);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMAllocateMapRegisters,5)
	(struct ndis_mp_block *nmb, UINT dmachan,
	 NDIS_DMA_SIZE dmasize, ULONG basemap, ULONG max_buf_size)
{
	struct ndis_device *wnd = nmb->wnd;

	ENTER2("%p, %d %d %d %d", wnd, dmachan, dmasize, basemap, max_buf_size);
	if (!wrap_is_pci_bus(wnd->wd->dev_bus)) {
		ERROR("used on a non-PCI device");
		return NDIS_STATUS_NOT_SUPPORTED;
	}
	if (wnd->dma_map_count > 0) {
		WARNING("%s: map registers already allocated: %u",
			wnd->net_dev->name, wnd->dma_map_count);
		EXIT2(return NDIS_STATUS_RESOURCES);
	}
	if (dmasize == NDIS_DMA_24BITS) {
		if (pci_set_dma_mask(wnd->wd->pci.pdev, DMA_BIT_MASK(24)) ||
		    pci_set_consistent_dma_mask(wnd->wd->pci.pdev,
						DMA_BIT_MASK(24)))
			WARNING("setting dma mask failed");
	} else if (dmasize == NDIS_DMA_32BITS) {
		/* consistent dma is in low 32-bits by default */
		if (pci_set_dma_mask(wnd->wd->pci.pdev, DMA_BIT_MASK(32)))
			WARNING("setting dma mask failed");
#ifdef CONFIG_X86_64
	} else if (dmasize == NDIS_DMA_64BITS) {
		if (pci_set_dma_mask(wnd->wd->pci.pdev, DMA_BIT_MASK(64)) ||
		    pci_set_consistent_dma_mask(wnd->wd->pci.pdev,
						DMA_BIT_MASK(64)))
			WARNING("setting dma mask failed");
		else
			wnd->net_dev->features |= NETIF_F_HIGHDMA;
#endif
	} else {
		ERROR("dmasize %d not supported", dmasize);
		EXIT2(return NDIS_STATUS_NOT_SUPPORTED);
	}
	/* since memory for buffer is allocated with kmalloc, buffer
	 * is physically contiguous, so entire map will fit in one
	 * register */
	if (basemap > 64) {
		WARNING("Windows driver %s requesting too many (%u) "
			"map registers", wnd->wd->driver->name, basemap);
		/* As per NDIS, NDIS_STATUS_RESOURCES should be
		 * returned, but with that Atheros PCI driver fails -
		 * for now tolerate it */
//		EXIT2(return NDIS_STATUS_RESOURCES);
	}

	wnd->dma_map_addr = kzalloc(basemap * sizeof(*(wnd->dma_map_addr)),
				    GFP_KERNEL);
	if (!wnd->dma_map_addr)
		EXIT2(return NDIS_STATUS_RESOURCES);
	wnd->dma_map_count = basemap;
	TRACE2("%u", wnd->dma_map_count);
	EXIT2(return NDIS_STATUS_SUCCESS);
}

wstdcall void WIN_FUNC(NdisMFreeMapRegisters,1)
	(struct ndis_mp_block *nmb)
{
	struct ndis_device *wnd = nmb->wnd;
	int i;

	ENTER2("wnd: %p", wnd);
	if (wnd->dma_map_addr) {
		for (i = 0; i < wnd->dma_map_count; i++) {
			if (wnd->dma_map_addr[i])
				WARNING("%s: dma addr 0x%llx not freed by "
					"Windows driver", wnd->net_dev->name,
					(unsigned long long)wnd->dma_map_addr[i]);
		}
		kfree(wnd->dma_map_addr);
		wnd->dma_map_addr = NULL;
	} else
		WARNING("map registers already freed?");
	wnd->dma_map_count = 0;
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisMStartBufferPhysicalMapping,6)
	(struct ndis_mp_block *nmb, ndis_buffer *buf,
	 ULONG index, BOOLEAN write_to_dev,
	 struct ndis_phy_addr_unit *phy_addr_array, UINT *array_size)
{
	struct ndis_device *wnd = nmb->wnd;

	ENTER3("%p, %p, %u, %u", wnd, buf, index, wnd->dma_map_count);
	if (!wrap_is_pci_bus(wnd->wd->dev_bus)) {
		ERROR("used on a non-PCI device");
		return;
	}
	if (unlikely(wnd->sg_dma_size || !write_to_dev ||
		     index >= wnd->dma_map_count)) {
		WARNING("invalid request: %d, %d, %d, %d", wnd->sg_dma_size,
			write_to_dev, index, wnd->dma_map_count);
		phy_addr_array[0].phy_addr = 0;
		phy_addr_array[0].length = 0;
		*array_size = 0;
		return;
	}
	if (wnd->dma_map_addr[index]) {
		TRACE2("buffer %p at %d is already mapped: %llx", buf, index,
		       (unsigned long long)wnd->dma_map_addr[index]);
//		*array_size = 1;
		return;
	}
	TRACE3("%p, %p, %u", buf, MmGetSystemAddressForMdl(buf),
	       MmGetMdlByteCount(buf));
	DBG_BLOCK(4) {
		dump_bytes(__func__, MmGetSystemAddressForMdl(buf),
			   MmGetMdlByteCount(buf));
	}
	wnd->dma_map_addr[index] =
		PCI_DMA_MAP_SINGLE(wnd->wd->pci.pdev,
				   MmGetSystemAddressForMdl(buf),
				   MmGetMdlByteCount(buf), PCI_DMA_TODEVICE);
	phy_addr_array[0].phy_addr = wnd->dma_map_addr[index];
	phy_addr_array[0].length = MmGetMdlByteCount(buf);
	TRACE4("%llx, %d, %d", phy_addr_array[0].phy_addr,
	       phy_addr_array[0].length, index);
	*array_size = 1;
}

wstdcall void WIN_FUNC(NdisMCompleteBufferPhysicalMapping,3)
	(struct ndis_mp_block *nmb, ndis_buffer *buf, ULONG index)
{
	struct ndis_device *wnd = nmb->wnd;

	ENTER3("%p, %p %u (%u)", wnd, buf, index, wnd->dma_map_count);

	if (!wrap_is_pci_bus(wnd->wd->dev_bus)) {
		ERROR("used on a non-PCI device");
		return;
	}
	if (unlikely(wnd->sg_dma_size))
		WARNING("buffer %p may have been unmapped already", buf);
	if (index >= wnd->dma_map_count) {
		ERROR("invalid map register (%u >= %u)",
		      index, wnd->dma_map_count);
		return;
	}
	TRACE4("%llx", (unsigned long long)wnd->dma_map_addr[index]);
	if (wnd->dma_map_addr[index]) {
		PCI_DMA_UNMAP_SINGLE(wnd->wd->pci.pdev, wnd->dma_map_addr[index],
				     MmGetMdlByteCount(buf), PCI_DMA_TODEVICE);
		wnd->dma_map_addr[index] = 0;
	} else
		WARNING("map registers at %u not used", index);
}

wstdcall void WIN_FUNC(NdisMAllocateSharedMemory,5)
	(struct ndis_mp_block *nmb, ULONG size,
	 BOOLEAN cached, void **virt, NDIS_PHY_ADDRESS *phys)
{
	dma_addr_t dma_addr;
	struct wrap_device *wd = nmb->wnd->wd;

	ENTER3("size: %u, cached: %d", size, cached);
	if (!wrap_is_pci_bus(wd->dev_bus)) {
		ERROR("used on a non-PCI device");
		return;
	}
	*virt = PCI_DMA_ALLOC_COHERENT(wd->pci.pdev, size, &dma_addr);
	if (*virt)
		*phys = dma_addr;
	else
		WARNING("couldn't allocate %d bytes of %scached DMA memory",
			size, cached ? "" : "un-");
	EXIT3(return);
}

wstdcall void WIN_FUNC(NdisMFreeSharedMemory,5)
	(struct ndis_mp_block *nmb, ULONG size, BOOLEAN cached,
	 void *virt, NDIS_PHY_ADDRESS addr)
{
	struct wrap_device *wd = nmb->wnd->wd;
	ENTER3("%p, %llx, %u", virt, addr, size);
	if (!wrap_is_pci_bus(wd->dev_bus)) {
		ERROR("used on a non-PCI device");
		return;
	}
	PCI_DMA_FREE_COHERENT(wd->pci.pdev, size, virt, addr);
	EXIT3(return);
}

wstdcall void alloc_shared_memory_async(void *arg1, void *arg2)
{
	struct ndis_device *wnd;
	struct alloc_shared_mem *alloc_shared_mem;
	struct miniport *mp;
	void *virt;
	NDIS_PHY_ADDRESS phys;
	KIRQL irql;

	wnd = arg1;
	alloc_shared_mem = arg2;
	mp = &wnd->wd->driver->ndis_driver->mp;
	NdisMAllocateSharedMemory(wnd->nmb, alloc_shared_mem->size,
				  alloc_shared_mem->cached, &virt, &phys);
	irql = serialize_lock_irql(wnd);
	assert_irql(_irql_ == DISPATCH_LEVEL);
	LIN2WIN5(mp->alloc_complete, wnd->nmb, virt,
		 &phys, alloc_shared_mem->size, alloc_shared_mem->ctx);
	serialize_unlock_irql(wnd, irql);
	kfree(alloc_shared_mem);
}
WIN_FUNC_DECL(alloc_shared_memory_async,2)

wstdcall NDIS_STATUS WIN_FUNC(NdisMAllocateSharedMemoryAsync,4)
	(struct ndis_mp_block *nmb, ULONG size, BOOLEAN cached, void *ctx)
{
	struct ndis_device *wnd = nmb->wnd;
	struct alloc_shared_mem *alloc_shared_mem;

	ENTER3("wnd: %p", wnd);
	alloc_shared_mem = kmalloc(sizeof(*alloc_shared_mem), irql_gfp());
	if (!alloc_shared_mem) {
		WARNING("couldn't allocate memory");
		return NDIS_STATUS_FAILURE;
	}

	alloc_shared_mem->size = size;
	alloc_shared_mem->cached = cached;
	alloc_shared_mem->ctx = ctx;
	if (schedule_ntos_work_item(WIN_FUNC_PTR(alloc_shared_memory_async,2),
				    wnd, alloc_shared_mem))
		EXIT3(return NDIS_STATUS_FAILURE);
	EXIT3(return NDIS_STATUS_PENDING);
}

/* Some drivers allocate NDIS_BUFFER (aka MDL) very often; instead of
 * allocating and freeing with kernel functions, we chain them into
 * ndis_buffer_pool. When an MDL is freed, it is added to the list of
 * free MDLs. When allocated, we first check if there is one in free
 * list and if so just return it; otherwise, we allocate a new one and
 * return that. This reduces memory fragmentation. Windows DDK says
 * that the driver itself shouldn't check what is returned in
 * pool_handle, presumably because buffer pools are not used in
 * XP. However, as long as driver follows rest of the semantics - that
 * it should indicate maximum number of MDLs used with num_descr and
 * pass the same pool_handle in other buffer functions, this should
 * work. Sadly, though, NdisFreeBuffer doesn't pass the pool_handle,
 * so we use 'process' field of MDL to store pool_handle. */

wstdcall void WIN_FUNC(NdisAllocateBufferPool,3)
	(NDIS_STATUS *status, struct ndis_buffer_pool **pool_handle,
	 UINT num_descr)
{
	struct ndis_buffer_pool *pool;

	ENTER1("buffers: %d", num_descr);
	pool = kmalloc(sizeof(*pool), irql_gfp());
	if (!pool) {
		*status = NDIS_STATUS_RESOURCES;
		EXIT3(return);
	}
	spin_lock_init(&pool->lock);
	pool->max_descr = num_descr;
	pool->num_allocated_descr = 0;
	pool->free_descr = NULL;
	*pool_handle = pool;
	*status = NDIS_STATUS_SUCCESS;
	TRACE1("pool: %p, num_descr: %d", pool, num_descr);
	EXIT1(return);
}

wstdcall void WIN_FUNC(NdisAllocateBuffer,5)
	(NDIS_STATUS *status, ndis_buffer **buffer,
	 struct ndis_buffer_pool *pool, void *virt, UINT length)
{
	ndis_buffer *descr;

	ENTER4("pool: %p (%d)", pool, pool->num_allocated_descr);
	/* NDIS drivers should call this at DISPATCH_LEVEL, but
	 * alloc_tx_packet calls at SOFT_IRQL */
	assert_irql(_irql_ <= SOFT_LEVEL);
	if (!pool) {
		*status = NDIS_STATUS_FAILURE;
		*buffer = NULL;
		EXIT4(return);
	}
	spin_lock_bh(&pool->lock);
	if ((descr = pool->free_descr))
		pool->free_descr = descr->next;
	spin_unlock_bh(&pool->lock);
	if (descr) {
		typeof(descr->flags) flags;
		flags = descr->flags;
		memset(descr, 0, sizeof(*descr));
		MmInitializeMdl(descr, virt, length);
		if (flags & MDL_CACHE_ALLOCATED)
			descr->flags |= MDL_CACHE_ALLOCATED;
	} else {
		if (pool->num_allocated_descr > pool->max_descr) {
			TRACE2("pool %p is full: %d(%d)", pool,
			       pool->num_allocated_descr, pool->max_descr);
#ifndef ALLOW_POOL_OVERFLOW
			*status = NDIS_STATUS_FAILURE;
			*buffer = NULL;
			return;
#endif
		}
		descr = allocate_init_mdl(virt, length);
		if (!descr) {
			WARNING("couldn't allocate buffer");
			*status = NDIS_STATUS_FAILURE;
			*buffer = NULL;
			EXIT4(return);
		}
		TRACE4("buffer %p for %p, %d", descr, virt, length);
		atomic_inc_var(pool->num_allocated_descr);
	}
	/* TODO: make sure this mdl can map given buffer */
	MmBuildMdlForNonPagedPool(descr);
//	descr->flags |= MDL_ALLOCATED_FIXED_SIZE |
//		MDL_MAPPED_TO_SYSTEM_VA | MDL_PAGES_LOCKED;
	descr->pool = pool;
	*buffer = descr;
	*status = NDIS_STATUS_SUCCESS;
	TRACE4("buffer: %p", descr);
	EXIT4(return);
}

wstdcall void WIN_FUNC(NdisFreeBuffer,1)
	(ndis_buffer *buffer)
{
	struct ndis_buffer_pool *pool;

	ENTER4("%p", buffer);
	if (!buffer || !buffer->pool) {
		ERROR("invalid buffer");
		EXIT4(return);
	}
	pool = buffer->pool;
	if (pool->num_allocated_descr > MAX_ALLOCATED_NDIS_BUFFERS) {
		/* NB NB NB: set mdl's 'pool' field to NULL before
		 * calling free_mdl; otherwise free_mdl calls
		 * NdisFreeBuffer back */
		atomic_dec_var(pool->num_allocated_descr);
		buffer->pool = NULL;
		free_mdl(buffer);
	} else {
		spin_lock_bh(&pool->lock);
		buffer->next = pool->free_descr;
		pool->free_descr = buffer;
		spin_unlock_bh(&pool->lock);
	}
	EXIT4(return);
}

wstdcall void WIN_FUNC(NdisFreeBufferPool,1)
	(struct ndis_buffer_pool *pool)
{
	ndis_buffer *cur, *next;

	TRACE3("pool: %p", pool);
	if (!pool) {
		WARNING("invalid pool");
		EXIT3(return);
	}
	spin_lock_bh(&pool->lock);
	cur = pool->free_descr;
	while (cur) {
		next = cur->next;
		cur->pool = NULL;
		free_mdl(cur);
		cur = next;
	}
	spin_unlock_bh(&pool->lock);
	kfree(pool);
	pool = NULL;
	EXIT3(return);
}

wstdcall void WIN_FUNC(NdisAdjustBufferLength,2)
	(ndis_buffer *buffer, UINT length)
{
	ENTER4("%p, %d", buffer, length);
	buffer->bytecount = length;
}

wstdcall void WIN_FUNC(NdisQueryBuffer,3)
	(ndis_buffer *buffer, void **virt, UINT *length)
{
	ENTER4("buffer: %p", buffer);
	if (virt)
		*virt = MmGetSystemAddressForMdl(buffer);
	*length = MmGetMdlByteCount(buffer);
	TRACE4("%p, %u", virt ? *virt : NULL, *length);
	return;
}

wstdcall void WIN_FUNC(NdisQueryBufferSafe,4)
	(ndis_buffer *buffer, void **virt, UINT *length,
	 enum mm_page_priority priority)
{
	ENTER4("%p, %p, %p, %d", buffer, virt, length, priority);
	if (virt)
		*virt = MmGetSystemAddressForMdlSafe(buffer, priority);
	*length = MmGetMdlByteCount(buffer);
	TRACE4("%p, %u", virt ? *virt : NULL, *length);
}

wstdcall void *WIN_FUNC(NdisBufferVirtualAddress,1)
	(ndis_buffer *buffer)
{
	ENTER3("%p", buffer);
	return MmGetSystemAddressForMdl(buffer);
}

wstdcall ULONG WIN_FUNC(NdisBufferLength,1)
	(ndis_buffer *buffer)
{
	ENTER3("%p", buffer);
	return MmGetMdlByteCount(buffer);
}

wstdcall void WIN_FUNC(NdisQueryBufferOffset,3)
	(ndis_buffer *buffer, UINT *offset, UINT *length)
{
	ENTER3("%p", buffer);
	*offset = MmGetMdlByteOffset(buffer);
	*length = MmGetMdlByteCount(buffer);
	TRACE3("%d, %d", *offset, *length);
}

wstdcall void WIN_FUNC(NdisUnchainBufferAtBack,2)
	(struct ndis_packet *packet, ndis_buffer **buffer)
{
	ndis_buffer *b, *btail;

	ENTER3("%p", packet);
	b = packet->private.buffer_head;
	if (!b) {
		/* no buffer in packet */
		*buffer = NULL;
		EXIT3(return);
	}
	btail = packet->private.buffer_tail;
	*buffer = btail;
	if (b == btail) {
		/* one buffer in packet */
		packet->private.buffer_head = NULL;
		packet->private.buffer_tail = NULL;
	} else {
		while (b->next != btail)
			b = b->next;
		packet->private.buffer_tail = b;
		b->next = NULL;
	}
	packet->private.valid_counts = FALSE;
	EXIT3(return);
}

wstdcall void WIN_FUNC(NdisUnchainBufferAtFront,2)
	(struct ndis_packet *packet, ndis_buffer **buffer)
{
	ENTER3("%p", packet);
	if (packet->private.buffer_head == NULL) {
		/* no buffer in packet */
		*buffer = NULL;
		EXIT3(return);
	}

	*buffer = packet->private.buffer_head;
	if (packet->private.buffer_head == packet->private.buffer_tail) {
		/* one buffer in packet */
		packet->private.buffer_head = NULL;
		packet->private.buffer_tail = NULL;
	} else
		packet->private.buffer_head = (*buffer)->next;

	packet->private.valid_counts = FALSE;
	EXIT3(return);
}

wstdcall void WIN_FUNC(NdisGetFirstBufferFromPacketSafe,6)
	(struct ndis_packet *packet, ndis_buffer **first_buffer,
	 void **first_buffer_va, UINT *first_buffer_length,
	 UINT *total_buffer_length, enum mm_page_priority priority)
{
	ndis_buffer *b = packet->private.buffer_head;

	ENTER3("%p(%p)", packet, b);
	*first_buffer = b;
	if (b) {
		*first_buffer_va = MmGetSystemAddressForMdlSafe(b, priority);
		*first_buffer_length = *total_buffer_length =
			MmGetMdlByteCount(b);
		for (b = b->next; b; b = b->next)
			*total_buffer_length += MmGetMdlByteCount(b);
	} else {
		*first_buffer_va = NULL;
		*first_buffer_length = 0;
		*total_buffer_length = 0;
	}
	TRACE3("%p, %d, %d", *first_buffer_va, *first_buffer_length,
	       *total_buffer_length);
	EXIT3(return);
}

wstdcall void WIN_FUNC(NdisGetFirstBufferFromPacket,6)
	(struct ndis_packet *packet, ndis_buffer **first_buffer,
	 void **first_buffer_va, UINT *first_buffer_length,
	 UINT *total_buffer_length, enum mm_page_priority priority)
{
	NdisGetFirstBufferFromPacketSafe(packet, first_buffer,
					 first_buffer_va, first_buffer_length,
					 total_buffer_length,
					 NormalPagePriority);
}

wstdcall void WIN_FUNC(NdisAllocatePacketPoolEx,5)
	(NDIS_STATUS *status, struct ndis_packet_pool **pool_handle,
	 UINT num_descr, UINT overflowsize, UINT proto_rsvd_length)
{
	struct ndis_packet_pool *pool;

	ENTER3("buffers: %d, length: %d", num_descr, proto_rsvd_length);
	pool = kzalloc(sizeof(*pool), irql_gfp());
	if (!pool) {
		*status = NDIS_STATUS_RESOURCES;
		EXIT3(return);
	}
	spin_lock_init(&pool->lock);
	pool->max_descr = num_descr;
	pool->num_allocated_descr = 0;
	pool->num_used_descr = 0;
	pool->free_descr = NULL;
	pool->proto_rsvd_length = proto_rsvd_length;
	*pool_handle = pool;
	*status = NDIS_STATUS_SUCCESS;
	TRACE3("pool: %p", pool);
	EXIT3(return);
}

wstdcall void WIN_FUNC(NdisAllocatePacketPool,4)
	(NDIS_STATUS *status, struct ndis_packet_pool **pool_handle,
	 UINT num_descr, UINT proto_rsvd_length)
{
	NdisAllocatePacketPoolEx(status, pool_handle, num_descr, 0,
				 proto_rsvd_length);
	EXIT3(return);
}

wstdcall void WIN_FUNC(NdisFreePacketPool,1)
	(struct ndis_packet_pool *pool)
{
	struct ndis_packet *packet, *next;

	ENTER3("pool: %p", pool);
	if (!pool) {
		WARNING("invalid pool");
		EXIT3(return);
	}
	spin_lock_bh(&pool->lock);
	packet = pool->free_descr;
	while (packet) {
		next = (struct ndis_packet *)packet->reserved[0];
		kfree(packet);
		packet = next;
	}
	pool->num_allocated_descr = 0;
	pool->num_used_descr = 0;
	pool->free_descr = NULL;
	spin_unlock_bh(&pool->lock);
	kfree(pool);
	EXIT3(return);
}

wstdcall UINT WIN_FUNC(NdisPacketPoolUsage,1)
	(struct ndis_packet_pool *pool)
{
	EXIT4(return pool->num_used_descr);
}

wstdcall void WIN_FUNC(NdisAllocatePacket,3)
	(NDIS_STATUS *status, struct ndis_packet **ndis_packet,
	 struct ndis_packet_pool *pool)
{
	struct ndis_packet *packet;
	int packet_length;

	ENTER4("pool: %p", pool);
	if (!pool) {
		*status = NDIS_STATUS_RESOURCES;
		*ndis_packet = NULL;
		EXIT4(return);
	}
	assert_irql(_irql_ <= SOFT_LEVEL);
	if (pool->num_used_descr > pool->max_descr) {
		TRACE3("pool %p is full: %d(%d)", pool,
		       pool->num_used_descr, pool->max_descr);
#ifndef ALLOW_POOL_OVERFLOW
		*status = NDIS_STATUS_RESOURCES;
		*ndis_packet = NULL;
		return;
#endif
	}
	/* packet has space for 1 byte in protocol_reserved field */
	packet_length = sizeof(*packet) - 1 + pool->proto_rsvd_length +
		sizeof(struct ndis_packet_oob_data);
	spin_lock_bh(&pool->lock);
	if ((packet = pool->free_descr))
		pool->free_descr = (void *)packet->reserved[0];
	spin_unlock_bh(&pool->lock);
	if (!packet) {
		packet = kmalloc(packet_length, irql_gfp());
		if (!packet) {
			WARNING("couldn't allocate packet");
			*status = NDIS_STATUS_RESOURCES;
			*ndis_packet = NULL;
			return;
		}
		atomic_inc_var(pool->num_allocated_descr);
	}
	TRACE4("%p, %p", pool, packet);
	atomic_inc_var(pool->num_used_descr);
	memset(packet, 0, packet_length);
	packet->private.oob_offset =
		packet_length - sizeof(struct ndis_packet_oob_data);
	packet->private.packet_flags = fPACKET_ALLOCATED_BY_NDIS;
	packet->private.pool = pool;
	*ndis_packet = packet;
	*status = NDIS_STATUS_SUCCESS;
	EXIT4(return);
}

wstdcall void WIN_FUNC(NdisDprAllocatePacket,3)
	(NDIS_STATUS *status, struct ndis_packet **packet,
	 struct ndis_packet_pool *pool)
{
	NdisAllocatePacket(status, packet, pool);
}

wstdcall void WIN_FUNC(NdisFreePacket,1)
	(struct ndis_packet *packet)
{
	struct ndis_packet_pool *pool;

	ENTER4("%p, %p", packet, packet->private.pool);
	pool = packet->private.pool;
	if (!pool) {
		ERROR("invalid pool %p", packet);
		EXIT4(return);
	}
	assert((int)pool->num_used_descr > 0);
	atomic_dec_var(pool->num_used_descr);
	if (packet->reserved[1]) {
		TRACE3("%p, %p", packet, (void *)packet->reserved[1]);
		kfree((void *)packet->reserved[1]);
		packet->reserved[1] = 0;
	}
	if (pool->num_allocated_descr > MAX_ALLOCATED_NDIS_PACKETS) {
		TRACE3("%p", pool);
		atomic_dec_var(pool->num_allocated_descr);
		kfree(packet);
	} else {
		TRACE4("%p, %p, %p", pool, packet, pool->free_descr);
		spin_lock_bh(&pool->lock);
		packet->reserved[0] =
			(typeof(packet->reserved[0]))pool->free_descr;
		pool->free_descr = packet;
		spin_unlock_bh(&pool->lock);
	}
	EXIT4(return);
}

wstdcall struct ndis_packet_stack *WIN_FUNC(NdisIMGetCurrentPacketStack,2)
	(struct ndis_packet *packet, BOOLEAN *stacks_remain)
{
	struct ndis_packet_stack *stack;

	if (!packet->reserved[1]) {
		stack = kzalloc(2 * sizeof(*stack), irql_gfp());
		TRACE3("%p, %p", packet, stack);
		packet->reserved[1] = (typeof(packet->reserved[1]))stack;
	} else {
		stack = (void *)packet->reserved[1];;
		if (xchg(&stack->ndis_reserved[0], 1)) {
			stack++;
			if (xchg(&stack->ndis_reserved[0], 1))
				stack = NULL;
		}
		TRACE3("%p", stack);
	}
	if (stack)
		*stacks_remain = TRUE;
	else
		*stacks_remain = FALSE;

	EXIT3(return stack);
}

wstdcall void WIN_FUNC(NdisCopyFromPacketToPacketSafe,7)
	(struct ndis_packet *dst, UINT dst_offset, UINT num_to_copy,
	 struct ndis_packet *src, UINT src_offset, UINT *num_copied,
	 enum mm_page_priority priority)
{
	UINT dst_n, src_n, n, left;
	ndis_buffer *dst_buf;
	ndis_buffer *src_buf;

	ENTER4("");
	if (!dst || !src) {
		*num_copied = 0;
		EXIT4(return);
	}

	dst_buf = dst->private.buffer_head;
	src_buf = src->private.buffer_head;

	if (!dst_buf || !src_buf) {
		*num_copied = 0;
		EXIT4(return);
	}
	dst_n = MmGetMdlByteCount(dst_buf) - dst_offset;
	src_n = MmGetMdlByteCount(src_buf) - src_offset;

	n = min(src_n, dst_n);
	n = min(n, num_to_copy);
	memcpy(MmGetSystemAddressForMdl(dst_buf) + dst_offset,
	       MmGetSystemAddressForMdl(src_buf) + src_offset, n);

	left = num_to_copy - n;
	while (left > 0) {
		src_offset += n;
		dst_offset += n;
		dst_n -= n;
		src_n -= n;
		if (dst_n == 0) {
			dst_buf = dst_buf->next;
			if (!dst_buf)
				break;
			dst_n = MmGetMdlByteCount(dst_buf);
			dst_offset = 0;
		}
		if (src_n == 0) {
			src_buf = src_buf->next;
			if (!src_buf)
				break;
			src_n = MmGetMdlByteCount(src_buf);
			src_offset = 0;
		}

		n = min(src_n, dst_n);
		n = min(n, left);
		memcpy(MmGetSystemAddressForMdl(dst_buf) + dst_offset,
		       MmGetSystemAddressForMdl(src_buf) + src_offset, n);
		left -= n;
	}
	*num_copied = num_to_copy - left;
	EXIT4(return);
}

wstdcall void WIN_FUNC(NdisCopyFromPacketToPacket,6)
	(struct ndis_packet *dst, UINT dst_offset, UINT num_to_copy,
	 struct ndis_packet *src, UINT src_offset, UINT *num_copied)
{
	NdisCopyFromPacketToPacketSafe(dst, dst_offset, num_to_copy,
				       src, src_offset, num_copied,
				       NormalPagePriority);
	return;
}

wstdcall void WIN_FUNC(NdisIMCopySendPerPacketInfo,2)
	(struct ndis_packet *dst, struct ndis_packet *src)
{
	struct ndis_packet_oob_data *dst_oob, *src_oob;
	dst_oob = NDIS_PACKET_OOB_DATA(dst);
	src_oob = NDIS_PACKET_OOB_DATA(src);
	memcpy(&dst_oob->ext, &src_oob->ext, sizeof(dst_oob->ext));
	return;
}

wstdcall void WIN_FUNC(NdisSend,3)
	(NDIS_STATUS *status, struct ndis_mp_block *nmb,
	 struct ndis_packet *packet)
{
	struct ndis_device *wnd = nmb->wnd;
	struct miniport *mp;
	KIRQL irql;

	mp = &wnd->wd->driver->ndis_driver->mp;
	if (mp->send_packets) {
		irql = serialize_lock_irql(wnd);
		assert_irql(_irql_ == DISPATCH_LEVEL);
		LIN2WIN3(mp->send_packets, wnd->nmb->mp_ctx, &packet, 1);
		serialize_unlock_irql(wnd, irql);
		if (deserialized_driver(wnd))
			*status = NDIS_STATUS_PENDING;
		else {
			struct ndis_packet_oob_data *oob_data;
			oob_data = NDIS_PACKET_OOB_DATA(packet);
			*status = oob_data->status;
			switch (*status) {
			case NDIS_STATUS_SUCCESS:
				free_tx_packet(wnd, packet, *status);
				break;
			case NDIS_STATUS_PENDING:
				break;
			case NDIS_STATUS_RESOURCES:
				wnd->tx_ok = 0;
				break;
			case NDIS_STATUS_FAILURE:
			default:
				free_tx_packet(wnd, packet, *status);
				break;
			}
		}
	} else {
		irql = serialize_lock_irql(wnd);
		assert_irql(_irql_ == DISPATCH_LEVEL);
		*status = LIN2WIN3(mp->send, wnd->nmb->mp_ctx, packet, 0);
		serialize_unlock_irql(wnd, irql);
		switch (*status) {
		case NDIS_STATUS_SUCCESS:
			free_tx_packet(wnd, packet, *status);
			break;
		case NDIS_STATUS_PENDING:
			break;
		case NDIS_STATUS_RESOURCES:
			wnd->tx_ok = 0;
			break;
		case NDIS_STATUS_FAILURE:
		default:
			free_tx_packet(wnd, packet, *status);
			break;
		}
	}
	EXIT3(return);
}

/* called for serialized drivers only */
wstdcall void mp_timer_dpc(struct kdpc *kdpc, void *ctx, void *arg1, void *arg2)
{
	struct ndis_mp_timer *timer;
	struct ndis_mp_block *nmb;

	timer = ctx;
	TIMERENTER("%p, %p, %p, %p", timer, timer->func, timer->ctx, timer->nmb);
	assert_irql(_irql_ == DISPATCH_LEVEL);
	nmb = timer->nmb;
	serialize_lock(nmb->wnd);
	LIN2WIN4(timer->func, NULL, timer->ctx, NULL, NULL);
	serialize_unlock(nmb->wnd);
	TIMEREXIT(return);
}
WIN_FUNC_DECL(mp_timer_dpc,4)

wstdcall void WIN_FUNC(NdisMInitializeTimer,4)
	(struct ndis_mp_timer *timer, struct ndis_mp_block *nmb,
	 DPC func, void *ctx)
{
	TIMERENTER("%p, %p, %p, %p", timer, func, ctx, nmb);
	assert_irql(_irql_ == PASSIVE_LEVEL);
	timer->func = func;
	timer->ctx = ctx;
	timer->nmb = nmb;
	if (deserialized_driver(nmb->wnd))
		KeInitializeDpc(&timer->kdpc, func, ctx);
	else
		KeInitializeDpc(&timer->kdpc, WIN_FUNC_PTR(mp_timer_dpc,4),
				timer);
	wrap_init_timer(&timer->nt_timer, NotificationTimer, nmb);
	TIMEREXIT(return);
}

wstdcall void WIN_FUNC(NdisMSetPeriodicTimer,2)
	(struct ndis_mp_timer *timer, UINT period_ms)
{
	unsigned long expires = MSEC_TO_HZ(period_ms);

	TIMERENTER("%p, %u, %ld", timer, period_ms, expires);
	assert_irql(_irql_ <= DISPATCH_LEVEL);
	wrap_set_timer(&timer->nt_timer, expires, expires, &timer->kdpc);
	TIMEREXIT(return);
}

wstdcall void WIN_FUNC(NdisMCancelTimer,2)
	(struct ndis_mp_timer *timer, BOOLEAN *canceled)
{
	TIMERENTER("%p", timer);
	assert_irql(_irql_ <= DISPATCH_LEVEL);
	*canceled = KeCancelTimer(&timer->nt_timer);
	TIMERTRACE("%d", *canceled);
	return;
}

wstdcall void WIN_FUNC(NdisInitializeTimer,3)
	(struct ndis_timer *timer, void *func, void *ctx)
{
	TIMERENTER("%p, %p, %p", timer, func, ctx);
	assert_irql(_irql_ == PASSIVE_LEVEL);
	KeInitializeDpc(&timer->kdpc, func, ctx);
	wrap_init_timer(&timer->nt_timer, NotificationTimer, NULL);
	TIMEREXIT(return);
}

/* NdisMSetTimer is a macro that calls NdisSetTimer with
 * ndis_mp_timer typecast to ndis_timer */

wstdcall void WIN_FUNC(NdisSetTimer,2)
	(struct ndis_timer *timer, UINT duetime_ms)
{
	unsigned long expires = MSEC_TO_HZ(duetime_ms);

	TIMERENTER("%p, %p, %u, %ld", timer, timer->nt_timer.wrap_timer,
		   duetime_ms, expires);
	assert_irql(_irql_ <= DISPATCH_LEVEL);
	wrap_set_timer(&timer->nt_timer, expires, 0, &timer->kdpc);
	TIMEREXIT(return);
}

wstdcall void WIN_FUNC(NdisCancelTimer,2)
	(struct ndis_timer *timer, BOOLEAN *canceled)
{
	TIMERENTER("%p", timer);
	assert_irql(_irql_ <= DISPATCH_LEVEL);
	*canceled = KeCancelTimer(&timer->nt_timer);
	TIMEREXIT(return);
}

wstdcall void WIN_FUNC(NdisMRegisterAdapterShutdownHandler,3)
	(struct ndis_mp_block *nmb, void *ctx, void *func)
{
	struct ndis_device *wnd = nmb->wnd;
	ENTER1("%p", func);
	wnd->wd->driver->ndis_driver->mp.shutdown = func;
	wnd->shutdown_ctx = ctx;
}

wstdcall void WIN_FUNC(NdisMDeregisterAdapterShutdownHandler,1)
	(struct ndis_mp_block *nmb)
{
	struct ndis_device *wnd = nmb->wnd;
	wnd->wd->driver->ndis_driver->mp.shutdown = NULL;
	wnd->shutdown_ctx = NULL;
}

/* TODO: rt61 (serialized) driver doesn't want MiniportEnableInterrupt
 * to be called in irq handler, but mrv800c (deserialized) driver
 * wants. NDIS is confusing about when to call MiniportEnableInterrupt
 * For now, handle these cases with two separate irq handlers based on
 * observation of these two drivers. However, it is likely not
 * correct. */
wstdcall void deserialized_irq_handler(struct kdpc *kdpc, void *ctx,
				       void *arg1, void *arg2)
{
	struct ndis_device *wnd = ctx;
	ndis_interrupt_handler irq_handler = arg1;
	struct miniport *mp = arg2;

	TRACE6("%p", irq_handler);
	assert_irql(_irql_ == DISPATCH_LEVEL);
	LIN2WIN1(irq_handler, wnd->nmb->mp_ctx);
	if (mp->enable_interrupt)
		LIN2WIN1(mp->enable_interrupt, wnd->nmb->mp_ctx);
	EXIT6(return);
}
WIN_FUNC_DECL(deserialized_irq_handler,4)

wstdcall void serialized_irq_handler(struct kdpc *kdpc, void *ctx,
				     void *arg1, void *arg2)
{
	struct ndis_device *wnd = ctx;
	ndis_interrupt_handler irq_handler = arg1;

	TRACE6("%p, %p, %p", wnd, irq_handler, arg2);
	assert_irql(_irql_ == DISPATCH_LEVEL);
	serialize_lock(wnd);
	LIN2WIN1(irq_handler, arg2);
	serialize_unlock(wnd);
	EXIT6(return);
}
WIN_FUNC_DECL(serialized_irq_handler,4)

wstdcall BOOLEAN ndis_isr(struct kinterrupt *kinterrupt, void *ctx)
{
	struct ndis_mp_interrupt *mp_interrupt = ctx;
	struct ndis_device *wnd = mp_interrupt->nmb->wnd;
	BOOLEAN recognized = TRUE, queue_handler = TRUE;

	TRACE6("%p", wnd);
	/* kernel may call ISR when registering interrupt, in
	 * the same context if DEBUG_SHIRQ is enabled */
	assert_irql(_irql_ == DIRQL || _irql_ == PASSIVE_LEVEL);
	if (mp_interrupt->shared)
		LIN2WIN3(mp_interrupt->isr, &recognized, &queue_handler,
			 wnd->nmb->mp_ctx);
	else {
		struct miniport *mp;
		mp = &wnd->wd->driver->ndis_driver->mp;
		LIN2WIN1(mp->disable_interrupt, wnd->nmb->mp_ctx);
		/* it is not shared interrupt, so handler must be called */
		recognized = queue_handler = TRUE;
	}
	if (recognized) {
		if (queue_handler) {
			TRACE5("%p", &wnd->irq_kdpc);
			queue_kdpc(&wnd->irq_kdpc);
		}
		EXIT6(return TRUE);
	}
	EXIT6(return FALSE);
}
WIN_FUNC_DECL(ndis_isr,2)

wstdcall NDIS_STATUS WIN_FUNC(NdisMRegisterInterrupt,7)
	(struct ndis_mp_interrupt *mp_interrupt,
	 struct ndis_mp_block *nmb, UINT vector, UINT level,
	 BOOLEAN req_isr, BOOLEAN shared, enum kinterrupt_mode mode)
{
	struct ndis_device *wnd = nmb->wnd;
	struct miniport *mp;

	ENTER1("%p, vector:%d, level:%d, req_isr:%d, shared:%d, mode:%d",
	       mp_interrupt, vector, level, req_isr, shared, mode);

	mp = &wnd->wd->driver->ndis_driver->mp;
	nt_spin_lock_init(&mp_interrupt->lock);
	mp_interrupt->irq = vector;
	mp_interrupt->isr = mp->isr;
	mp_interrupt->mp_dpc = mp->handle_interrupt;
	mp_interrupt->nmb = nmb;
	mp_interrupt->req_isr = req_isr;
	if (shared && !req_isr)
		WARNING("shared but dynamic interrupt!");
	mp_interrupt->shared = shared;
	wnd->mp_interrupt = mp_interrupt;
	if (mp->enable_interrupt)
		mp_interrupt->enable = TRUE;
	else
		mp_interrupt->enable = FALSE;

	if (deserialized_driver(wnd)) {
		KeInitializeDpc(&wnd->irq_kdpc,
				WIN_FUNC_PTR(deserialized_irq_handler,4),
				nmb->wnd);
		wnd->irq_kdpc.arg1 = mp->handle_interrupt;
		wnd->irq_kdpc.arg2 = mp;
		TRACE2("%p, %p, %p, %p", wnd->irq_kdpc.arg1, wnd->irq_kdpc.arg2,
		       nmb->wnd, nmb->mp_ctx);
	} else {
		KeInitializeDpc(&wnd->irq_kdpc,
				WIN_FUNC_PTR(serialized_irq_handler,4),
				nmb->wnd);
		wnd->irq_kdpc.arg1 = mp->handle_interrupt;
		wnd->irq_kdpc.arg2 = nmb->mp_ctx;
		TRACE2("%p, %p, %p", wnd->irq_kdpc.arg1, wnd->irq_kdpc.arg2,
		       nmb->wnd);
	}

	if (IoConnectInterrupt(&mp_interrupt->kinterrupt,
			       WIN_FUNC_PTR(ndis_isr,2), mp_interrupt, NULL,
			       vector, DIRQL, DIRQL, mode, shared, 0, FALSE) !=
	    STATUS_SUCCESS) {
		printk(KERN_WARNING "%s: request for IRQ %d failed\n",
		       DRIVER_NAME, vector);
		return NDIS_STATUS_RESOURCES;
	}
	printk(KERN_INFO "%s: using IRQ %d\n", DRIVER_NAME, vector);
	EXIT1(return NDIS_STATUS_SUCCESS);
}

wstdcall void WIN_FUNC(NdisMDeregisterInterrupt,1)
	(struct ndis_mp_interrupt *mp_interrupt)
{
	struct ndis_mp_block *nmb;

	ENTER1("%p", mp_interrupt);
	nmb = xchg(&mp_interrupt->nmb, NULL);
	TRACE1("%p", nmb);
	if (!nmb) {
		WARNING("interrupt already freed?");
		return;
	}
	nmb->wnd->mp_interrupt = NULL;
	if (dequeue_kdpc(&nmb->wnd->irq_kdpc))
		TRACE2("interrupt kdpc was pending");
	flush_workqueue(wrapndis_wq);
	IoDisconnectInterrupt(mp_interrupt->kinterrupt);
	EXIT1(return);
}

wstdcall BOOLEAN WIN_FUNC(NdisMSynchronizeWithInterrupt,3)
	(struct ndis_mp_interrupt *mp_interrupt,
	 PKSYNCHRONIZE_ROUTINE sync_func, void *ctx)
{
	return KeSynchronizeExecution(mp_interrupt->kinterrupt, sync_func, ctx);
}

/* called via function pointer; but 64-bit RNDIS driver calls directly */
wstdcall void WIN_FUNC(NdisMIndicateStatus,4)
	(struct ndis_mp_block *nmb, NDIS_STATUS status, void *buf, UINT len)
{
	struct ndis_device *wnd = nmb->wnd;
	struct ndis_status_indication *si;

	ENTER2("status=0x%x len=%d", status, len);
	switch (status) {
	case NDIS_STATUS_MEDIA_CONNECT:
		set_media_state(wnd, NdisMediaStateConnected);
		break;
	case NDIS_STATUS_MEDIA_DISCONNECT:
		set_media_state(wnd, NdisMediaStateDisconnected);
		break;
	case NDIS_STATUS_MEDIA_SPECIFIC_INDICATION:
		if (!buf)
			break;
		si = buf;
		TRACE2("status_type=%d", si->status_type);
		switch (si->status_type) {
		case Ndis802_11StatusType_MediaStreamMode:
			break;
#ifdef CONFIG_WIRELESS_EXT
		case Ndis802_11StatusType_Authentication:
			buf = (char *)buf + sizeof(*si);
			len -= sizeof(*si);
			while (len > 0) {
				int pairwise_error = 0, group_error = 0;
				struct ndis_auth_req *auth_req =
					(struct ndis_auth_req *)buf;
				TRACE1(MACSTRSEP, MAC2STR(auth_req->bssid));
				if (auth_req->flags & 0x01)
					TRACE2("reauth request");
				if (auth_req->flags & 0x02)
					TRACE2("key update request");
				if (auth_req->flags & 0x06) {
					pairwise_error = 1;
					TRACE2("pairwise_error");
				}
				if (auth_req->flags & 0x0E) {
					group_error = 1;
					TRACE2("group_error");
				}
				if (pairwise_error || group_error) {
					union iwreq_data wrqu;
					struct iw_michaelmicfailure micfailure;

					memset(&micfailure, 0, sizeof(micfailure));
					if (pairwise_error)
						micfailure.flags |=
							IW_MICFAILURE_PAIRWISE;
					if (group_error)
						micfailure.flags |=
							IW_MICFAILURE_GROUP;
					memcpy(micfailure.src_addr.sa_data,
					       auth_req->bssid, ETH_ALEN);
					memset(&wrqu, 0, sizeof(wrqu));
					wrqu.data.length = sizeof(micfailure);
					wireless_send_event(wnd->net_dev,
							    IWEVMICHAELMICFAILURE,
							    &wrqu, (u8 *)&micfailure);
				}
				len -= auth_req->length;
				buf = (char *)buf + auth_req->length;
			}
			break;
		case Ndis802_11StatusType_PMKID_CandidateList:
		{
			u8 *end;
			unsigned long i;
			struct ndis_pmkid_candidate_list *cand;

			cand = buf + sizeof(struct ndis_status_indication);
			if (len < sizeof(struct ndis_status_indication) +
			    sizeof(struct ndis_pmkid_candidate_list) ||
				cand->version != 1) {
				WARNING("unrecognized PMKID ignored");
				EXIT1(return);
			}

			end = (u8 *)buf + len;
			TRACE2("PMKID ver %d num_cand %d",
			       cand->version, cand->num_candidates);
			for (i = 0; i < cand->num_candidates; i++) {
				struct iw_pmkid_cand pcand;
				union iwreq_data wrqu;
				struct ndis_pmkid_candidate *c =
					&cand->candidates[i];
				if ((u8 *)(c + 1) > end) {
					TRACE2("truncated PMKID");
					break;
				}
				TRACE2("%ld: " MACSTRSEP " 0x%x",
				       i, MAC2STR(c->bssid), c->flags);
				memset(&pcand, 0, sizeof(pcand));
				if (c->flags & 0x01)
					pcand.flags |= IW_PMKID_CAND_PREAUTH;
				pcand.index = i;
				memcpy(pcand.bssid.sa_data, c->bssid, ETH_ALEN);

				memset(&wrqu, 0, sizeof(wrqu));
				wrqu.data.length = sizeof(pcand);
				wireless_send_event(wnd->net_dev, IWEVPMKIDCAND,
						    &wrqu, (u8 *)&pcand);
			}
			break;
		}
		case Ndis802_11StatusType_RadioState:
		{
			struct ndis_radio_status_indication *radio_status = buf;
			if (radio_status->radio_state ==
			    Ndis802_11RadioStatusOn)
				INFO("radio is turned on");
			else if (radio_status->radio_state ==
				 Ndis802_11RadioStatusHardwareOff)
				INFO("radio is turned off by hardware");
			else if (radio_status->radio_state ==
				 Ndis802_11RadioStatusSoftwareOff)
				INFO("radio is turned off by software");
			break;
		}
#endif
		default:
			/* is this RSSI indication? */
			TRACE2("unknown indication: %x", si->status_type);
			break;
		}
		break;
	default:
		TRACE2("unknown status: %08X", status);
		break;
	}

	EXIT2(return);
}

/* called via function pointer; but 64-bit RNDIS driver calls directly */
wstdcall void WIN_FUNC(NdisMIndicateStatusComplete,1)
	(struct ndis_mp_block *nmb)
{
	struct ndis_device *wnd = nmb->wnd;
	ENTER2("%p", wnd);
	if (wnd->tx_ok)
		queue_work(wrapndis_wq, &wnd->tx_work);
}

/* called via function pointer */
wstdcall void NdisMSendComplete(struct ndis_mp_block *nmb,
				struct ndis_packet *packet, NDIS_STATUS status)
{
	struct ndis_device *wnd = nmb->wnd;
	ENTER4("%p, %08X", packet, status);
	assert_irql(_irql_ <= DISPATCH_LEVEL);
	if (deserialized_driver(wnd))
		free_tx_packet(wnd, packet, status);
	else {
		struct ndis_packet_oob_data *oob_data;
		NDIS_STATUS pkt_status;
		TRACE3("%p, %08x", packet, status);
		oob_data = NDIS_PACKET_OOB_DATA(packet);
		switch ((pkt_status = xchg(&oob_data->status, status))) {
		case NDIS_STATUS_NOT_RECOGNIZED:
			free_tx_packet(wnd, packet, status);
			break;
		case NDIS_STATUS_PENDING:
		case 0:
			break;
		default:
			WARNING("%p: invalid status: %08X", packet, pkt_status);
			break;
		}
		/* In case a serialized driver has earlier requested a
		 * pause by returning NDIS_STATUS_RESOURCES during
		 * MiniportSend(Packets), wakeup tx worker now.
		 */
		if (xchg(&wnd->tx_ok, 1) == 0) {
			TRACE3("%d, %d", wnd->tx_ring_start, wnd->tx_ring_end);
			queue_work(wrapndis_wq, &wnd->tx_work);
		}
	}
	EXIT3(return);
}

/* called via function pointer */
wstdcall void NdisMSendResourcesAvailable(struct ndis_mp_block *nmb)
{
	struct ndis_device *wnd = nmb->wnd;
	ENTER3("%d, %d", wnd->tx_ring_start, wnd->tx_ring_end);
	wnd->tx_ok = 1;
	queue_work(wrapndis_wq, &wnd->tx_work);
	EXIT3(return);
}

wstdcall void return_packet(void *arg1, void *arg2)
{
	struct ndis_device *wnd;
	struct ndis_packet *packet;
	struct miniport *mp;
	KIRQL irql;

	wnd = arg1;
	packet = arg2;
	ENTER4("%p, %p", wnd, packet);
	mp = &wnd->wd->driver->ndis_driver->mp;
	irql = serialize_lock_irql(wnd);
	assert_irql(_irql_ == DISPATCH_LEVEL);
	LIN2WIN2(mp->return_packet, wnd->nmb->mp_ctx, packet);
	serialize_unlock_irql(wnd, irql);
	EXIT4(return);
}
WIN_FUNC_DECL(return_packet,2)

/* called via function pointer */
wstdcall void NdisMIndicateReceivePacket(struct ndis_mp_block *nmb,
					 struct ndis_packet **packets,
					 UINT nr_packets)
{
	struct ndis_device *wnd;
	ndis_buffer *buffer;
	struct ndis_packet *packet;
	struct sk_buff *skb;
	ULONG i, length, total_length;
	struct ndis_packet_oob_data *oob_data;
	void *virt;
	struct ndis_tcp_ip_checksum_packet_info csum;

	ENTER3("%p, %d", nmb, nr_packets);
	assert_irql(_irql_ <= DISPATCH_LEVEL);
	wnd = nmb->wnd;
	for (i = 0; i < nr_packets; i++) {
		packet = packets[i];
		if (!packet) {
			WARNING("empty packet ignored");
			continue;
		}
		wnd->net_dev->last_rx = jiffies;
		/* get total number of bytes in packet */
		NdisGetFirstBufferFromPacketSafe(packet, &buffer, &virt,
						 &length, &total_length,
						 NormalPagePriority);
		TRACE3("%d, %d", length, total_length);
		oob_data = NDIS_PACKET_OOB_DATA(packet);
		TRACE3("0x%x, 0x%x, %llu", packet->private.flags,
		       packet->private.packet_flags, oob_data->time_rxed);
		skb = dev_alloc_skb(total_length);
		if (skb) {
			while (buffer) {
				memcpy_skb(skb, MmGetSystemAddressForMdl(buffer),
					   MmGetMdlByteCount(buffer));
				buffer = buffer->next;
			}
			skb->dev = wnd->net_dev;
			skb->protocol = eth_type_trans(skb, wnd->net_dev);
			pre_atomic_add(wnd->net_stats.rx_bytes, total_length);
			atomic_inc_var(wnd->net_stats.rx_packets);
			csum.value = (typeof(csum.value))(ULONG_PTR)
				oob_data->ext.info[TcpIpChecksumPacketInfo];
			TRACE3("0x%05x", csum.value);
			if (wnd->rx_csum.value &&
			    (csum.rx.tcp_succeeded || csum.rx.udp_succeeded ||
			     csum.rx.ip_succeeded))
				skb->ip_summed = CHECKSUM_UNNECESSARY;
			else
				skb->ip_summed = CHECKSUM_NONE;

			if (in_interrupt())
				netif_rx(skb);
			else
				netif_rx_ni(skb);
		} else {
			WARNING("couldn't allocate skb; packet dropped");
			atomic_inc_var(wnd->net_stats.rx_dropped);
		}

		/* serialized drivers check the status upon return
		 * from this function */
		if (!deserialized_driver(wnd)) {
			oob_data->status = NDIS_STATUS_SUCCESS;
			continue;
		}

		/* if a deserialized driver sets
		 * NDIS_STATUS_RESOURCES, then it reclaims the packet
		 * upon return from this function */
		if (oob_data->status == NDIS_STATUS_RESOURCES)
			continue;

		assert(oob_data->status == NDIS_STATUS_SUCCESS);
		/* deserialized driver doesn't check the status upon
		 * return from this function; we need to call
		 * MiniportReturnPacket later for this packet. Calling
		 * MiniportReturnPacket from here is not correct - the
		 * driver doesn't expect it (at least Centrino driver
		 * crashes) */
		schedule_ntos_work_item(WIN_FUNC_PTR(return_packet,2),
					wnd, packet);
	}
	EXIT3(return);
}

/* called via function pointer (by NdisMEthIndicateReceive macro); the
 * first argument is nmb->eth_db */
wstdcall void EthRxIndicateHandler(struct ndis_mp_block *nmb, void *rx_ctx,
				   char *header1, char *header, UINT header_size,
				   void *look_ahead, UINT look_ahead_size,
				   UINT packet_size)
{
	struct sk_buff *skb = NULL;
	struct ndis_device *wnd;
	unsigned int skb_size = 0;
	KIRQL irql;
	struct ndis_packet_oob_data *oob_data;

	ENTER3("nmb = %p, rx_ctx = %p, buf = %p, size = %d, buf = %p, "
	       "size = %d, packet = %d", nmb, rx_ctx, header, header_size,
	       look_ahead, look_ahead_size, packet_size);

	wnd = nmb->wnd;
	TRACE3("wnd = %p", wnd);
	if (!wnd) {
		ERROR("nmb is NULL");
		EXIT3(return);
	}
	wnd->net_dev->last_rx = jiffies;

	if (look_ahead_size < packet_size) {
		struct ndis_packet *packet;
		struct miniport *mp;
		unsigned int bytes_txed;
		NDIS_STATUS res;

		NdisAllocatePacket(&res, &packet, wnd->tx_packet_pool);
		if (res != NDIS_STATUS_SUCCESS) {
			atomic_inc_var(wnd->net_stats.rx_dropped);
			EXIT3(return);
		}
		oob_data = NDIS_PACKET_OOB_DATA(packet);
		mp = &wnd->wd->driver->ndis_driver->mp;
		irql = serialize_lock_irql(wnd);
		assert_irql(_irql_ == DISPATCH_LEVEL);
		res = LIN2WIN6(mp->tx_data, packet, &bytes_txed, nmb,
			       rx_ctx, look_ahead_size, packet_size);
		serialize_unlock_irql(wnd, irql);
		TRACE3("%d, %d, %d", header_size, look_ahead_size, bytes_txed);
		if (res == NDIS_STATUS_SUCCESS) {
			ndis_buffer *buffer;
			struct ndis_tcp_ip_checksum_packet_info csum;
			skb = dev_alloc_skb(header_size + look_ahead_size +
					    bytes_txed);
			if (!skb) {
				ERROR("couldn't allocate skb; packet dropped");
				atomic_inc_var(wnd->net_stats.rx_dropped);
				NdisFreePacket(packet);
				return;
			}
			memcpy_skb(skb, header, header_size);
			memcpy_skb(skb, look_ahead, look_ahead_size);
			buffer = packet->private.buffer_head;
			while (buffer) {
				memcpy_skb(skb,
					   MmGetSystemAddressForMdl(buffer),
					   MmGetMdlByteCount(buffer));
				buffer = buffer->next;
			}
			skb_size = header_size + look_ahead_size + bytes_txed;
			csum.value = (typeof(csum.value))(ULONG_PTR)
				oob_data->ext.info[TcpIpChecksumPacketInfo];
			TRACE3("0x%05x", csum.value);
			if (wnd->rx_csum.value &&
			    (csum.rx.tcp_succeeded || csum.rx.udp_succeeded))
				skb->ip_summed = CHECKSUM_UNNECESSARY;
			else
				skb->ip_summed = CHECKSUM_NONE;
			NdisFreePacket(packet);
		} else if (res == NDIS_STATUS_PENDING) {
			/* driver will call td_complete */
			oob_data->look_ahead = kmalloc(look_ahead_size,
						       GFP_ATOMIC);
			if (!oob_data->look_ahead) {
				NdisFreePacket(packet);
				ERROR("packet dropped");
				atomic_inc_var(wnd->net_stats.rx_dropped);
				EXIT3(return);
			}
			assert(sizeof(oob_data->header) == header_size);
			memcpy(oob_data->header, header,
			       sizeof(oob_data->header));
			memcpy(oob_data->look_ahead, look_ahead,
			       look_ahead_size);
			oob_data->look_ahead_size = look_ahead_size;
			EXIT3(return);
		} else {
			WARNING("packet dropped: %08X", res);
			atomic_inc_var(wnd->net_stats.rx_dropped);
			NdisFreePacket(packet);
			EXIT3(return);
		}
	} else {
		skb_size = header_size + packet_size;
		skb = dev_alloc_skb(skb_size);
		if (skb) {
			memcpy_skb(skb, header, header_size);
			memcpy_skb(skb, look_ahead, packet_size);
		}
	}

	if (skb) {
		skb->dev = wnd->net_dev;
		skb->protocol = eth_type_trans(skb, wnd->net_dev);
		pre_atomic_add(wnd->net_stats.rx_bytes, skb_size);
		atomic_inc_var(wnd->net_stats.rx_packets);
		if (in_interrupt())
			netif_rx(skb);
		else
			netif_rx_ni(skb);
	}

	EXIT3(return);
}

/* called via function pointer */
wstdcall void NdisMTransferDataComplete(struct ndis_mp_block *nmb,
					struct ndis_packet *packet,
					NDIS_STATUS status, UINT bytes_txed)
{
	struct ndis_device *wnd = nmb->wnd;
	struct sk_buff *skb;
	unsigned int skb_size;
	struct ndis_packet_oob_data *oob_data;
	ndis_buffer *buffer;
	struct ndis_tcp_ip_checksum_packet_info csum;

	ENTER3("wnd = %p, packet = %p, bytes_txed = %d",
	       wnd, packet, bytes_txed);
	if (!packet) {
		WARNING("illegal packet");
		EXIT3(return);
	}
	wnd->net_dev->last_rx = jiffies;
	oob_data = NDIS_PACKET_OOB_DATA(packet);
	skb_size = sizeof(oob_data->header) + oob_data->look_ahead_size +
		bytes_txed;
	skb = dev_alloc_skb(skb_size);
	if (!skb) {
		kfree(oob_data->look_ahead);
		NdisFreePacket(packet);
		ERROR("couldn't allocate skb; packet dropped");
		atomic_inc_var(wnd->net_stats.rx_dropped);
		EXIT3(return);
	}
	memcpy_skb(skb, oob_data->header, sizeof(oob_data->header));
	memcpy_skb(skb, oob_data->look_ahead, oob_data->look_ahead_size);
	buffer = packet->private.buffer_head;
	while (buffer) {
		memcpy_skb(skb, MmGetSystemAddressForMdl(buffer),
			   MmGetMdlByteCount(buffer));
		buffer = buffer->next;
	}
	kfree(oob_data->look_ahead);
	NdisFreePacket(packet);
	skb->dev = wnd->net_dev;
	skb->protocol = eth_type_trans(skb, wnd->net_dev);
	pre_atomic_add(wnd->net_stats.rx_bytes, skb_size);
	atomic_inc_var(wnd->net_stats.rx_packets);

	csum.value = (typeof(csum.value))(ULONG_PTR)
		oob_data->ext.info[TcpIpChecksumPacketInfo];
	TRACE3("0x%05x", csum.value);
	if (wnd->rx_csum.value &&
	    (csum.rx.tcp_succeeded || csum.rx.udp_succeeded))
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	else
		skb->ip_summed = CHECKSUM_NONE;

	if (in_interrupt())
		netif_rx(skb);
	else
		netif_rx_ni(skb);
}

/* called via function pointer */
wstdcall void EthRxComplete(struct ndis_mp_block *nmb)
{
	TRACE3("");
}

/* called via function pointer */
wstdcall void NdisMQueryInformationComplete(struct ndis_mp_block *nmb,
					    NDIS_STATUS status)
{
	struct ndis_device *wnd = nmb->wnd;
	typeof(wnd->ndis_req_task) task;

	ENTER2("nmb: %p, wnd: %p, %08X", nmb, wnd, status);
	wnd->ndis_req_status = status;
	wnd->ndis_req_done = 1;
	if ((task = xchg(&wnd->ndis_req_task, NULL)))
		wake_up_process(task);
	else
		WARNING("invalid task");
	EXIT2(return);
}

/* called via function pointer */
wstdcall void NdisMSetInformationComplete(struct ndis_mp_block *nmb,
					  NDIS_STATUS status)
{
	struct ndis_device *wnd = nmb->wnd;
	typeof(wnd->ndis_req_task) task;

	ENTER2("status = %08X", status);
	wnd->ndis_req_status = status;
	wnd->ndis_req_done = 1;
	if ((task = xchg(&wnd->ndis_req_task, NULL)))
		wake_up_process(task);
	else
		WARNING("invalid task");
	EXIT2(return);
}

/* called via function pointer */
wstdcall void NdisMResetComplete(struct ndis_mp_block *nmb,
				 NDIS_STATUS status, BOOLEAN address_reset)
{
	struct ndis_device *wnd = nmb->wnd;
	typeof(wnd->ndis_req_task) task;

	ENTER2("status: %08X, %u", status, address_reset);
	wnd->ndis_req_status = status;
	wnd->ndis_req_done = address_reset + 1;
	if ((task = xchg(&wnd->ndis_req_task, NULL)))
		wake_up_process(task);
	else
		WARNING("invalid task");
	EXIT2(return);
}

wstdcall void WIN_FUNC(NdisMSleep,1)
	(ULONG us)
{
	unsigned long delay;

	ENTER4("%p: us: %u", current, us);
	delay = USEC_TO_HZ(us);
	sleep_hz(delay);
	TRACE4("%p: done", current);
}

wstdcall void WIN_FUNC(NdisGetCurrentSystemTime,1)
	(LARGE_INTEGER *time)
{
	*time = ticks_1601();
	TRACE5("%llu, %lu", *time, jiffies);
}

wstdcall LONG WIN_FUNC(NdisInterlockedDecrement,1)
	(LONG *val)
{
	return InterlockedDecrement(val);
}

wstdcall LONG WIN_FUNC(NdisInterlockedIncrement,1)
	(LONG *val)
{
	return InterlockedIncrement(val);
}

wstdcall struct nt_list *WIN_FUNC(NdisInterlockedInsertHeadList,3)
	(struct nt_list *head, struct nt_list *entry,
	 struct ndis_spinlock *lock)
{
	return ExInterlockedInsertHeadList(head, entry, &lock->klock);
}

wstdcall struct nt_list *WIN_FUNC(NdisInterlockedInsertTailList,3)
	(struct nt_list *head, struct nt_list *entry,
	 struct ndis_spinlock *lock)
{
	return ExInterlockedInsertTailList(head, entry, &lock->klock);
}

wstdcall struct nt_list *WIN_FUNC(NdisInterlockedRemoveHeadList,2)
	(struct nt_list *head, struct ndis_spinlock *lock)
{
	return ExInterlockedRemoveHeadList(head, &lock->klock);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMInitializeScatterGatherDma,3)
	(struct ndis_mp_block *nmb, BOOLEAN dma64_supported, ULONG max_phy_map)
{
	struct ndis_device *wnd = nmb->wnd;
	ENTER2("dma64_supported=%d, maxtransfer=%u", dma64_supported,
	       max_phy_map);
	if (!wrap_is_pci_bus(wnd->wd->dev_bus)) {
		ERROR("used on a non-PCI device");
		return NDIS_STATUS_NOT_SUPPORTED;
	}
#ifdef CONFIG_X86_64
	if (!dma64_supported) {
		TRACE1("64-bit DMA size is not supported");
		if (pci_set_dma_mask(wnd->wd->pci.pdev, DMA_BIT_MASK(32)) ||
		    pci_set_consistent_dma_mask(wnd->wd->pci.pdev,
						DMA_BIT_MASK(32)))
			WARNING("setting dma mask failed");
	}
#endif
	if ((wnd->attributes & NDIS_ATTRIBUTE_BUS_MASTER) &&
	    wrap_is_pci_bus(wnd->wd->dev_bus)) {
		wnd->sg_dma_size = max_phy_map;
		return NDIS_STATUS_SUCCESS;
	} else
		EXIT1(return NDIS_STATUS_NOT_SUPPORTED);
}

wstdcall ULONG WIN_FUNC(NdisMGetDmaAlignment,1)
	(struct ndis_mp_block *nmb)
{
	ENTER3("");
	return dma_get_cache_alignment();
}

wstdcall CHAR WIN_FUNC(NdisSystemProcessorCount,0)
	(void)
{
	return num_online_cpus();
}

wstdcall void WIN_FUNC(NdisGetCurrentProcessorCounts,3)
	(ULONG *idle, ULONG *kernel_user, ULONG *index)
{
	int cpu = smp_processor_id();
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,3,0)
	*idle = kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE];
	*kernel_user = kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM] +
		kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
#else
	*idle = kstat_cpu(cpu).cpustat.idle;
	*kernel_user = kstat_cpu(cpu).cpustat.system +
		kstat_cpu(cpu).cpustat.user;
#endif
	*index = cpu;
}

wstdcall void WIN_FUNC(NdisInitializeEvent,1)
	(struct ndis_event *ndis_event)
{
	EVENTENTER("%p", ndis_event);
	KeInitializeEvent(&ndis_event->nt_event, NotificationEvent, 0);
}

wstdcall BOOLEAN WIN_FUNC(NdisWaitEvent,2)
	(struct ndis_event *ndis_event, UINT ms)
{
	LARGE_INTEGER ticks;
	NTSTATUS res;

	EVENTENTER("%p %u", ndis_event, ms);
	ticks = -((LARGE_INTEGER)ms * TICKSPERMSEC);
	res = KeWaitForSingleObject(&ndis_event->nt_event, 0, 0, TRUE,
				    ms == 0 ? NULL : &ticks);
	if (res == STATUS_SUCCESS)
		EXIT3(return TRUE);
	else
		EXIT3(return FALSE);
}

wstdcall void WIN_FUNC(NdisSetEvent,1)
	(struct ndis_event *ndis_event)
{
	EVENTENTER("%p", ndis_event);
	KeSetEvent(&ndis_event->nt_event, 0, 0);
}

wstdcall void WIN_FUNC(NdisResetEvent,1)
	(struct ndis_event *ndis_event)
{
	EVENTENTER("%p", ndis_event);
	KeResetEvent(&ndis_event->nt_event);
}

static void ndis_worker(struct work_struct *dummy)
{
	struct nt_list *ent;
	struct ndis_work_item *ndis_work_item;

	WORKENTER("");
	while (1) {
		spin_lock_bh(&ndis_work_list_lock);
		ent = RemoveHeadList(&ndis_work_list);
		spin_unlock_bh(&ndis_work_list_lock);
		if (!ent)
			break;
		ndis_work_item = container_of(ent, struct ndis_work_item, list);
		WORKTRACE("%p: %p, %p", ndis_work_item,
			  ndis_work_item->func, ndis_work_item->ctx);
		LIN2WIN2(ndis_work_item->func, ndis_work_item,
			 ndis_work_item->ctx);
		WORKTRACE("%p done", ndis_work_item);
	}
	WORKEXIT(return);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisScheduleWorkItem,1)
	(struct ndis_work_item *ndis_work_item)
{
	ENTER3("%p", ndis_work_item);
	spin_lock_bh(&ndis_work_list_lock);
	InsertTailList(&ndis_work_list, &ndis_work_item->list);
	spin_unlock_bh(&ndis_work_list_lock);
	WORKTRACE("scheduling %p", ndis_work_item);
	queue_work(ndis_wq, &ndis_work);
	EXIT3(return NDIS_STATUS_SUCCESS);
}

wstdcall void WIN_FUNC(NdisMGetDeviceProperty,6)
	(struct ndis_mp_block *nmb, void **phy_dev, void **func_dev,
	 void **next_dev, void **alloc_res, void**trans_res)
{
	ENTER2("nmb: %p, phy_dev = %p, func_dev = %p, next_dev = %p, "
	       "alloc_res = %p, trans_res = %p", nmb, phy_dev, func_dev,
	       next_dev, alloc_res, trans_res);
	if (phy_dev)
		*phy_dev = nmb->pdo;
	if (func_dev)
		*func_dev = nmb->fdo;
	if (next_dev)
		*next_dev = nmb->next_device;
}

wstdcall void WIN_FUNC(NdisMRegisterUnloadHandler,2)
	(struct driver_object *drv_obj, void *unload)
{
	if (drv_obj)
		drv_obj->unload = unload;
	return;
}

wstdcall UINT WIN_FUNC(NdisGetVersion,0)
	(void)
{
	return 0x00050001;
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMQueryAdapterInstanceName,2)
	(struct unicode_string *name, struct ndis_mp_block *nmb)
{
	struct ndis_device *wnd = nmb->wnd;
	struct ansi_string ansi;

	if (wrap_is_pci_bus(wnd->wd->dev_bus))
		RtlInitAnsiString(&ansi, "PCI Ethernet Adapter");
	else
		RtlInitAnsiString(&ansi, "USB Ethernet Adapter");

	if (RtlAnsiStringToUnicodeString(name, &ansi, TRUE))
		EXIT2(return NDIS_STATUS_RESOURCES);
	else
		EXIT2(return NDIS_STATUS_SUCCESS);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisWriteEventLogEntry,7)
	(void *handle, NDIS_STATUS code, ULONG value, USHORT n,
	 void *strings, ULONG datasize, void *data)
{
	TRACE1("0x%x, 0x%x, %u, %u", code, value, n, datasize);
	return NDIS_STATUS_SUCCESS;
}

wstdcall void *WIN_FUNC(NdisGetRoutineAddress,1)
	(struct unicode_string *unicode_string)
{
	struct ansi_string ansi_string;
	void *address;

	if (RtlUnicodeStringToAnsiString(&ansi_string, unicode_string, TRUE) !=
	    STATUS_SUCCESS)
		EXIT1(return NULL);
	INFO("%s", ansi_string.buf);
	address = ndis_get_routine_address(ansi_string.buf);
	RtlFreeAnsiString(&ansi_string);
	return address;
}

wstdcall ULONG WIN_FUNC(NdisReadPcmciaAttributeMemory,4)
	(struct ndis_mp_block *nmb, ULONG offset, void *buffer,
	 ULONG length)
{
	TODO();
	return 0;
}

wstdcall ULONG WIN_FUNC(NdisWritePcmciaAttributeMemory,4)
	(struct ndis_mp_block *nmb, ULONG offset, void *buffer,
	 ULONG length)
{
	TODO();
	return 0;
}

wstdcall void WIN_FUNC(NdisMCoIndicateReceivePacket,3)
	(struct ndis_mp_block *nmb, struct ndis_packet **packets,
	 UINT nr_packets)
{
	ENTER3("nmb = %p", nmb);
	NdisMIndicateReceivePacket(nmb, packets, nr_packets);
	EXIT3(return);
}

wstdcall void WIN_FUNC(NdisMCoSendComplete,3)
	(NDIS_STATUS status, struct ndis_mp_block *nmb,
	 struct ndis_packet *packet)
{
	ENTER3("%08x", status);
	NdisMSendComplete(nmb, packet, status);
	EXIT3(return);
}

wstdcall void WIN_FUNC(NdisMCoRequestComplete,3)
	(NDIS_STATUS status, struct ndis_mp_block *nmb,
	 struct ndis_request *ndis_request)
{
	struct ndis_device *wnd = nmb->wnd;
	typeof(wnd->ndis_req_task) task;

	ENTER3("%08X", status);
	wnd->ndis_req_status = status;
	wnd->ndis_req_done = 1;
	if ((task = xchg(&wnd->ndis_req_task, NULL)))
		wake_up_process(task);
	else
		WARNING("invalid task");
	EXIT3(return);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisIMNotifiyPnPEvent,2)
	(struct ndis_mp_block *nmb, struct net_pnp_event *event)
{
	ENTER2("%p, %d", nmb, event->code);
	/* NdisWrapper never calls protocol's pnp event notifier, so
	 * nothing to do here */
	EXIT2(return NDIS_STATUS_SUCCESS);
}

wstdcall void WIN_FUNC(NdisCompletePnPEvent,3)
	(NDIS_STATUS status, void *handle, struct net_pnp_event *event)
{
	ENTER2("%d, %p, %d", status, handle, event->code);
	/* NdisWrapper never calls protocol's pnp event notifier, so
	 * nothing to do here */
	EXIT2(return);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMSetMiniportSecondary,2)
	(struct ndis_mp_block *nmb2, struct ndis_mp_block *nmb1)
{
	ENTER3("%p, %p", nmb1, nmb2);
	TODO();
	EXIT3(return NDIS_STATUS_SUCCESS);
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMPromoteMiniport,1)
	(struct ndis_mp_block *nmb)
{
	ENTER3("%p", nmb);
	TODO();
	EXIT3(return NDIS_STATUS_SUCCESS);
}

wstdcall void WIN_FUNC(NdisMCoActivateVcComplete,3)
	(NDIS_STATUS status, void *handle, void *params)
{
	TODO();
}

wstdcall void WIN_FUNC(NdisMCoDeactivateVcComplete,2)
	(NDIS_STATUS status, void *handle)
{
	TODO();
}

wstdcall void WIN_FUNC(NdisMRemoveMiniport,1)
	(void *handle)
{
	TODO();
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMOpenLog,3)
	(struct ndis_device *wnd, UINT size, void *handle)
{
	if (size >= sizeof(int))
		*((int *)handle) = 42;
	return NDIS_STATUS_SUCCESS;
}

wstdcall NDIS_STATUS WIN_FUNC(NdisMWriteLogData,3)
	(void *handle, char *buffer, UINT buffer_size)
{
	TODO();
	return NDIS_STATUS_SUCCESS;
}

wstdcall void WIN_FUNC(NdisMFlushLog,1)
	(void *handle)
{
	TODO();
}

wstdcall void WIN_FUNC(NdisMCloseLog,1)
	(void *handle)
{
	TODO();
}

static void *ndis_get_routine_address(char *name)
{
	int i;
	ENTER2("%p", name);
	for (i = 0; i < ARRAY_SIZE(ndis_exports); i++) {
		if (strcmp(name, ndis_exports[i].name) == 0) {
			TRACE2("%p", ndis_exports[i].func);
			return ndis_exports[i].func;
		}
	}
	EXIT2(return NULL);
}

/* ndis_init_device is called for each device */
int ndis_init_device(struct ndis_device *wnd)
{
	struct ndis_mp_block *nmb = wnd->nmb;

	KeInitializeSpinLock(&nmb->lock);
	wnd->mp_interrupt = NULL;
	wnd->wrap_timer_slist.next = NULL;
	if (wnd->wd->driver->ndis_driver)
		wnd->wd->driver->ndis_driver->mp.shutdown = NULL;

	nmb->filterdbs.eth_db = nmb;
	nmb->filterdbs.tr_db = nmb;
	nmb->filterdbs.fddi_db = nmb;
	nmb->filterdbs.arc_db = nmb;

	nmb->rx_packet = WIN_FUNC_PTR(NdisMIndicateReceivePacket,3);
	nmb->send_complete = WIN_FUNC_PTR(NdisMSendComplete,3);
	nmb->send_resource_avail = WIN_FUNC_PTR(NdisMSendResourcesAvailable,1);
	nmb->status = WIN_FUNC_PTR(NdisMIndicateStatus,4);
	nmb->status_complete = WIN_FUNC_PTR(NdisMIndicateStatusComplete,1);
	nmb->queryinfo_complete = WIN_FUNC_PTR(NdisMQueryInformationComplete,2);
	nmb->setinfo_complete = WIN_FUNC_PTR(NdisMSetInformationComplete,2);
	nmb->reset_complete = WIN_FUNC_PTR(NdisMResetComplete,3);
	nmb->eth_rx_indicate = WIN_FUNC_PTR(EthRxIndicateHandler,8);
	nmb->eth_rx_complete = WIN_FUNC_PTR(EthRxComplete,1);
	nmb->td_complete = WIN_FUNC_PTR(NdisMTransferDataComplete,4);
	return 0;
}

/* ndis_exit_device is called for each device */
void ndis_exit_device(struct ndis_device *wnd)
{
	struct wrap_device_setting *setting;
	ENTER2("%p", wnd);
	mutex_lock(&loader_mutex);
	nt_list_for_each_entry(setting, &wnd->wd->settings, list) {
		struct ndis_configuration_parameter *param;
		param = setting->encoded;
		if (param) {
			if (param->type == NdisParameterString)
				RtlFreeUnicodeString(&param->data.string);
			ExFreePool(param);
			setting->encoded = NULL;
		}
	}
	mutex_unlock(&loader_mutex);
}

/* ndis_init is called once when module is loaded */
int ndis_init(void)
{
	InitializeListHead(&ndis_work_list);
	spin_lock_init(&ndis_work_list_lock);
	INIT_WORK(&ndis_work, ndis_worker);

	ndis_wq = create_singlethread_workqueue("ndis_wq");
	if (!ndis_wq) {
		WARNING("couldn't create worker thread");
		EXIT1(return -ENOMEM);
	}

	TRACE1("ndis_wq: %p", ndis_wq);
	return 0;
}

/* ndis_exit is called once when module is removed */
void ndis_exit(void)
{
	ENTER1("");
	if (ndis_wq)
		destroy_workqueue(ndis_wq);
	EXIT1(return);
}
