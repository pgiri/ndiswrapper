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
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <asm/uaccess.h>

#include "ndis.h"
#include "iw_ndis.h"
#include "wrapndis.h"
#include "pnp.h"
#include "wrapper.h"

#if LINUX_VERSION_CODE > KERNEL_VERSION(5,16,0)
	#define PDE_DATA pde_data
#endif

#define MAX_PROC_STR_LEN 32

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
static kuid_t proc_kuid;
static kgid_t proc_kgid;
#else
#define proc_kuid proc_uid
#define proc_kgid proc_gid
#define kuid_t uid_t
#define kgid_t gid_t
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
static inline struct inode *file_inode(struct file *f)
{
	return f->f_dentry->d_inode;
}
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3,9,0)
static inline struct inode *file_inode(struct file *f)
{
	return f->f_path.dentry->d_inode;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
static inline void proc_set_user(struct proc_dir_entry *de, kuid_t uid,
				 kgid_t gid)
{
	de->uid = uid;
	de->gid = gid;
}

static inline void proc_remove(struct proc_dir_entry *de)
{
	if (de)
		remove_proc_entry(de->name, de->parent);
}

static inline void *PDE_DATA(const struct inode *inode)
{
	return PDE(inode)->data;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,26)
static inline struct proc_dir_entry *proc_create_data(const char *name,
	umode_t mode, struct proc_dir_entry *parent,
	struct file_operations *fops, void *data)
{
	struct proc_dir_entry *de;

	de = create_proc_entry(name, mode, parent);
	if (de) {
		de->data = data;
		de->proc_fops = fops;
	}

	return de;
}
#endif

static int do_proc_make_entry(const char *name, umode_t mode,
			      struct proc_dir_entry *parent,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
			      const struct proc_ops *fops,
#else
			      struct file_operations *fops,
#endif
			      kuid_t uid, kgid_t gid, struct ndis_device *wnd)
{
	struct proc_dir_entry *de;

	de = proc_create_data(name, mode, parent, fops, wnd);
	if (de == NULL) {
		ERROR("couldn't create proc entry for '%s'", name);
		return -ENOMEM;
	}
	proc_set_user(de, uid, gid);
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
#define PROC_DECLARE_RO(name) \
	static int proc_##name##_open(struct inode *inode, struct file *file) \
	{ \
		return single_open(file, proc_##name##_read, PDE_DATA(inode)); \
	} \
	static const struct proc_ops name##_fops = { \
		.proc_open = proc_##name##_open, \
		.proc_read = seq_read, \
		.proc_lseek = seq_lseek, \
		.proc_release = single_release, \
	};

#define PROC_DECLARE_RW(name) \
	static int proc_##name##_open(struct inode *inode, struct file *file) \
	{ \
		return single_open(file, proc_##name##_read, PDE_DATA(inode)); \
	} \
	static const struct proc_ops name##_fops = { \
		.proc_open = proc_##name##_open, \
		.proc_read = seq_read, \
		.proc_lseek = seq_lseek, \
		.proc_release = single_release, \
		.proc_write = proc_##name##_write, \
	};
#else
#define PROC_DECLARE_RO(name) \
	static int proc_##name##_open(struct inode *inode, struct file *file) \
	{ \
		return single_open(file, proc_##name##_read, PDE_DATA(inode)); \
	} \
	static struct file_operations name##_fops = { \
		.owner = THIS_MODULE, \
		.open = proc_##name##_open, \
		.read = seq_read, \
		.llseek = seq_lseek, \
		.release = single_release, \
	};

#define PROC_DECLARE_RW(name) \
	static int proc_##name##_open(struct inode *inode, struct file *file) \
	{ \
		return single_open(file, proc_##name##_read, PDE_DATA(inode)); \
	} \
	static struct file_operations name##_fops = { \
		.owner = THIS_MODULE, \
		.open = proc_##name##_open, \
		.read = seq_read, \
		.llseek = seq_lseek, \
		.release = single_release, \
		.write = proc_##name##_write, \
	};
#endif

#define proc_make_entry_ro(name, parent, wnd) \
	do_proc_make_entry(#name, S_IFREG | S_IRUSR | S_IRGRP, parent, \
			   &name##_fops, proc_kuid, proc_kgid, wnd)
#define proc_make_entry_rw(name, parent, wnd) \
	do_proc_make_entry(#name, \
			   S_IFREG | S_IRUSR | S_IRGRP | S_IWUSR | S_IWGRP, \
			   parent, &name##_fops, proc_kuid, proc_kgid, wnd)

#define add_text(fmt, ...) seq_printf(sf, fmt, ##__VA_ARGS__)

static struct proc_dir_entry *wrap_procfs_entry;

static int proc_stats_read(struct seq_file *sf, void *v)
{
	struct ndis_device *wnd = (struct ndis_device *)sf->private;
	struct ndis_wireless_stats stats;
	NDIS_STATUS res;
	ndis_rssi rssi;

	res = mp_query(wnd, OID_802_11_RSSI, &rssi, sizeof(rssi));
	if (!res)
		add_text("signal_level=%d dBm\n", (s32)rssi);

	res = mp_query(wnd, OID_802_11_STATISTICS, &stats, sizeof(stats));
	if (!res) {
		add_text("tx_frames=%llu\n", stats.tx_frag);
		add_text("tx_multicast_frames=%llu\n", stats.tx_multi_frag);
		add_text("tx_failed=%llu\n", stats.failed);
		add_text("tx_retry=%llu\n", stats.retry);
		add_text("tx_multi_retry=%llu\n", stats.multi_retry);
		add_text("tx_rtss_success=%llu\n", stats.rtss_succ);
		add_text("tx_rtss_fail=%llu\n", stats.rtss_fail);
		add_text("ack_fail=%llu\n", stats.ack_fail);
		add_text("frame_duplicates=%llu\n", stats.frame_dup);
		add_text("rx_frames=%llu\n", stats.rx_frag);
		add_text("rx_multicast_frames=%llu\n", stats.rx_multi_frag);
		add_text("fcs_errors=%llu\n", stats.fcs_err);
	}

	return 0;
}

PROC_DECLARE_RO(stats)

static int proc_encr_read(struct seq_file *sf, void *v)
{
	struct ndis_device *wnd = (struct ndis_device *)sf->private;
	int i, encr_status, auth_mode, infra_mode;
	NDIS_STATUS res;
	struct ndis_essid essid;
	mac_address ap_address;

	res = mp_query(wnd, OID_802_11_BSSID,
		       &ap_address, sizeof(ap_address));
	if (res)
		memset(ap_address, 0, ETH_ALEN);
	add_text("ap_address=" MACSTRSEP "\n", MAC2STR(ap_address));

	res = mp_query(wnd, OID_802_11_SSID, &essid, sizeof(essid));
	if (!res)
		add_text("essid=%.*s\n", essid.length, essid.essid);

	res = mp_query_int(wnd, OID_802_11_ENCRYPTION_STATUS, &encr_status);
	if (!res) {
		typeof(&wnd->encr_info.keys[0]) tx_key;
		add_text("tx_key=%u\n", wnd->encr_info.tx_key_index);
		add_text("key=");
		tx_key = &wnd->encr_info.keys[wnd->encr_info.tx_key_index];
		if (tx_key->length > 0)
			for (i = 0; i < tx_key->length; i++)
				add_text("%2.2X", tx_key->key[i]);
		else
			add_text("off");
		add_text("\n");
		add_text("encr_mode=%d\n", encr_status);
	}
	res = mp_query_int(wnd, OID_802_11_AUTHENTICATION_MODE, &auth_mode);
	if (!res)
		add_text("auth_mode=%d\n", auth_mode);
	res = mp_query_int(wnd, OID_802_11_INFRASTRUCTURE_MODE, &infra_mode);
	add_text("mode=%s\n", (infra_mode == Ndis802_11IBSS) ? "adhoc" :
		 (infra_mode == Ndis802_11Infrastructure) ? "managed" : "auto");

	return 0;
}

PROC_DECLARE_RO(encr)

static int proc_hw_read(struct seq_file *sf, void *v)
{
	struct ndis_device *wnd = (struct ndis_device *)sf->private;
	struct ndis_configuration config;
	enum ndis_power power_mode;
	NDIS_STATUS res;
	ndis_tx_power_level tx_power;
	ULONG bit_rate;
	ndis_rts_threshold rts_threshold;
	ndis_fragmentation_threshold frag_threshold;
	ndis_antenna antenna;
	ULONG packet_filter;
	int n;
	mac_address mac;
	char *hw_status[] = {"ready", "initializing", "resetting", "closing",
			     "not ready"};

	res = mp_query_int(wnd, OID_GEN_HARDWARE_STATUS, &n);
	if (res == NDIS_STATUS_SUCCESS && n >= 0 && n < ARRAY_SIZE(hw_status))
		add_text("status=%s\n", hw_status[n]);

	res = mp_query(wnd, OID_802_3_CURRENT_ADDRESS, mac, sizeof(mac));
	if (!res)
		add_text("mac: " MACSTRSEP "\n", MAC2STR(mac));
	res = mp_query(wnd, OID_802_11_CONFIGURATION, &config, sizeof(config));
	if (!res) {
		add_text("beacon_period=%u msec\n", config.beacon_period);
		add_text("atim_window=%u msec\n", config.atim_window);
		add_text("frequency=%u kHz\n", config.ds_config);
		add_text("hop_pattern=%u\n", config.fh_config.hop_pattern);
		add_text("hop_set=%u\n", config.fh_config.hop_set);
		add_text("dwell_time=%u msec\n", config.fh_config.dwell_time);
	}

	res = mp_query(wnd, OID_802_11_TX_POWER_LEVEL,
		       &tx_power, sizeof(tx_power));
	if (!res)
		add_text("tx_power=%u mW\n", tx_power);

	res = mp_query(wnd, OID_GEN_LINK_SPEED, &bit_rate, sizeof(bit_rate));
	if (!res)
		add_text("bit_rate=%u kBps\n", (u32)bit_rate / 10);

	res = mp_query(wnd, OID_802_11_RTS_THRESHOLD,
		       &rts_threshold, sizeof(rts_threshold));
	if (!res)
		add_text("rts_threshold=%u bytes\n", rts_threshold);

	res = mp_query(wnd, OID_802_11_FRAGMENTATION_THRESHOLD,
		       &frag_threshold, sizeof(frag_threshold));
	if (!res)
		add_text("frag_threshold=%u bytes\n", frag_threshold);

	res = mp_query_int(wnd, OID_802_11_POWER_MODE, &power_mode);
	if (!res)
		add_text("power_mode=%s\n",
			 (power_mode == NDIS_POWER_OFF) ? "always_on" :
			 (power_mode == NDIS_POWER_MAX) ? "max_savings" :
							  "min_savings");

	res = mp_query(wnd, OID_802_11_NUMBER_OF_ANTENNAS,
		       &antenna, sizeof(antenna));
	if (!res)
		add_text("num_antennas=%u\n", antenna);

	res = mp_query(wnd, OID_802_11_TX_ANTENNA_SELECTED,
		       &antenna, sizeof(antenna));
	if (!res)
		add_text("tx_antenna=%u\n", antenna);

	res = mp_query(wnd, OID_802_11_RX_ANTENNA_SELECTED,
		       &antenna, sizeof(antenna));
	if (!res)
		add_text("rx_antenna=%u\n", antenna);

	add_text("encryption_modes=%s%s%s%s%s%s%s\n",
		 test_bit(Ndis802_11Encryption1Enabled, &wnd->capa.encr) ?
		 "WEP" : "none",
		 test_bit(Ndis802_11Encryption2Enabled, &wnd->capa.encr) ?
		 "; TKIP with WPA" : "",
		 test_bit(Ndis802_11AuthModeWPA2, &wnd->capa.auth) ?
		 ", WPA2" : "",
		 test_bit(Ndis802_11AuthModeWPA2PSK, &wnd->capa.auth) ?
		 ", WPA2PSK" : "",
		 test_bit(Ndis802_11Encryption3Enabled, &wnd->capa.encr) ?
		 "; AES/CCMP with WPA" : "",
		 test_bit(Ndis802_11AuthModeWPA2, &wnd->capa.auth) ?
		 ", WPA2" : "",
		 test_bit(Ndis802_11AuthModeWPA2PSK, &wnd->capa.auth) ?
		 ", WPA2PSK" : "");

	res = mp_query_int(wnd, OID_GEN_CURRENT_PACKET_FILTER, &packet_filter);
	if (!res) {
		if (packet_filter != wnd->packet_filter)
			WARNING("wrong packet_filter? 0x%08x, 0x%08x\n",
				packet_filter, wnd->packet_filter);
		add_text("packet_filter: 0x%08x\n", packet_filter);
	}

	return 0;
}

PROC_DECLARE_RO(hw)

static int proc_settings_read(struct seq_file *sf, void *v)
{
	struct ndis_device *wnd = (struct ndis_device *)sf->private;
	struct wrap_device_setting *setting;

	add_text("hangcheck_interval=%d\n", (hangcheck_interval == 0) ?
		 (wnd->hangcheck_interval / HZ) : -1);

	list_for_each_entry(setting, &wnd->wd->settings, list) {
		add_text("%s=%s\n", setting->name, setting->value);
	}

	list_for_each_entry(setting, &wnd->wd->driver->settings, list) {
		add_text("%s=%s\n", setting->name, setting->value);
	}

	return 0;
}

static ssize_t proc_settings_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct ndis_device *wnd = PDE_DATA(file_inode(file));
	char setting[MAX_PROC_STR_LEN], *p;
	unsigned int i;
	NDIS_STATUS res;

	if (count > MAX_PROC_STR_LEN)
		return -EINVAL;

	memset(setting, 0, sizeof(setting));
	if (copy_from_user(setting, buf, count))
		return -EFAULT;

	if ((p = strchr(setting, '\n')))
		*p = 0;

	if ((p = strchr(setting, '=')))
		*p = 0;

	if (!strcmp(setting, "hangcheck_interval")) {
		if (!p)
			return -EINVAL;
		p++;
		i = simple_strtol(p, NULL, 10);
		hangcheck_del(wnd);
		if (i > 0) {
			wnd->hangcheck_interval = i * HZ;
			hangcheck_add(wnd);
		}
	} else if (!strcmp(setting, "suspend")) {
		if (!p)
			return -EINVAL;
		p++;
		i = simple_strtol(p, NULL, 10);
		if (i <= 0 || i > 3)
			return -EINVAL;
		i = -1;
		if (wrap_is_pci_bus(wnd->wd->dev_bus))
			i = wrap_pnp_suspend_pci_device(wnd->wd->pci.pdev,
							PMSG_SUSPEND);
		else if (wrap_is_usb_bus(wnd->wd->dev_bus))
			i = wrap_pnp_suspend_usb_device(wnd->wd->usb.intf,
							PMSG_SUSPEND);
		if (i)
			return -EINVAL;
	} else if (!strcmp(setting, "resume")) {
		i = -1;
		if (wrap_is_pci_bus(wnd->wd->dev_bus))
			i = wrap_pnp_resume_pci_device(wnd->wd->pci.pdev);
		else if (wrap_is_usb_bus(wnd->wd->dev_bus))
			i = wrap_pnp_resume_usb_device(wnd->wd->usb.intf);
		if (i)
			return -EINVAL;
	} else if (!strcmp(setting, "stats_enabled")) {
		if (!p)
			return -EINVAL;
		p++;
		i = simple_strtol(p, NULL, 10);
		if (i > 0)
			wnd->iw_stats_enabled = TRUE;
		else
			wnd->iw_stats_enabled = FALSE;
	} else if (!strcmp(setting, "packet_filter")) {
		if (!p)
			return -EINVAL;
		p++;
		i = simple_strtol(p, NULL, 10);
		res = mp_set_int(wnd, OID_GEN_CURRENT_PACKET_FILTER, i);
		if (res)
			WARNING("setting packet_filter failed: %08X", res);
	} else if (!strcmp(setting, "reinit")) {
		if (ndis_reinit(wnd) != NDIS_STATUS_SUCCESS)
			return -EFAULT;
	} else {
		struct ndis_configuration_parameter param;
		struct unicode_string key;
		struct ansi_string ansi;

		if (!p)
			return -EINVAL;
		p++;
		RtlInitAnsiString(&ansi, p);
		if (RtlAnsiStringToUnicodeString(&param.data.string, &ansi,
						 TRUE) != STATUS_SUCCESS)
			EXIT1(return -EFAULT);
		param.type = NdisParameterString;
		RtlInitAnsiString(&ansi, setting);
		if (RtlAnsiStringToUnicodeString(&key, &ansi,
						 TRUE) != STATUS_SUCCESS) {
			RtlFreeUnicodeString(&param.data.string);
			EXIT1(return -EINVAL);
		}
		NdisWriteConfiguration(&res, wnd->nmb, &key, &param);
		RtlFreeUnicodeString(&key);
		RtlFreeUnicodeString(&param.data.string);
		if (res != NDIS_STATUS_SUCCESS)
			return -EFAULT;
	}
	return count;
}

PROC_DECLARE_RW(settings)

int wrap_procfs_add_ndis_device(struct ndis_device *wnd)
{
	int ret;

	if (wrap_procfs_entry == NULL)
		return -ENOMEM;

	if (wnd->procfs_iface) {
		ERROR("%s already registered?", wnd->net_dev->name);
		return -EINVAL;
	}
	wnd->procfs_iface = proc_mkdir(wnd->net_dev->name, wrap_procfs_entry);
	if (wnd->procfs_iface == NULL) {
		ERROR("couldn't create proc directory");
		return -ENOMEM;
	}
	proc_set_user(wnd->procfs_iface, proc_kuid, proc_kgid);

	ret = proc_make_entry_ro(hw, wnd->procfs_iface, wnd);
	if (ret)
		goto err_hw;

	ret = proc_make_entry_ro(stats, wnd->procfs_iface, wnd);
	if (ret)
		goto err_stats;

	ret = proc_make_entry_ro(encr, wnd->procfs_iface, wnd);
	if (ret)
		goto err_encr;

	ret = proc_make_entry_rw(settings, wnd->procfs_iface, wnd);
	if (ret)
		goto err_settings;

	return 0;

err_settings:
	remove_proc_entry("encr", wnd->procfs_iface);
err_encr:
	remove_proc_entry("stats", wnd->procfs_iface);
err_stats:
	remove_proc_entry("hw", wnd->procfs_iface);
err_hw:
	proc_remove(wnd->procfs_iface);
	wnd->procfs_iface = NULL;
	return -ENOMEM;
}

void wrap_procfs_remove_ndis_device(struct ndis_device *wnd)
{
	struct proc_dir_entry *procfs_iface = xchg(&wnd->procfs_iface, NULL);

	if (procfs_iface == NULL)
		return;
	remove_proc_entry("hw", procfs_iface);
	remove_proc_entry("stats", procfs_iface);
	remove_proc_entry("encr", procfs_iface);
	remove_proc_entry("settings", procfs_iface);
	if (wrap_procfs_entry)
		proc_remove(procfs_iface);
}

static int proc_debug_read(struct seq_file *sf, void *v)
{
#if ALLOC_DEBUG
	enum alloc_type type;
#endif

	add_text("%d\n", debug);
#if ALLOC_DEBUG
	for (type = 0; type < ALLOC_TYPE_MAX; type++)
		add_text("total size of allocations in %s: %d\n",
			 alloc_type_name[type], alloc_size(type));
#endif
	return 0;
}

static ssize_t proc_debug_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	int i;
	char setting[MAX_PROC_STR_LEN], *p;

	if (count > MAX_PROC_STR_LEN)
		return -EINVAL;

	memset(setting, 0, sizeof(setting));
	if (copy_from_user(setting, buf, count))
		return -EFAULT;

	if ((p = strchr(setting, '\n')))
		*p = 0;

	if ((p = strchr(setting, '=')))
		*p = 0;

	i = simple_strtol(setting, NULL, 10);
	if (i >= 0 && i < 10)
		debug = i;
	else
		return -EINVAL;
	return count;
}

PROC_DECLARE_RW(debug)

int wrap_procfs_init(void)
{
	int ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
	struct user_namespace *ns = current_user_ns();
	proc_kuid = make_kuid(ns, proc_uid);
	if (!uid_valid(proc_kuid)) {
		ERROR("invalid UID\n");
		return -EINVAL;
	}
	proc_kgid = make_kgid(ns, proc_gid);
	if (!gid_valid(proc_kgid)) {
		ERROR("invalid GID\n");
		return -EINVAL;
	}
#endif

	wrap_procfs_entry = proc_mkdir(DRIVER_NAME, proc_net_root);
	if (wrap_procfs_entry == NULL) {
		ERROR("couldn't create procfs directory");
		return -ENOMEM;
	}
	proc_set_user(wrap_procfs_entry, proc_kuid, proc_kgid);

	ret = proc_make_entry_rw(debug, wrap_procfs_entry, NULL);

	return ret;
}

void wrap_procfs_remove(void)
{
	if (wrap_procfs_entry == NULL)
		return;
	remove_proc_entry("debug", wrap_procfs_entry);
	proc_remove(wrap_procfs_entry);
}
