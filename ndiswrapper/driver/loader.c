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
#include "loader.h"
#include "wrapndis.h"
#include "pnp.h"
#include "nvmalloc.h"

#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>

/*
  Network adapter: ClassGuid = {4d36e972-e325-11ce-bfc1-08002be10318}
  Network client: ClassGuid = {4d36e973-e325-11ce-bfc1-08002be10318}
  PCMCIA adapter: ClassGuid = {4d36e977-e325-11ce-bfc1-08002be10318}
  USB: ClassGuid = {36fc9e60-c465-11cf-8056-444553540000}
*/

/* the indices used here must match macros WRAP_NDIS_DEVICE etc. */
static struct guid class_guids[] = {
	/* Network */
	{ .data1 = 0x4d36e972, .data2 = 0xe325, .data3 = 0x11ce },
	/* USB WDM */
	{ .data1 = 0x36fc9e60, .data2 = 0xc465, .data3 = 0x11cf },
	/* Bluetooth */
	{ .data1 = 0xe0cbf06c, .data2 = 0xcd8b, .data3 = 0x4647 },
	/* ivtcorporation.com's bluetooth device claims this is
	 * bluetooth guid */
	{ .data1 = 0xf12d3cf8, .data2 = 0xb11d, .data3 = 0x457e},
};

struct mutex loader_mutex;
static struct completion loader_complete;

static struct nt_list wrap_devices;
static struct nt_list wrap_drivers;

static int wrap_device_type(int data1)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(class_guids); i++)
		if (data1 == class_guids[i].data1)
			return i;
	ERROR("unknown device: 0x%x\n", data1);
	return -1;
}

/* load driver for given device, if not already loaded */
struct wrap_driver *load_wrap_driver(struct wrap_device *wd)
{
	int ret;
	struct nt_list *cur;
	struct wrap_driver *wrap_driver;

	ENTER1("device: %04X:%04X:%04X:%04X", wd->vendor, wd->device,
	       wd->subvendor, wd->subdevice);
	mutex_lock(&loader_mutex);
	wrap_driver = NULL;
	nt_list_for_each(cur, &wrap_drivers) {
		wrap_driver = container_of(cur, struct wrap_driver, list);
		if (!stricmp(wrap_driver->name, wd->driver_name)) {
			TRACE1("driver %s already loaded", wrap_driver->name);
			break;
		} else
			wrap_driver = NULL;
	}
	mutex_unlock(&loader_mutex);

	if (!wrap_driver) {
		char *argv[] = {"loadndisdriver", WRAP_CMD_LOAD_DRIVER,
#if DEBUG >= 1
				"1",
#else
				"0",
#endif
				UTILS_VERSION, wd->driver_name,
				wd->conf_file_name, NULL};
		char *env[] = {NULL};

		TRACE1("loading driver %s", wd->driver_name);
		mutex_lock(&loader_mutex);
		reinit_completion(&loader_complete);
		ret = call_usermodehelper("/sbin/loadndisdriver", argv, env,
					  UMH_WAIT_PROC);
		if (ret) {
			mutex_unlock(&loader_mutex);
			ERROR("couldn't load driver %s; check system log "
			      "for messages from 'loadndisdriver'",
			      wd->driver_name);
			EXIT1(return NULL);
		}
		wait_for_completion(&loader_complete);
		TRACE1("%s", wd->driver_name);
		wrap_driver = NULL;
		nt_list_for_each(cur, &wrap_drivers) {
			wrap_driver = container_of(cur, struct wrap_driver,
						   list);
			if (!stricmp(wrap_driver->name, wd->driver_name)) {
				wd->driver = wrap_driver;
				break;
			} else
				wrap_driver = NULL;
		}
		mutex_unlock(&loader_mutex);
		if (wrap_driver)
			TRACE1("driver %s is loaded", wrap_driver->name);
		else
			ERROR("couldn't load driver '%s'", wd->driver_name);
	}
	EXIT1(return wrap_driver);
}

/* load the driver files from userspace. */
static int load_sys_files(struct wrap_driver *driver,
			  struct load_driver *load_driver)
{
	int i, err;

