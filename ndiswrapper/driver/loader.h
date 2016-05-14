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

#ifndef _LOADER_H_
#define _LOADER_H_

#include "ndiswrapper.h"

#ifndef __KERNEL__
#define __user
#endif

struct load_driver_file {
	char driver_name[MAX_DRIVER_NAME_LEN];
	char name[MAX_DRIVER_NAME_LEN];
	size_t size;
	void __user *data;
};

struct load_device_setting {
	char name[MAX_SETTING_NAME_LEN];
	char value[MAX_SETTING_VALUE_LEN];
};

struct load_device {
	int bus;
	int vendor;
	int device;
	int subvendor;
	int subdevice;
	char conf_file_name[MAX_DRIVER_NAME_LEN];
	char driver_name[MAX_DRIVER_NAME_LEN];
};

struct load_driver {
	char name[MAX_DRIVER_NAME_LEN];
	char conf_file_name[MAX_DRIVER_NAME_LEN];
	unsigned int num_sys_files;
	struct load_driver_file sys_files[MAX_DRIVER_PE_IMAGES];
	unsigned int num_settings;
	struct load_device_setting settings[MAX_DEVICE_SETTINGS];
	unsigned int num_bin_files;
	struct load_driver_file bin_files[MAX_DRIVER_BIN_FILES];
};

#define WRAP_IOCTL_LOAD_DEVICE _IOW(('N' + 'd' + 'i' + 'S'), 0,	\
				    struct load_device *)
#define WRAP_IOCTL_LOAD_DRIVER _IOW(('N' + 'd' + 'i' + 'S'), 1,	\
				    struct load_driver *)
#define WRAP_IOCTL_LOAD_BIN_FILE _IOW(('N' + 'd' + 'i' + 'S'), 2,	\
				      struct load_driver_file *)

#ifdef CONFIG_COMPAT
struct load_driver_file32 {
	char driver_name[MAX_DRIVER_NAME_LEN];
	char name[MAX_DRIVER_NAME_LEN];
	u32 size;
	u32 data;
};

struct load_driver32 {
	char name[MAX_DRIVER_NAME_LEN];
	char conf_file_name[MAX_DRIVER_NAME_LEN];
	u32 num_sys_files;
	struct load_driver_file32 sys_files[MAX_DRIVER_PE_IMAGES];
	u32 num_settings;
	struct load_device_setting settings[MAX_DEVICE_SETTINGS];
	u32 num_bin_files;
	struct load_driver_file32 bin_files[MAX_DRIVER_BIN_FILES];
} __packed;

#define WRAP_IOCTL_LOAD_DEVICE32 _IOW(('N' + 'd' + 'i' + 'S'), 0, u32)
#define WRAP_IOCTL_LOAD_DRIVER32 _IOW(('N' + 'd' + 'i' + 'S'), 1, u32)
#define WRAP_IOCTL_LOAD_BIN_FILE32 _IOW(('N' + 'd' + 'i' + 'S'), 2, u32)
#endif

#define WRAP_CMD_LOAD_DEVICE "load_device"
#define WRAP_CMD_LOAD_DRIVER "load_driver"
#define WRAP_CMD_LOAD_BIN_FILE "load_bin_file"

int loader_init(void);
void loader_exit(void);

#ifdef __KERNEL__
struct wrap_device *load_wrap_device(struct load_device *load_device);
struct wrap_driver *load_wrap_driver(struct wrap_device *device);
struct wrap_bin_file *get_bin_file(char *bin_file_name);
void free_bin_file(struct wrap_bin_file *bin_file);
void unload_wrap_driver(struct wrap_driver *driver);
void unload_wrap_device(struct wrap_device *wd);
struct wrap_device *get_wrap_device(void *dev, int bus_type);

extern struct mutex loader_mutex;
#endif

#endif /* LOADER_H */