	TRACE1("num_pe_images = %d", load_driver->num_sys_files);
	TRACE1("loading driver: %s", load_driver->name);
	strncpy(driver->name, load_driver->name, sizeof(driver->name));
	driver->name[sizeof(driver->name)-1] = 0;
	TRACE1("driver: %s", driver->name);
	err = 0;
	driver->num_pe_images = 0;
	for (i = 0; i < load_driver->num_sys_files; i++) {
		struct pe_image *pe_image;
		pe_image = &driver->pe_images[driver->num_pe_images];

		strncpy(pe_image->name, load_driver->sys_files[i].name,
			sizeof(pe_image->name));
		pe_image->name[sizeof(pe_image->name)-1] = 0;
		TRACE1("image size: %zu bytes", load_driver->sys_files[i].size);

#ifdef CONFIG_X86_64
#ifdef PAGE_KERNEL_EXECUTABLE
		pe_image->image =
			nvmalloc(load_driver->sys_files[i].size,
				  GFP_KERNEL | __GFP_HIGHMEM,
				  PAGE_KERNEL_EXECUTABLE);
#elif defined PAGE_KERNEL_EXEC
		pe_image->image =
			nvmalloc(load_driver->sys_files[i].size,
				  GFP_KERNEL | __GFP_HIGHMEM,
				  PAGE_KERNEL_EXEC);
#else
#error x86_64 should have either PAGE_KERNEL_EXECUTABLE or PAGE_KERNEL_EXEC
#endif
#else
		/* hate to play with kernel macros, but PAGE_KERNEL_EXEC is
		 * not available to modules! */
#ifdef cpu_has_nx
		if (cpu_has_nx)
			pe_image->image =
				nvmalloc(load_driver->sys_files[i].size,
					  GFP_KERNEL | __GFP_HIGHMEM,
					  __pgprot(__PAGE_KERNEL & ~_PAGE_NX));
		else
			pe_image->image =
				vmalloc(load_driver->sys_files[i].size);
#else
			pe_image->image =
				vmalloc(load_driver->sys_files[i].size);
#endif
#endif
		if (!pe_image->image) {
			ERROR("couldn't allocate memory");
			err = -ENOMEM;
			break;
		}
		TRACE1("image is at %p", pe_image->image);

		if (copy_from_user(pe_image->image,
				   load_driver->sys_files[i].data,
				   load_driver->sys_files[i].size)) {
			ERROR("couldn't load file %s",
			      load_driver->sys_files[i].name);
			err = -EFAULT;
			break;
		}
		pe_image->size = load_driver->sys_files[i].size;
		driver->num_pe_images++;
	}

	if (!err && link_pe_images(driver->pe_images, driver->num_pe_images)) {
		ERROR("couldn't prepare driver '%s'", load_driver->name);
		err = -EINVAL;
	}

	if (driver->num_pe_images < load_driver->num_sys_files || err) {
		for (i = 0; i < driver->num_pe_images; i++)
			if (driver->pe_images[i].image)
				vfree(driver->pe_images[i].image);
		driver->num_pe_images = 0;
		EXIT1(return err);
	} else
		EXIT1(return 0);
}

struct wrap_bin_file *get_bin_file(char *bin_file_name)
{
	int i = 0;
	struct wrap_driver *driver, *cur;

	ENTER1("%s", bin_file_name);
	mutex_lock(&loader_mutex);
	driver = NULL;
	nt_list_for_each_entry(cur, &wrap_drivers, list) {
		for (i = 0; i < cur->num_bin_files; i++)
			if (!stricmp(cur->bin_files[i].name, bin_file_name)) {
				driver = cur;
				break;
			}
		if (driver)
			break;
	}
	mutex_unlock(&loader_mutex);
	if (!driver) {
		TRACE1("couldn't find bin file '%s'", bin_file_name);
		return NULL;
	}

	if (!driver->bin_files[i].data) {
		char *argv[] = {"loadndisdriver", WRAP_CMD_LOAD_BIN_FILE,
#if DEBUG >= 1
				"1",
#else
				"0",
#endif
				UTILS_VERSION, driver->name,
				driver->bin_files[i].name, NULL};
		char *env[] = {NULL};
		int ret;

		TRACE1("loading bin file %s/%s", driver->name,
		       driver->bin_files[i].name);
		mutex_lock(&loader_mutex);
		reinit_completion(&loader_complete);
		ret = call_usermodehelper("/sbin/loadndisdriver", argv, env,
					  UMH_WAIT_PROC);
		if (ret) {
			mutex_unlock(&loader_mutex);
			ERROR("couldn't load file %s/%s; check system log "
			      "for messages from 'loadndisdriver' (%d)",
			      driver->name, driver->bin_files[i].name, ret);
			EXIT1(return NULL);
		}
		wait_for_completion(&loader_complete);
		mutex_unlock(&loader_mutex);
		if (!driver->bin_files[i].data) {
			WARNING("couldn't load binary file %s",
				driver->bin_files[i].name);
			EXIT1(return NULL);
		}
	}
	EXIT2(return &(driver->bin_files[i]));
}

/* called with loader_mutex down */
static int add_bin_file(struct load_driver_file *driver_file)
{
	struct wrap_driver *driver, *cur;
	struct wrap_bin_file *bin_file;
	int i = 0;

	driver = NULL;
	nt_list_for_each_entry(cur, &wrap_drivers, list) {
		for (i = 0; i < cur->num_bin_files; i++)
			if (!stricmp(cur->bin_files[i].name,
				     driver_file->name)) {
				driver = cur;
				break;
			}
		if (driver)
			break;
	}
	if (!driver) {
		ERROR("couldn't find %s", driver_file->name);
		return -EINVAL;
	}
	bin_file = &driver->bin_files[i];
	strncpy(bin_file->name, driver_file->name, sizeof(bin_file->name));
	bin_file->name[sizeof(bin_file->name)-1] = 0;
	bin_file->data = vmalloc(driver_file->size);
	if (!bin_file->data) {
		ERROR("couldn't allocate memory");
		return -ENOMEM;
	}
	bin_file->size = driver_file->size;
	if (copy_from_user(bin_file->data, driver_file->data, bin_file->size)) {
		ERROR("couldn't copy data");
		free_bin_file(bin_file);
		return -EFAULT;
	}
	return 0;
}

void free_bin_file(struct wrap_bin_file *bin_file)
{
	TRACE2("unloading %s", bin_file->name);
	if (bin_file->data)
		vfree(bin_file->data);
	bin_file->data = NULL;
	bin_file->size = 0;
	EXIT2(return);
}

/* load firmware files from userspace */
static int load_bin_files_info(struct wrap_driver *driver,
			       struct load_driver *load_driver)
{
	struct wrap_bin_file *bin_files;
	int i;

	ENTER1("%s, %d", load_driver->name, load_driver->num_bin_files);
	driver->num_bin_files = 0;
	driver->bin_files = NULL;
	if (load_driver->num_bin_files == 0)
		EXIT1(return 0);
	bin_files = kzalloc(load_driver->num_bin_files * sizeof(*bin_files),
			    GFP_KERNEL);
	if (!bin_files) {
		ERROR("couldn't allocate memory");
		EXIT1(return -ENOMEM);
	}

	for (i = 0; i < load_driver->num_bin_files; i++) {
		strncpy(bin_files[i].name, load_driver->bin_files[i].name,
			sizeof(bin_files[i].name));
		bin_files[i].name[sizeof(bin_files[i].name)-1] = 0;
		TRACE2("loaded bin file %s", bin_files[i].name);
	}
	driver->num_bin_files = load_driver->num_bin_files;
	driver->bin_files = bin_files;
	EXIT1(return 0);
}

/* load settings for a device. called with loader_mutex down */
static int load_settings(struct wrap_driver *wrap_driver,
			 struct load_driver *load_driver)
{
	int i, num_settings;

	ENTER1("%p, %p", wrap_driver, load_driver);

	num_settings = 0;
	for (i = 0; i < load_driver->num_settings; i++) {
		struct load_device_setting *load_setting =
			&load_driver->settings[i];
		struct wrap_device_setting *setting;
		ULONG data1;

		setting = kzalloc(sizeof(*setting), GFP_KERNEL);
		if (!setting) {
			ERROR("couldn't allocate memory");
			break;
		}
		strncpy(setting->name, load_setting->name,
			sizeof(setting->name));
		setting->name[sizeof(setting->name)-1] = 0;
		strncpy(setting->value, load_setting->value,
		       sizeof(setting->value));
		setting->value[sizeof(setting->value)-1] = 0;
		TRACE2("%p: %s=%s", setting, setting->name, setting->value);

		if (strcmp(setting->name, "driver_version") == 0) {
			strncpy(wrap_driver->version, setting->value,
				sizeof(wrap_driver->version));
			wrap_driver->version[sizeof(wrap_driver->version)-1] = 0;
		} else if (strcmp(setting->name, "class_guid") == 0 &&
			   sscanf(setting->value, "%x", &data1) == 1) {
			wrap_driver->dev_type = wrap_device_type(data1);
			if (wrap_driver->dev_type < 0) {
				WARNING("unknown guid: %x", data1);
				wrap_driver->dev_type = 0;
			}
		}
		InsertTailList(&wrap_driver->settings, &setting->list);
		num_settings++;
	}
	/* it is not a fatal error if some settings couldn't be loaded */
	if (num_settings > 0)
		EXIT1(return 0);
	else
		EXIT1(return -EINVAL);
}

void unload_wrap_device(struct wrap_device *wd)
{
	struct nt_list *cur;
	ENTER1("unloading device %p (%04X:%04X:%04X:%04X), driver %s", wd,
	       wd->vendor, wd->device, wd->subvendor, wd->subdevice,
	       wd->driver_name);
	mutex_lock(&loader_mutex);
	while ((cur = RemoveHeadList(&wd->settings))) {
		struct wrap_device_setting *setting;
		setting = container_of(cur, struct wrap_device_setting, list);
		kfree(setting);
	}
	RemoveEntryList(&wd->list);
	mutex_unlock(&loader_mutex);
	kfree(wd);
	EXIT1(return);
}

/* should be called with loader_mutex down */
void unload_wrap_driver(struct wrap_driver *driver)
{
	int i;
	struct driver_object *drv_obj;
	struct nt_list *cur, *next;

	ENTER1("unloading driver: %s (%p)", driver->name, driver);
	TRACE1("freeing %d images", driver->num_pe_images);
	drv_obj = driver->drv_obj;
	for (i = 0; i < driver->num_pe_images; i++)
		if (driver->pe_images[i].image) {
			TRACE1("freeing image at %p",
			       driver->pe_images[i].image);
			vfree(driver->pe_images[i].image);
		}

	TRACE1("freeing %d bin files", driver->num_bin_files);
	for (i = 0; i < driver->num_bin_files; i++) {
		TRACE1("freeing image at %p", driver->bin_files[i].data);
		if (driver->bin_files[i].data)
			vfree(driver->bin_files[i].data);
	}
	kfree(driver->bin_files);
	RtlFreeUnicodeString(&drv_obj->name);
	RemoveEntryList(&driver->list);
	nt_list_for_each_safe(cur, next, &driver->settings) {
		struct wrap_device_setting *setting;
		struct ndis_configuration_parameter *param;

		setting = container_of(cur, struct wrap_device_setting, list);
		TRACE2("%p", setting);
		param = setting->encoded;
		if (param) {
			TRACE2("%p", param);
			if (param->type == NdisParameterString)
				RtlFreeUnicodeString(&param->data.string);
			ExFreePool(param);
		}
		kfree(setting);
	}
	/* this frees driver */
	free_custom_extensions(drv_obj->drv_ext);
	kfree(drv_obj->drv_ext);
	TRACE1("drv_obj: %p", drv_obj);

	EXIT1(return);
}

/* call the entry point of the driver */
static int start_wrap_driver(struct wrap_driver *driver)
{
	int i;
	NTSTATUS ret, res;
	struct driver_object *drv_obj;
	typeof(driver->pe_images[0].entry) entry;

	ENTER1("%s", driver->name);
	drv_obj = driver->drv_obj;
	for (ret = res = 0, i = 0; i < driver->num_pe_images; i++)
		/* dlls are already started by loader */
		if (driver->pe_images[i].type == IMAGE_FILE_EXECUTABLE_IMAGE) {
			entry = driver->pe_images[i].entry;
			drv_obj->start = driver->pe_images[i].entry;
			drv_obj->driver_size = driver->pe_images[i].size;
			TRACE1("entry: %p, %p, drv_obj: %p",
			       entry, *entry, drv_obj);
			res = LIN2WIN2(entry, drv_obj, &drv_obj->name);
			ret |= res;
			TRACE1("entry returns %08X", res);
			break;
		}
	if (ret) {
		ERROR("driver initialization failed: %08X", ret);
		RtlFreeUnicodeString(&drv_obj->name);
		/* this frees ndis_driver */
		free_custom_extensions(drv_obj->drv_ext);
		kfree(drv_obj->drv_ext);
		TRACE1("drv_obj: %p", drv_obj);
		ObDereferenceObject(drv_obj);
		EXIT1(return -EINVAL);
	}
	EXIT1(return 0);
}

/*
 * add driver to list of loaded driver but make sure this driver is
 * not loaded before. called with loader_mutex down
 */
static int add_wrap_driver(struct wrap_driver *driver)
{
	struct wrap_driver *tmp;

	ENTER1("name: %s", driver->name);
	nt_list_for_each_entry(tmp, &wrap_drivers, list) {
		if (stricmp(tmp->name, driver->name) == 0) {
			ERROR("cannot add duplicate driver");
			EXIT1(return -EBUSY);
		}
	}
	InsertHeadList(&wrap_drivers, &driver->list);
	EXIT1(return 0);
}

/* load a driver from userspace and initialize it. called with
 * loader_mutex down */
static int load_user_space_driver(struct load_driver *load_driver)
{
	struct driver_object *drv_obj;
	struct ansi_string ansi_reg;
	struct wrap_driver *wrap_driver = NULL;

	ENTER1("%p", load_driver);
	drv_obj = allocate_object(sizeof(*drv_obj), OBJECT_TYPE_DRIVER, NULL);
	if (!drv_obj) {
		ERROR("couldn't allocate memory");
		EXIT1(return -ENOMEM);
	}
	TRACE1("drv_obj: %p", drv_obj);
	drv_obj->drv_ext = kzalloc(sizeof(*(drv_obj->drv_ext)), GFP_KERNEL);
	if (!drv_obj->drv_ext) {
		ERROR("couldn't allocate memory");
		ObDereferenceObject(drv_obj);
		EXIT1(return -ENOMEM);
	}
	InitializeListHead(&drv_obj->drv_ext->custom_ext);
	if (IoAllocateDriverObjectExtension(drv_obj,
					    (void *)WRAP_DRIVER_CLIENT_ID,
					    sizeof(*wrap_driver),
					    (void **)&wrap_driver) !=
	    STATUS_SUCCESS)
		EXIT1(return -ENOMEM);
	TRACE1("driver: %p", wrap_driver);
	memset(wrap_driver, 0, sizeof(*wrap_driver));
	InitializeListHead(&wrap_driver->list);
	InitializeListHead(&wrap_driver->settings);
	wrap_driver->drv_obj = drv_obj;
	RtlInitAnsiString(&ansi_reg, "/tmp");
	if (RtlAnsiStringToUnicodeString(&drv_obj->name, &ansi_reg, TRUE) !=
	    STATUS_SUCCESS) {
		ERROR("couldn't initialize registry path");
		free_custom_extensions(drv_obj->drv_ext);
		kfree(drv_obj->drv_ext);
		TRACE1("drv_obj: %p", drv_obj);
		ObDereferenceObject(drv_obj);
		EXIT1(return -EINVAL);
	}
	strncpy(wrap_driver->name, load_driver->name, sizeof(wrap_driver->name));
	wrap_driver->name[sizeof(wrap_driver->name)-1] = 0;
	if (load_sys_files(wrap_driver, load_driver) ||
	    load_bin_files_info(wrap_driver, load_driver) ||
	    load_settings(wrap_driver, load_driver) ||
	    start_wrap_driver(wrap_driver) ||
	    add_wrap_driver(wrap_driver)) {
		unload_wrap_driver(wrap_driver);
		ObDereferenceObject(drv_obj);
		EXIT1(return -EINVAL);
	} else {
		printk(KERN_INFO "%s: driver %s (%s) loaded\n",
		       DRIVER_NAME, wrap_driver->name, wrap_driver->version);
		add_taint(TAINT_PROPRIETARY_MODULE, LOCKDEP_NOW_UNRELIABLE);
		EXIT1(return 0);
	}
}

static struct pci_device_id wrap_pci_id_table[] = {
	{
		.vendor = PCI_ANY_ID,
		.device = PCI_ANY_ID,
		.subvendor = PCI_ANY_ID,
		.subdevice = PCI_ANY_ID,
		.class = 0,
		.class_mask = 0,
		.driver_data = 0
	}
};

static struct pci_driver wrap_pci_driver = {
	.name		= DRIVER_NAME,
	.id_table	= wrap_pci_id_table,
	.probe		= wrap_pnp_start_pci_device,
	.remove		= wrap_pnp_remove_pci_device,
	.suspend	= wrap_pnp_suspend_pci_device,
	.resume		= wrap_pnp_resume_pci_device,
};

#ifdef ENABLE_USB
static struct usb_device_id wrap_usb_id_table[] = {
	{
		.driver_info = 1
	},
};

static struct usb_driver wrap_usb_driver = {
	.name = DRIVER_NAME,
	.id_table = wrap_usb_id_table,
	.probe = wrap_pnp_start_usb_device,
	.disconnect = wrap_pnp_remove_usb_device,
	.suspend = wrap_pnp_suspend_usb_device,
	.resume = wrap_pnp_resume_usb_device,
};
#endif

/* register drivers for pci and usb */
static void register_devices(void)
{
	int res;

	res = pci_register_driver(&wrap_pci_driver);
	if (res < 0) {
		ERROR("couldn't register pci driver: %d", res);
		wrap_pci_driver.name = NULL;
	}

#ifdef ENABLE_USB
	res = usb_register(&wrap_usb_driver);
	if (res < 0) {
		ERROR("couldn't register usb driver: %d", res);
		wrap_usb_driver.name = NULL;
	}
#endif
	EXIT1(return);
}

static void unregister_devices(void)
{
	struct nt_list *cur, *next;

	mutex_lock(&loader_mutex);
	nt_list_for_each_safe(cur, next, &wrap_devices) {
		struct wrap_device *wd;
		wd = container_of(cur, struct wrap_device, list);
		set_bit(HW_DISABLED, &wd->hw_status);
	}
	mutex_unlock(&loader_mutex);

	if (wrap_pci_driver.name)
		pci_unregister_driver(&wrap_pci_driver);
#ifdef ENABLE_USB
	if (wrap_usb_driver.name)
		usb_deregister(&wrap_usb_driver);
#endif
}

struct wrap_device *load_wrap_device(struct load_device *load_device)
{
	int ret;
	struct nt_list *cur;
	struct wrap_device *wd = NULL;
	char vendor[5], device[5], subvendor[5], subdevice[5], bus[5];

	ENTER1("%04x, %04x, %04x, %04x", load_device->vendor,
	       load_device->device, load_device->subvendor,
	       load_device->subdevice);
	if (sprintf(vendor, "%04x", load_device->vendor) == 4 &&
	    sprintf(device, "%04x", load_device->device) == 4 &&
	    sprintf(subvendor, "%04x", load_device->subvendor) == 4 &&
	    sprintf(subdevice, "%04x", load_device->subdevice) == 4 &&
	    sprintf(bus, "%04x", load_device->bus) == 4) {
		char *argv[] = {"loadndisdriver", WRAP_CMD_LOAD_DEVICE,
#if DEBUG >= 1
				"1",
#else
				"0",
#endif
				UTILS_VERSION, vendor, device,
				subvendor, subdevice, bus, NULL};
		char *env[] = {NULL};
		TRACE2("%s, %s, %s, %s, %s", vendor, device,
		       subvendor, subdevice, bus);
		mutex_lock(&loader_mutex);
		reinit_completion(&loader_complete);
		ret = call_usermodehelper("/sbin/loadndisdriver", argv, env,
					  UMH_WAIT_PROC);
		if (ret) {
			mutex_unlock(&loader_mutex);
			TRACE1("couldn't load device %04x:%04x; check system "
			       "log for messages from 'loadndisdriver'",
			       load_device->vendor, load_device->device);
			EXIT1(return NULL);
		}
		wait_for_completion(&loader_complete);
		wd = NULL;
		nt_list_for_each(cur, &wrap_devices) {
			wd = container_of(cur, struct wrap_device, list);
			TRACE2("%p, %04x, %04x, %04x, %04x", wd, wd->vendor,
			       wd->device, wd->subvendor, wd->subdevice);
			if (wd->vendor == load_device->vendor &&
			    wd->device == load_device->device)
				break;
			else
				wd = NULL;
		}
		mutex_unlock(&loader_mutex);
	} else
		wd = NULL;
	EXIT1(return wd);
}

struct wrap_device *get_wrap_device(void *dev, int bus)
{
	struct nt_list *cur;
	struct wrap_device *wd;

	mutex_lock(&loader_mutex);
	wd = NULL;
	nt_list_for_each(cur, &wrap_devices) {
		wd = container_of(cur, struct wrap_device, list);
		if (bus == WRAP_PCI_BUS &&
		    wrap_is_pci_bus(wd->dev_bus) && wd->pci.pdev == dev)
			break;
		else if (bus == WRAP_USB_BUS &&
			 wrap_is_usb_bus(wd->dev_bus) && wd->usb.udev == dev)
			break;
		else
			wd = NULL;
	}
	mutex_unlock(&loader_mutex);
	return wd;
}

/* called with loader_mutex is down */
static long wrapper_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct load_driver *load_driver;
	struct load_device load_device;
	struct load_driver_file load_bin_file;
	int ret;
	void __user *addr = (void __user *)arg;

	ENTER1("cmd: 0x%x", cmd);

	ret = 0;
	switch (cmd) {
	case WRAP_IOCTL_LOAD_DEVICE:
		if (copy_from_user(&load_device, addr, sizeof(load_device))) {
			ret = -EFAULT;
			break;
		}
		TRACE2("%04x, %04x, %04x, %04x", load_device.vendor,
		       load_device.device, load_device.subvendor,
		       load_device.subdevice);
		if (load_device.vendor) {
			struct wrap_device *wd;
			wd = kzalloc(sizeof(*wd), GFP_KERNEL);
			if (!wd) {
				ret = -ENOMEM;
				break;
			}
			InitializeListHead(&wd->settings);
			wd->dev_bus = WRAP_BUS(load_device.bus);
			wd->vendor = load_device.vendor;
			wd->device = load_device.device;
			wd->subvendor = load_device.subvendor;
			wd->subdevice = load_device.subdevice;
			strncpy(wd->conf_file_name, load_device.conf_file_name,
				sizeof(wd->conf_file_name));
			wd->conf_file_name[sizeof(wd->conf_file_name)-1] = 0;
			strncpy(wd->driver_name, load_device.driver_name,
			       sizeof(wd->driver_name));
			wd->driver_name[sizeof(wd->driver_name)-1] = 0;
			InsertHeadList(&wrap_devices, &wd->list);
			ret = 0;
		} else
			ret = -EINVAL;
		break;
	case WRAP_IOCTL_LOAD_DRIVER:
		TRACE1("loading driver at %p", addr);
		load_driver = vmalloc(sizeof(*load_driver));
		if (!load_driver) {
			ret = -ENOMEM;
			break;
		}
		if (copy_from_user(load_driver, addr, sizeof(*load_driver)))
			ret = -EFAULT;
		else
			ret = load_user_space_driver(load_driver);
		vfree(load_driver);
		break;
	case WRAP_IOCTL_LOAD_BIN_FILE:
		if (copy_from_user(&load_bin_file, addr, sizeof(load_bin_file)))
			ret = -EFAULT;
		else
			ret = add_bin_file(&load_bin_file);
		break;
	default:
		ERROR("unknown ioctl 0x%x", cmd);
		ret = -EINVAL;
		break;
	}
	complete(&loader_complete);
	EXIT1(return ret);
}

#ifdef CONFIG_COMPAT
static int copy_load_driver_file32(struct load_driver_file *k,
				   struct load_driver_file32 __user *u)
{
	u32 data;

	if (copy_from_user(&k->driver_name, &u->driver_name,
			   sizeof(u->driver_name) + sizeof(u->name)))
		return -EFAULT;

	if (get_user(k->size, &u->size))
		return -EFAULT;
	if (get_user(data, &u->data))
		return -EFAULT;

	k->data = (void __user *)(unsigned long)data;
	return 0;
}

static int copy_load_driver32(struct load_driver *k,
			      struct load_driver32 __user *u)
{
	int i;

	if (copy_from_user(&k->name, &u->name,
			   sizeof(u->name) + sizeof(u->conf_file_name)))
		return -EFAULT;

	if (get_user(k->num_sys_files, &u->num_sys_files))
		return -EFAULT;

	for (i = 0; i < k->num_sys_files; i++)
		if (copy_load_driver_file32(&k->sys_files[i], &u->sys_files[i]))
			return -EFAULT;

	if (get_user(k->num_settings, &u->num_settings))
		return -EFAULT;

	if (copy_from_user(&k->settings, &u->settings,
			   sizeof(u->settings[0]) * k->num_settings))
		return -EFAULT;

	if (get_user(k->num_bin_files, &u->num_bin_files))
		return -EFAULT;

	for (i = 0; i < k->num_bin_files; i++)
		if (copy_load_driver_file32(&k->bin_files[i], &u->bin_files[i]))
			return -EFAULT;

	return 0;
}

static long wrapper_ioctl_compat(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	int ret = 0;
	void __user *addr = (void __user *)arg;
	struct load_driver *kdriver;
	struct load_driver32 __user *udriver = addr;
	struct load_driver_file kfile;
	struct load_driver_file32 __user *ufile = addr;

	ENTER1("cmd: 0x%x", cmd);

	switch (cmd) {
	case WRAP_IOCTL_LOAD_DEVICE32:
		return wrapper_ioctl(file, WRAP_IOCTL_LOAD_DEVICE, arg);
	case WRAP_IOCTL_LOAD_DRIVER32:
		TRACE1("loading driver at %p", addr);
		kdriver = vmalloc(sizeof(*kdriver));
		if (!kdriver) {
			ret = -ENOMEM;
			break;
		}

		ret = copy_load_driver32(kdriver, udriver);
		if (!ret)
			ret = load_user_space_driver(kdriver);

		vfree(kdriver);
		break;
	case WRAP_IOCTL_LOAD_BIN_FILE32:
		ret = copy_load_driver_file32(&kfile, ufile);
		if (ret)
			break;

		ret = add_bin_file(&kfile);
		break;
	default:
		ERROR("unknown ioctl 0x%x", cmd);
		ret = -EINVAL;
		break;
	}
	complete(&loader_complete);
	EXIT1(return ret);
}
#endif

static int wrapper_ioctl_release(struct inode *inode, struct file *file)
{
	ENTER1("");
	complete(&loader_complete);
	return 0;
}

static struct file_operations wrapper_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= wrapper_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= wrapper_ioctl_compat,
#endif
	.release	= wrapper_ioctl_release,
};

static struct miscdevice wrapper_misc = {
	.name	= DRIVER_NAME,
	.minor	= MISC_DYNAMIC_MINOR,
	.fops	= &wrapper_fops
};

int loader_init(void)
{
	int err;

	InitializeListHead(&wrap_drivers);
	InitializeListHead(&wrap_devices);
	mutex_init(&loader_mutex);
	init_completion(&loader_complete);
	if ((err = misc_register(&wrapper_misc)) < 0) {
		ERROR("couldn't register module (%d)", err);
		unregister_devices();
		EXIT1(return err);
	}
	register_devices();
	EXIT1(return 0);
}

void loader_exit(void)
{
	struct nt_list *cur, *next;

	ENTER1("");
	misc_deregister(&wrapper_misc);
	unregister_devices();
	mutex_lock(&loader_mutex);
	nt_list_for_each_safe(cur, next, &wrap_drivers) {
		struct wrap_driver *driver;
		driver = container_of(cur, struct wrap_driver, list);
		unload_wrap_driver(driver);
	}
	mutex_unlock(&loader_mutex);
	EXIT1(return);
}
