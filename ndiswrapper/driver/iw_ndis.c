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

#include <linux/wireless.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_arp.h>
#include <linux/usb.h>
#include <linux/random.h>

#include <net/iw_handler.h>
#include <linux/rtnetlink.h>
#include <asm/uaccess.h>

#include "iw_ndis.h"
#include "wrapndis.h"

#ifdef CONFIG_WIRELESS_EXT

static int freq_chan[] = { 2412, 2417, 2422, 2427, 2432, 2437, 2442,
			   2447, 2452, 2457, 2462, 2467, 2472, 2484 };

static const char *network_names[] = {"IEEE 802.11FH", "IEEE 802.11b",
				      "IEEE 802.11a", "IEEE 802.11g", "Auto"};

static int set_essid(struct ndis_device *wnd, const char *ssid, int ssid_len)
{
	NDIS_STATUS res;
	struct ndis_essid req;

	if (ssid_len > NDIS_ESSID_MAX_SIZE)
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.length = ssid_len;
	if (ssid_len)
		memcpy(&req.essid, ssid, ssid_len);

	res = mp_set(wnd, OID_802_11_SSID, &req, sizeof(req));
	if (res) {
		WARNING("setting essid failed (%08X)", res);
		EXIT2(return -EINVAL);
	}
	memcpy(&wnd->essid, &req, sizeof(req));
	EXIT2(return 0);
}

static int set_iw_auth_mode(struct ndis_device *wnd, int wpa_version,
			    int auth_80211_alg)
{
	NDIS_STATUS res;
	ULONG auth_mode;

	ENTER2("%d, %d", wpa_version, auth_80211_alg);
	if (wpa_version & IW_AUTH_WPA_VERSION_WPA2) {
		if (wnd->iw_auth_key_mgmt & IW_AUTH_KEY_MGMT_802_1X)
			auth_mode = Ndis802_11AuthModeWPA2;
		else
			auth_mode = Ndis802_11AuthModeWPA2PSK;
	} else if (wpa_version & IW_AUTH_WPA_VERSION_WPA) {
		if (wnd->iw_auth_key_mgmt & IW_AUTH_KEY_MGMT_802_1X)
			auth_mode = Ndis802_11AuthModeWPA;
		else if (wnd->iw_auth_key_mgmt & IW_AUTH_KEY_MGMT_PSK)
			auth_mode = Ndis802_11AuthModeWPAPSK;
		else
			auth_mode = Ndis802_11AuthModeWPANone;
	} else if (auth_80211_alg & IW_AUTH_ALG_SHARED_KEY) {
		if (auth_80211_alg & IW_AUTH_ALG_OPEN_SYSTEM)
			auth_mode = Ndis802_11AuthModeAutoSwitch;
		else
			auth_mode = Ndis802_11AuthModeShared;
	} else
		auth_mode = Ndis802_11AuthModeOpen;

	res = mp_set_int(wnd, OID_802_11_AUTHENTICATION_MODE, auth_mode);
	if (res) {
		WARNING("setting auth mode to %u failed (%08X)",
			auth_mode, res);
		if (res == NDIS_STATUS_INVALID_DATA)
			EXIT2(return -EINVAL);
		return -EOPNOTSUPP;
	}
	wnd->iw_auth_wpa_version = wpa_version;
	wnd->iw_auth_80211_alg = auth_80211_alg;
	EXIT2(return 0);
}

static int set_auth_mode(struct ndis_device *wnd)
{
	return set_iw_auth_mode(wnd, wnd->iw_auth_wpa_version,
				wnd->iw_auth_80211_alg);
}

static enum ndis_priv_filter ndis_priv_mode(struct ndis_device *wnd)
{
	if (wnd->iw_auth_wpa_version & IW_AUTH_WPA_VERSION_WPA2 ||
	    wnd->iw_auth_wpa_version & IW_AUTH_WPA_VERSION_WPA)
		return Ndis802_11PrivFilter8021xWEP;
	else
		return Ndis802_11PrivFilterAcceptAll;
}

static int set_priv_filter(struct ndis_device *wnd)
{
	NDIS_STATUS res;
	ULONG flags;

	flags = ndis_priv_mode(wnd);
	ENTER2("filter: %d", flags);
	res = mp_set_int(wnd, OID_802_11_PRIVACY_FILTER, flags);
	if (res)
		TRACE2("setting privacy filter to %d failed (%08X)",
		       flags, res);
	EXIT2(return 0);
}

static int set_encr_mode(struct ndis_device *wnd)
{
	return set_iw_encr_mode(wnd, wnd->iw_auth_cipher_pairwise,
				wnd->iw_auth_cipher_group);
}

static int set_assoc_params(struct ndis_device *wnd)
{
	TRACE2("wpa_version=0x%x auth_alg=0x%x key_mgmt=0x%x "
	       "cipher_pairwise=0x%x cipher_group=0x%x",
	       wnd->iw_auth_wpa_version, wnd->iw_auth_80211_alg,
	       wnd->iw_auth_key_mgmt, wnd->iw_auth_cipher_pairwise,
	       wnd->iw_auth_cipher_group);
	set_auth_mode(wnd);
	set_priv_filter(wnd);
	set_encr_mode(wnd);
	return 0;
}

static int iw_set_essid(struct net_device *dev, struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	int length;

	ENTER2("");
	/* there is no way to turn off essid other than to set to
	 * random bytes; instead, we use off to mean any */
	if (wrqu->essid.flags) {
		length = wrqu->essid.length;
		/* Strip '\0' appended by wireless extensions 19 and older */
		if (length > 0 && extra[length - 1] == '\0')
			length--;
		TRACE2("%d", length);
		if (length <= 0 || length > NDIS_ESSID_MAX_SIZE)
			EXIT2(return -EINVAL);
	} else
		length = 0;

	set_assoc_params(wnd);

	if (set_essid(wnd, extra, length))
		EXIT2(return -EINVAL);

	EXIT2(return 0);
}

static int iw_get_essid(struct net_device *dev, struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	NDIS_STATUS res;
	struct ndis_essid req;

	ENTER2("");
	memset(&req, 0, sizeof(req));
	res = mp_query(wnd, OID_802_11_SSID, &req, sizeof(req));
	if (res) {
		WARNING("getting essid failed (%08X)", res);
		EXIT2(return -EOPNOTSUPP);
	}
	memcpy(extra, req.essid, req.length);
	if (req.length > 0)
		wrqu->essid.flags = 1;
	else
		wrqu->essid.flags = 0;
	wrqu->essid.length = req.length;
	EXIT2(return 0);
}

/* index must be 0 - N, as per NDIS */
static int add_wep_key(struct ndis_device *wnd, char *key, int key_len,
		       int index)
{
	struct ndis_encr_key ndis_key;
	NDIS_STATUS res;

	ENTER2("key index: %d, length: %d", index, key_len);
	if (key_len <= 0 || key_len > NDIS_ENCODING_TOKEN_MAX) {
		WARNING("invalid key length (%d)", key_len);
		EXIT2(return -EINVAL);
	}
	if (index < 0 || index >= MAX_ENCR_KEYS) {
		WARNING("invalid key index (%d)", index);
		EXIT2(return -EINVAL);
	}
	ndis_key.struct_size = sizeof(ndis_key);
	ndis_key.length = key_len;
	memcpy(&ndis_key.key, key, key_len);
	ndis_key.index = index;

	if (index == wnd->encr_info.tx_key_index) {
		ndis_key.index |= (1 << 31);
		res = set_iw_encr_mode(wnd, IW_AUTH_CIPHER_WEP104,
				       IW_AUTH_CIPHER_NONE);
		if (res)
			WARNING("encryption couldn't be enabled (%08X)", res);
	}
	TRACE2("key %d: " MACSTRSEP, index, MAC2STR(key));
	res = mp_set(wnd, OID_802_11_ADD_WEP, &ndis_key, sizeof(ndis_key));
	if (res) {
		WARNING("adding encryption key %d failed (%08X)",
			index+1, res);
		EXIT2(return -EINVAL);
	}

	/* Atheros driver messes up ndis_key during ADD_WEP, so
	 * don't rely on that; instead use info in key and key_len */
	wnd->encr_info.keys[index].length = key_len;
	memcpy(&wnd->encr_info.keys[index].key, key, key_len);

	EXIT2(return 0);
}

static int set_infra_mode(struct ndis_device *wnd,
			  enum ndis_infrastructure_mode mode)
{
	NDIS_STATUS res;
	unsigned int i;

	ENTER2("%d", mode);
	res = mp_query_int(wnd, OID_802_11_INFRASTRUCTURE_MODE,
			   &wnd->infrastructure_mode);
	if (res != NDIS_STATUS_SUCCESS) {
		WARNING("getting operating mode failed (%08X)", res);
		EXIT2(return -EINVAL);
	}
	if (wnd->infrastructure_mode == mode)
		EXIT2(return 0);
	res = mp_set_int(wnd, OID_802_11_INFRASTRUCTURE_MODE, mode);
	if (res) {
		WARNING("setting operating mode to %d failed (%08X)",
			mode, res);
		EXIT2(return -EINVAL);
	}
	/* NDIS drivers clear keys when infrastructure mode is
	 * changed. But Linux tools assume otherwise. So set the
	 * keys */
	if (wnd->iw_auth_key_mgmt == 0 ||
	    wnd->iw_auth_key_mgmt == IW_AUTH_KEY_MGMT_802_1X) {
		for (i = 0; i < MAX_ENCR_KEYS; i++) {
			if (wnd->encr_info.keys[i].length > 0)
				add_wep_key(wnd, wnd->encr_info.keys[i].key,
					    wnd->encr_info.keys[i].length, i);
		}
	}
	wnd->infrastructure_mode = mode;
	EXIT2(return 0);
}

static int iw_set_infra_mode(struct net_device *dev,
			     struct iw_request_info *info,
			     union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	enum ndis_infrastructure_mode ndis_mode;

	ENTER2("%d", wrqu->mode);
	switch (wrqu->mode) {
	case IW_MODE_ADHOC:
		ndis_mode = Ndis802_11IBSS;
		break;
	case IW_MODE_INFRA:
		ndis_mode = Ndis802_11Infrastructure;
		break;
	case IW_MODE_AUTO:
		ndis_mode = Ndis802_11AutoUnknown;
		break;
	default:
		EXIT2(return -EINVAL);
	}

	if (set_infra_mode(wnd, ndis_mode))
		EXIT2(return -EINVAL);

	EXIT2(return 0);
}

static int iw_get_infra_mode(struct net_device *dev,
			     struct iw_request_info *info,
			     union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	int ndis_mode, iw_mode;
	NDIS_STATUS res;

	ENTER2("");
	res = mp_query_int(wnd, OID_802_11_INFRASTRUCTURE_MODE, &ndis_mode);
	if (res) {
		WARNING("getting operating mode failed (%08X)", res);
		EXIT2(return -EOPNOTSUPP);
	}

	switch (ndis_mode) {
	case Ndis802_11IBSS:
		iw_mode = IW_MODE_ADHOC;
		break;
	case Ndis802_11Infrastructure:
		iw_mode = IW_MODE_INFRA;
		break;
	case Ndis802_11AutoUnknown:
		iw_mode = IW_MODE_AUTO;
		break;
	default:
		ERROR("invalid operating mode (%u)", ndis_mode);
		EXIT2(return -EINVAL);
	}
	wrqu->mode = iw_mode;
	EXIT2(return 0);
}

static const char *network_type_to_name(int net_type)
{
	if (net_type >= 0 && net_type < ARRAY_SIZE(network_names))
		return network_names[net_type];
	else
		return network_names[ARRAY_SIZE(network_names) - 1];
}

static int iw_get_network_type(struct net_device *dev,
			       struct iw_request_info *info,
			       union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	unsigned int network_type;
	NDIS_STATUS res;

	ENTER2("");
	res = mp_query_int(wnd, OID_802_11_NETWORK_TYPE_IN_USE,
			   &network_type);
	if (res) {
		WARNING("getting network type failed: %08X", res);
		network_type = -1;
	}
	strncpy(wrqu->name, network_type_to_name(network_type),
		sizeof(wrqu->name) - 1);
	wrqu->name[sizeof(wrqu->name)-1] = 0;
	return 0;
}

static int iw_get_freq(struct net_device *dev, struct iw_request_info *info,
		       union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	NDIS_STATUS res;
	struct ndis_configuration req;

	ENTER2("");
	memset(&req, 0, sizeof(req));
	res = mp_query(wnd, OID_802_11_CONFIGURATION, &req, sizeof(req));
	if (res) {
		WARNING("getting configuration failed (%08X)", res);
		EXIT2(return -EOPNOTSUPP);
	}

	memset(&(wrqu->freq), 0, sizeof(struct iw_freq));

	/* see comment in wireless.h above the "struct iw_freq"
	   definition for an explanation of this if
	   NOTE: 1000000 is due to the kHz
	*/
	if (req.ds_config > 1000000) {
		wrqu->freq.m = req.ds_config / 10;
		wrqu->freq.e = 1;
	}
	else
		wrqu->freq.m = req.ds_config;

	/* convert from kHz to Hz */
	wrqu->freq.e += 3;

	return 0;
}

static int iw_set_freq(struct net_device *dev, struct iw_request_info *info,
		       union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	NDIS_STATUS res;
	struct ndis_configuration req;

	ENTER2("");
	/* this OID is valid only when not associated */
	if (netif_carrier_ok(wnd->net_dev))
		EXIT2(return 0);
	memset(&req, 0, sizeof(req));
	res = mp_query(wnd, OID_802_11_CONFIGURATION, &req, sizeof(req));
	if (res) {
		WARNING("getting configuration failed (%08X)", res);
		EXIT2(return 0);
	}

	if (wrqu->freq.m < 1000 && wrqu->freq.e == 0) {
		if (wrqu->freq.m >= 1 && wrqu->freq.m <= ARRAY_SIZE(freq_chan))
			req.ds_config = freq_chan[wrqu->freq.m - 1] * 1000;
		else
			return -EINVAL;
	} else {
		int i;
		req.ds_config = wrqu->freq.m;
		for (i = wrqu->freq.e; i > 0; i--)
			req.ds_config *= 10;
		req.ds_config /= 1000;
	}
	res = mp_set(wnd, OID_802_11_CONFIGURATION, &req, sizeof(req));
	if (res)
		WARNING("setting configuration failed (%08X)", res);
	return 0;
}

static int iw_get_tx_power(struct net_device *dev, struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	ndis_tx_power_level ndis_power;
	NDIS_STATUS res;

	ENTER2("");
	res = mp_query(wnd, OID_802_11_TX_POWER_LEVEL,
		       &ndis_power, sizeof(ndis_power));
	if (res)
		return -EOPNOTSUPP;
	wrqu->txpower.flags = IW_TXPOW_MWATT;
	wrqu->txpower.disabled = 0;
	wrqu->txpower.fixed = 0;
	wrqu->txpower.value = ndis_power;
	return 0;
}

static int iw_set_tx_power(struct net_device *dev, struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	ndis_tx_power_level ndis_power;
	NDIS_STATUS res;

	ENTER2("");
	if (wrqu->txpower.disabled)
		ndis_power = 0;
	else {
		if (wrqu->txpower.flags == IW_TXPOW_MWATT)
			ndis_power = wrqu->txpower.value;
		else { // wrqu->txpower.flags == IW_TXPOW_DBM
			if (wrqu->txpower.value > 20)
				ndis_power = 128;
			else if (wrqu->txpower.value < -43)
				ndis_power = 127;
			else {
				signed char tmp;
				tmp = wrqu->txpower.value;
				tmp = -12 - tmp;
				tmp <<= 2;
				ndis_power = (unsigned char)tmp;
			}
		}
	}
	TRACE2("%d", ndis_power);
	res = mp_set(wnd, OID_802_11_TX_POWER_LEVEL,
		     &ndis_power, sizeof(ndis_power));
	if (res)
		EXIT2(return -EOPNOTSUPP);
	if (ndis_power == 0)
		res = disassociate(wnd, 0);
	EXIT2(return 0);
}

static int iw_get_bitrate(struct net_device *dev, struct iw_request_info *info,
			  union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	ULONG ndis_rate;
	int res;

	ENTER2("");
	res = mp_query(wnd, OID_GEN_LINK_SPEED, &ndis_rate, sizeof(ndis_rate));
	if (res) {
		WARNING("getting bitrate failed (%08X)", res);
		ndis_rate = 0;
	}

	wrqu->bitrate.value = ndis_rate * 100;
	return 0;
}

static int iw_set_bitrate(struct net_device *dev, struct iw_request_info *info,
			  union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	int i, n;
	NDIS_STATUS res;
	UCHAR rates[NDIS_MAX_RATES_EX];

	ENTER2("");
	if (wrqu->bitrate.fixed == 0)
		EXIT2(return 0);

	res = mp_query_info(wnd, OID_802_11_SUPPORTED_RATES, &rates,
			    sizeof(rates), &n, NULL);
	if (res) {
		WARNING("getting bit rate failed (%08X)", res);
		EXIT2(return 0);
	}
	for (i = 0; i < n; i++) {
		if (rates[i] & 0x80)
			continue;
		if ((rates[i] & 0x7f) * 500000 > wrqu->bitrate.value) {
			TRACE2("setting rate %d to 0",
			       (rates[i] & 0x7f) * 500000);
			rates[i] = 0;
		}
	}

	res = mp_set(wnd, OID_802_11_DESIRED_RATES, &rates, n);
	if (res) {
		WARNING("setting bit rate failed (%08X)", res);
		EXIT2(return 0);
	}

	return 0;
}

static int iw_set_dummy(struct net_device *dev, struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	/* Do nothing. Used for ioctls that are not implemented. */
	return 0;
}

static int iw_get_rts_threshold(struct net_device *dev,
				struct iw_request_info *info,
				union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	ndis_rts_threshold threshold;
	NDIS_STATUS res;

	ENTER2("");
	res = mp_query(wnd, OID_802_11_RTS_THRESHOLD,
		       &threshold, sizeof(threshold));
	if (res)
		return -EOPNOTSUPP;

	wrqu->rts.value = threshold;
	return 0;
}

static int iw_set_rts_threshold(struct net_device *dev,
				struct iw_request_info *info,
				union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	ndis_rts_threshold threshold;
	NDIS_STATUS res;

	ENTER2("");
	threshold = wrqu->rts.value;
	res = mp_set(wnd, OID_802_11_RTS_THRESHOLD,
		     &threshold, sizeof(threshold));
	if (res == NDIS_STATUS_INVALID_DATA)
		return -EINVAL;
	if (res)
		return -EOPNOTSUPP;

	return 0;
}

static int iw_get_frag_threshold(struct net_device *dev,
				 struct iw_request_info *info,
				 union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	ndis_fragmentation_threshold frag_threshold;
	NDIS_STATUS res;

	ENTER2("");
	res = mp_query(wnd, OID_802_11_FRAGMENTATION_THRESHOLD,
		       &frag_threshold, sizeof(frag_threshold));
	if (res)
		return -ENOTSUPP;

	wrqu->frag.value = frag_threshold;
	return 0;
}

static int iw_set_frag_threshold(struct net_device *dev,
				 struct iw_request_info *info,
				 union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	ndis_rts_threshold threshold;
	NDIS_STATUS res;

	ENTER2("");
	threshold = wrqu->frag.value;
	res = mp_set(wnd, OID_802_11_FRAGMENTATION_THRESHOLD,
		     &threshold, sizeof(threshold));
	if (res == NDIS_STATUS_INVALID_DATA)
		return -EINVAL;
	if (res)
		return -EOPNOTSUPP;
	return 0;
}

int get_ap_address(struct ndis_device *wnd, mac_address ap_addr)
{
	NDIS_STATUS res;

	res = mp_query(wnd, OID_802_11_BSSID, ap_addr, ETH_ALEN);
	TRACE2(MACSTRSEP, MAC2STR(ap_addr));
	if (res) {
		TRACE2("res: %08X", res);
		memset(ap_addr, 0x0, ETH_ALEN);
		EXIT2(return -EOPNOTSUPP);
	}
	EXIT2(return 0);
}

static int iw_get_ap_address(struct net_device *dev,
			     struct iw_request_info *info,
			     union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	mac_address ap_addr;

	ENTER2("");
	get_ap_address(wnd, ap_addr);
	memcpy(wrqu->ap_addr.sa_data, ap_addr, ETH_ALEN);
	wrqu->ap_addr.sa_family = ARPHRD_ETHER;
	EXIT2(return 0);
}

static int iw_set_ap_address(struct net_device *dev,
			     struct iw_request_info *info,
			     union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	NDIS_STATUS res;
	mac_address ap_addr;

	ENTER2("");
	memcpy(ap_addr, wrqu->ap_addr.sa_data, ETH_ALEN);
	TRACE2(MACSTRSEP, MAC2STR(ap_addr));
	res = mp_set(wnd, OID_802_11_BSSID, ap_addr, ETH_ALEN);
	/* user apps may set ap's mac address, which is not required;
	 * they may fail to work if this function fails, so return
	 * success */
	if (res)
		WARNING("setting AP mac address failed (%08X)", res);

	EXIT2(return 0);
}

int set_ndis_auth_mode(struct ndis_device *wnd, ULONG auth_mode)
{
	NDIS_STATUS res;

	ENTER2("%d", auth_mode);
	res = mp_set_int(wnd, OID_802_11_AUTHENTICATION_MODE, auth_mode);
	if (res) {
		WARNING("setting auth mode to %u failed (%08X)",
			auth_mode, res);
		if (res == NDIS_STATUS_INVALID_DATA)
			EXIT2(return -EINVAL);
		return -EOPNOTSUPP;
	}
	switch (auth_mode) {
	case Ndis802_11AuthModeWPA:
		wnd->iw_auth_wpa_version = IW_AUTH_WPA_VERSION_WPA;
		wnd->iw_auth_key_mgmt = IW_AUTH_KEY_MGMT_802_1X;
		break;
	case Ndis802_11AuthModeWPAPSK:
		wnd->iw_auth_wpa_version = IW_AUTH_WPA_VERSION_WPA;
		wnd->iw_auth_key_mgmt = IW_AUTH_KEY_MGMT_PSK;
        break;
	case Ndis802_11AuthModeWPANone:
		wnd->iw_auth_wpa_version = IW_AUTH_WPA_VERSION_DISABLED;
		wnd->iw_auth_key_mgmt = IW_AUTH_KEY_MGMT_PSK;
		break;
	case Ndis802_11AuthModeWPA2:
		wnd->iw_auth_wpa_version = IW_AUTH_WPA_VERSION_WPA2;
		wnd->iw_auth_key_mgmt = IW_AUTH_KEY_MGMT_802_1X;
		break;
	case Ndis802_11AuthModeWPA2PSK:
		wnd->iw_auth_wpa_version = IW_AUTH_WPA_VERSION_WPA2;
		wnd->iw_auth_key_mgmt = IW_AUTH_KEY_MGMT_PSK;
		break;
	case Ndis802_11AuthModeOpen:
		wnd->iw_auth_wpa_version = IW_AUTH_WPA_VERSION_DISABLED;
		wnd->iw_auth_80211_alg = IW_AUTH_ALG_OPEN_SYSTEM;
		break;
	case Ndis802_11AuthModeShared:
		wnd->iw_auth_wpa_version = IW_AUTH_WPA_VERSION_DISABLED;
		wnd->iw_auth_80211_alg = IW_AUTH_ALG_SHARED_KEY;
		break;
	case Ndis802_11AuthModeAutoSwitch:
		wnd->iw_auth_wpa_version = IW_AUTH_WPA_VERSION_DISABLED;
		wnd->iw_auth_80211_alg = IW_AUTH_ALG_SHARED_KEY;
		wnd->iw_auth_80211_alg |= IW_AUTH_ALG_OPEN_SYSTEM;
		break;
	default:
		WARNING("invalid authentication algorithm: %d", auth_mode);
		break;
	}
	EXIT2(return 0);
}

int get_ndis_auth_mode(struct ndis_device *wnd)
{
	ULONG mode;
	NDIS_STATUS res;

	res = mp_query_int(wnd, OID_802_11_AUTHENTICATION_MODE, &mode);
	if (res) {
		WARNING("getting authentication mode failed (%08X)", res);
		EXIT2(return -EOPNOTSUPP);
	}
	TRACE2("%d", mode);
	return mode;
}

int set_iw_encr_mode(struct ndis_device *wnd, int cipher_pairwise,
		     int cipher_groupwise)
{
	NDIS_STATUS res;
	ULONG ndis_mode;

	ENTER2("%d, %d", cipher_pairwise, cipher_groupwise);
	if (cipher_pairwise & IW_AUTH_CIPHER_CCMP)
		ndis_mode = Ndis802_11Encryption3Enabled;
	else if (cipher_pairwise & IW_AUTH_CIPHER_TKIP)
		ndis_mode = Ndis802_11Encryption2Enabled;
	else if (cipher_pairwise &
		 (IW_AUTH_CIPHER_WEP40 | IW_AUTH_CIPHER_WEP104))
		ndis_mode = Ndis802_11Encryption1Enabled;
	else if (cipher_groupwise & IW_AUTH_CIPHER_CCMP)
		ndis_mode = Ndis802_11Encryption3Enabled;
	else if (cipher_groupwise & IW_AUTH_CIPHER_TKIP)
		ndis_mode = Ndis802_11Encryption2Enabled;
	else
		ndis_mode = Ndis802_11EncryptionDisabled;

	res = mp_set_int(wnd, OID_802_11_ENCRYPTION_STATUS, ndis_mode);
	if (res) {
		WARNING("setting encryption mode to %u failed (%08X)",
			ndis_mode, res);
		if (res == NDIS_STATUS_INVALID_DATA)
			EXIT2(return -EINVAL);
		return -EOPNOTSUPP;
	}
	wnd->iw_auth_cipher_pairwise = cipher_pairwise;
	wnd->iw_auth_cipher_group = cipher_groupwise;
	EXIT2(return 0);
}

int get_ndis_encr_mode(struct ndis_device *wnd)
{
	ULONG mode;
	NDIS_STATUS res;

	ENTER2("");
	res = mp_query_int(wnd, OID_802_11_ENCRYPTION_STATUS, &mode);
	if (res) {
		WARNING("getting encryption status failed (%08X)", res);
		EXIT2(return -EOPNOTSUPP);
	} else
		EXIT2(return mode);
}

static int iw_get_encr(struct net_device *dev, struct iw_request_info *info,
		       union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	int index, mode;
	struct encr_info *encr_info = &wnd->encr_info;

	ENTER2("wnd = %p", wnd);
	wrqu->data.length = 0;
	extra[0] = 0;

	index = (wrqu->encoding.flags & IW_ENCODE_INDEX);
	TRACE2("index = %u", index);
	if (index > 0)
		index--;
	else
		index = encr_info->tx_key_index;

	if (index < 0 || index >= MAX_ENCR_KEYS) {
		WARNING("encryption index out of range (%u)", index);
		EXIT2(return -EINVAL);
	}

	if (index != encr_info->tx_key_index) {
		if (encr_info->keys[index].length > 0) {
			wrqu->data.flags |= IW_ENCODE_ENABLED;
			wrqu->data.length = encr_info->keys[index].length;
			memcpy(extra, encr_info->keys[index].key,
			       encr_info->keys[index].length);
		}
		else
			wrqu->data.flags |= IW_ENCODE_DISABLED;

		EXIT2(return 0);
	}

	/* transmit key */
	mode = get_ndis_encr_mode(wnd);
	if (mode < 0)
		EXIT2(return -EOPNOTSUPP);

	if (mode == Ndis802_11EncryptionDisabled ||
	    mode == Ndis802_11EncryptionNotSupported)
		wrqu->data.flags |= IW_ENCODE_DISABLED;
	else {
		if (mode == Ndis802_11Encryption1KeyAbsent ||
		    mode == Ndis802_11Encryption2KeyAbsent ||
		    mode == Ndis802_11Encryption3KeyAbsent)
			wrqu->data.flags |= IW_ENCODE_NOKEY;
		else {
			wrqu->data.flags |= IW_ENCODE_ENABLED;
			wrqu->encoding.flags |= index+1;
			wrqu->data.length = encr_info->keys[index].length;
			memcpy(extra, encr_info->keys[index].key,
			       encr_info->keys[index].length);
		}
	}
	mode = get_ndis_auth_mode(wnd);
	if (mode < 0)
		EXIT2(return -EOPNOTSUPP);

	if (mode == Ndis802_11AuthModeOpen)
		wrqu->data.flags |= IW_ENCODE_OPEN;
	else if (mode == Ndis802_11AuthModeAutoSwitch)
		wrqu->data.flags |= IW_ENCODE_RESTRICTED;
	else // Ndis802_11AuthModeAutoSwitch, Ndis802_11AuthModeWPA etc.
		wrqu->data.flags |= IW_ENCODE_RESTRICTED;

	EXIT2(return 0);
}

/* remove_key is for both wep and wpa */
static int remove_key(struct ndis_device *wnd, int index,
		      mac_address bssid)
{
	NDIS_STATUS res;
	if (wnd->encr_info.keys[index].length == 0)
		EXIT2(return 0);
	wnd->encr_info.keys[index].length = 0;
	memset(&wnd->encr_info.keys[index].key, 0,
	       sizeof(wnd->encr_info.keys[index].length));
	if (wnd->iw_auth_cipher_pairwise == IW_AUTH_CIPHER_TKIP ||
	    wnd->iw_auth_cipher_pairwise == IW_AUTH_CIPHER_CCMP ||
	    wnd->iw_auth_cipher_group == IW_AUTH_CIPHER_TKIP ||
	    wnd->iw_auth_cipher_group == IW_AUTH_CIPHER_CCMP) {
		struct ndis_remove_key rmkey;
		rmkey.struct_size = sizeof(rmkey);
		rmkey.index = index;
		if (bssid) {
			/* pairwise key */
			if (memcmp(bssid, "\xff\xff\xff\xff\xff\xff",
				   ETH_ALEN) != 0)
				rmkey.index |= (1 << 30);
			memcpy(rmkey.bssid, bssid, sizeof(rmkey.bssid));
		} else
			memset(rmkey.bssid, 0xff, sizeof(rmkey.bssid));
		if (mp_set(wnd, OID_802_11_REMOVE_KEY, &rmkey, sizeof(rmkey)))
			EXIT2(return -EINVAL);
	} else {
		ndis_key_index keyindex = index;
		res = mp_set_int(wnd, OID_802_11_REMOVE_WEP, keyindex);
		if (res) {
			WARNING("removing encryption key %d failed (%08X)",
				keyindex, res);
			EXIT2(return -EINVAL);
		}
	}
	/* if it is transmit key, disable encryption */
	if (index == wnd->encr_info.tx_key_index) {
		res = set_iw_encr_mode(wnd, IW_AUTH_CIPHER_NONE,
				       IW_AUTH_CIPHER_NONE);
		if (res)
			WARNING("changing encr status failed (%08X)", res);
	}
	TRACE2("key %d removed", index);
	EXIT2(return 0);
}

static int iw_set_wep(struct net_device *dev, struct iw_request_info *info,
		      union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	NDIS_STATUS res;
	unsigned int index, key_len;
	struct encr_info *encr_info = &wnd->encr_info;
	unsigned char *key;

	ENTER2("");
	index = (wrqu->encoding.flags & IW_ENCODE_INDEX);
	TRACE2("index = %u", index);

	/* iwconfig gives index as 1 - N */
	if (index > 0)
		index--;
	else
		index = encr_info->tx_key_index;

	if (index >= MAX_ENCR_KEYS) {
		WARNING("encryption index out of range (%u)", index);
		EXIT2(return -EINVAL);
	}

	/* remove key if disabled */
	if (wrqu->data.flags & IW_ENCODE_DISABLED) {
		if (remove_key(wnd, index, NULL))
			EXIT2(return -EINVAL);
		else
			EXIT2(return 0);
	}

	/* global encryption state (for all keys) */
	if (wrqu->data.flags & IW_ENCODE_OPEN)
		res = set_ndis_auth_mode(wnd, Ndis802_11AuthModeOpen);
	else // if (wrqu->data.flags & IW_ENCODE_RESTRICTED)
		res = set_ndis_auth_mode(wnd, Ndis802_11AuthModeShared);
	if (res) {
		WARNING("setting authentication mode failed (%08X)", res);
		EXIT2(return -EINVAL);
	}

	TRACE2("key length: %d", wrqu->data.length);

	if (wrqu->data.length > 0) {
		key_len = wrqu->data.length;
		key = extra;
	} else { // must be set as tx key
		if (encr_info->keys[index].length == 0) {
			WARNING("key %d is not set", index+1);
			EXIT2(return -EINVAL);
		}
		key_len = encr_info->keys[index].length;
		key = encr_info->keys[index].key;
		encr_info->tx_key_index = index;
	}

	if (add_wep_key(wnd, key, key_len, index))
		EXIT2(return -EINVAL);

	if (index == encr_info->tx_key_index) {
		/* if transmit key is at index other than 0, some
		 * drivers, at least Atheros and TI, want another
		 * (global) non-transmit key to be set; don't know why */
		if (index != 0) {
			int i;
			for (i = 0; i < MAX_ENCR_KEYS; i++)
				if (i != index &&
				    encr_info->keys[i].length != 0)
					break;
			if (i == MAX_ENCR_KEYS) {
				if (index == 0)
					i = index + 1;
				else
					i = index - 1;
				if (add_wep_key(wnd, key, key_len, i))
					WARNING("couldn't add broadcast key"
						" at %d", i);
			}
		}
		/* ndis drivers want essid to be set after setting encr */
		set_essid(wnd, wnd->essid.essid, wnd->essid.length);
	}
	EXIT2(return 0);
}

static int iw_set_nick(struct net_device *dev, struct iw_request_info *info,
		       union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);

	if (wrqu->data.length >= sizeof(wnd->nick))
		return -EINVAL;
	memcpy(wnd->nick, extra, wrqu->data.length);
	wnd->nick[wrqu->data.length] = 0;
	return 0;
}

static int iw_get_nick(struct net_device *dev, struct iw_request_info *info,
		       union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);

	wrqu->data.length = strlen(wnd->nick);
	memcpy(extra, wnd->nick, wrqu->data.length);
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 27) && !defined(IW_REQUEST_FLAG_COMPAT)
#define	iwe_stream_add_event(a, b, c, d, e)	iwe_stream_add_event(b, c, d, e)
#define	iwe_stream_add_point(a, b, c, d, e)	iwe_stream_add_point(b, c, d, e)
#define	iwe_stream_add_value(a, b, c, d, e, f)	\
	iwe_stream_add_value(b, c, d, e, f)
#define	iwe_stream_lcp_len(a)			IW_EV_LCP_LEN
#endif

static char *ndis_translate_scan(struct net_device *dev,
				 struct iw_request_info *info, char *event,
				 char *end_buf, void *item)
{
	struct iw_event iwe;
	char *current_val;
	char *ret;
	int i, nrates;
	unsigned char custom_str[64];
	struct ndis_wlan_bssid *bssid;
	struct ndis_wlan_bssid_ex *bssid_ex;
	int extended;

	ENTER2("%p, %p", event, item);
	bssid = item;
	bssid_ex = item;
	extended = (bssid->length > offsetof(struct ndis_wlan_bssid_ex, var));

	/* add mac address */
	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWAP;
	iwe.u.ap_addr.sa_family = ARPHRD_ETHER;
	iwe.len = IW_EV_ADDR_LEN;
	memcpy(iwe.u.ap_addr.sa_data, bssid->mac, ETH_ALEN);
	ret = iwe_stream_add_event(info, event, end_buf, &iwe, IW_EV_ADDR_LEN);
	if (ret == event)
		return NULL;
	event = ret;

	/* add essid */
	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWESSID;
	iwe.u.data.length = bssid->ssid.length;
	if (iwe.u.data.length > IW_ESSID_MAX_SIZE)
		iwe.u.data.length = IW_ESSID_MAX_SIZE;
	iwe.u.data.flags = 1;
	iwe.len = IW_EV_POINT_LEN + iwe.u.data.length;
	ret = iwe_stream_add_point(info, event, end_buf, &iwe,
				   bssid->ssid.essid);
	if (ret == event)
		return NULL;
	event = ret;

	/* add protocol name */
	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWNAME;
	strncpy(iwe.u.name, network_type_to_name(bssid->net_type), IFNAMSIZ - 1);
	ret = iwe_stream_add_event(info, event, end_buf, &iwe, IW_EV_CHAR_LEN);
	if (ret == event)
		return NULL;
	event = ret;

	/* add mode */
	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWMODE;
	if (bssid->mode == Ndis802_11IBSS)
		iwe.u.mode = IW_MODE_ADHOC;
	else if (bssid->mode == Ndis802_11Infrastructure)
		iwe.u.mode = IW_MODE_MASTER;
	else // if (bssid->mode == Ndis802_11AutoUnknown)
		iwe.u.mode = IW_MODE_AUTO;
	ret = iwe_stream_add_event(info, event, end_buf, &iwe, IW_EV_UINT_LEN);
	if (ret == event)
		return NULL;
	event = ret;

	/* add freq */
	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWFREQ;
	iwe.u.freq.m = bssid->config.ds_config;
	if (bssid->config.ds_config > 1000000) {
		iwe.u.freq.m = bssid->config.ds_config / 10;
		iwe.u.freq.e = 1;
	}
	else
		iwe.u.freq.m = bssid->config.ds_config;
	/* convert from kHz to Hz */
	iwe.u.freq.e += 3;
	iwe.len = IW_EV_FREQ_LEN;
	ret = iwe_stream_add_event(info, event, end_buf, &iwe, IW_EV_FREQ_LEN);
	if (ret == event)
		return NULL;
	event = ret;

	/* add qual */
	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = IWEVQUAL;
	i = 100 * (bssid->rssi - WL_NOISE) / (WL_SIGMAX - WL_NOISE);
	if (i < 0)
		i = 0;
	else if (i > 100)
		i = 100;
	iwe.u.qual.level = bssid->rssi;
	iwe.u.qual.noise = WL_NOISE;
	iwe.u.qual.qual = i;
	iwe.len = IW_EV_QUAL_LEN;
	ret = iwe_stream_add_event(info, event, end_buf, &iwe, IW_EV_QUAL_LEN);
	if (ret == event)
		return NULL;
	event = ret;

	/* add key info */
	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = SIOCGIWENCODE;
	if (bssid->privacy == Ndis802_11PrivFilterAcceptAll)
		iwe.u.data.flags = IW_ENCODE_DISABLED;
	else
		iwe.u.data.flags = IW_ENCODE_ENABLED | IW_ENCODE_NOKEY;
	iwe.u.data.length = 0;
	iwe.len = IW_EV_POINT_LEN;
	ret = iwe_stream_add_point(info, event, end_buf, &iwe,
				   bssid->ssid.essid);
	if (ret == event)
		return NULL;
	event = ret;

	/* add rate */
	memset(&iwe, 0, sizeof(iwe));
	current_val = event + iwe_stream_lcp_len(info);
	iwe.cmd = SIOCGIWRATE;
	if (extended)
		nrates = ARRAY_SIZE(bssid->rates);
	else
		nrates = ARRAY_SIZE(bssid_ex->rates_ex);
	for (i = 0; i < nrates; i++) {
		if (bssid_ex->rates_ex[i] & 0x7f) {
			iwe.u.bitrate.value = ((bssid->rates[i] & 0x7f) *
					       500000);
			ret = iwe_stream_add_value(info, event, current_val,
						   end_buf, &iwe,
						   IW_EV_PARAM_LEN);
			if (ret == current_val)
				return NULL;
			current_val = ret;
		}
	}

	if ((current_val - event) > iwe_stream_lcp_len(info))
		event = current_val;

	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = IWEVCUSTOM;
	sprintf(custom_str, "bcn_int=%d", bssid->config.beacon_period);
	iwe.u.data.length = strlen(custom_str);
	ret = iwe_stream_add_point(info, event, end_buf, &iwe, custom_str);
	if (ret == event)
		return NULL;
	event = ret;

	memset(&iwe, 0, sizeof(iwe));
	iwe.cmd = IWEVCUSTOM;
	sprintf(custom_str, "atim=%u", bssid->config.atim_window);
	iwe.u.data.length = strlen(custom_str);
	ret = iwe_stream_add_point(info, event, end_buf, &iwe, custom_str);
	if (ret == event)
		return NULL;
	event = ret;

	TRACE2("%d, %zu", bssid->length, sizeof(*bssid));
	if (extended) {
		struct ndis_variable_ies *iep = bssid_ex->var;
		unsigned char *end = (unsigned char *)&bssid_ex->fixed +
			bssid_ex->ie_length;

		while (&iep->length < end && &iep->data[iep->length] <= end) {
			unsigned char ielen = iep->length + 2;

			memset(&iwe, 0, sizeof(iwe));
			iwe.cmd = IWEVGENIE;
			iwe.u.data.length = ielen;
			ret = iwe_stream_add_point(info, event, end_buf, &iwe,
						   (char *)iep);
			if (ret == event)
				return NULL;
			event = ret;
			iep = (typeof(iep))&iep->data[iep->length];
		}
	}
	TRACE2("event = %p, current_val = %p", event, current_val);
	EXIT2(return event);
}

static int set_scan(struct ndis_device *wnd)
{
	NDIS_STATUS res;

	ENTER2("");
	res = mp_set(wnd, OID_802_11_BSSID_LIST_SCAN, NULL, 0);
	if (res) {
		WARNING("scanning failed (%08X)", res);
		EXIT2(return -EOPNOTSUPP);
	}
	wnd->scan_timestamp = jiffies;
	EXIT2(return 0);
}

static int iw_set_scan(struct net_device *dev, struct iw_request_info *info,
		       union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	return set_scan(wnd);
}

static int iw_get_scan(struct net_device *dev, struct iw_request_info *info,
		       union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	unsigned int i, buf_len, needed, data_len;
	NDIS_STATUS res;
	struct ndis_bssid_list *bssid_list = NULL;
	char *event = extra;
	struct ndis_wlan_bssid *cur_item;

	ENTER2("");
	if (time_before(jiffies, wnd->scan_timestamp + 3 * HZ))
		return -EAGAIN;
	/* try with space for a few scan items */
	buf_len = sizeof(ULONG) + offsetof(struct ndis_wlan_bssid_ex, var) * 8;

	/* Try many times, as the needed space may grow between queries */
	for (i = 0; i < 10; i++) {
		bssid_list = kzalloc(buf_len, GFP_KERNEL);
		if (!bssid_list) {
			ERROR("couldn't allocate %u bytes for scan results",
			      buf_len);
			return -ENOMEM;
		}

		needed = 0;
		data_len = 0;
		res = mp_query_info(wnd, OID_802_11_BSSID_LIST, bssid_list,
				    buf_len, &data_len, &needed);
		TRACE2("try %d: given %d bytes, needed %d, written %d",
		       i, buf_len, needed, data_len);
		if (needed <= buf_len)
			break;
		kfree(bssid_list);
		buf_len = needed;
	}
	if (res) {
		WARNING("getting BSSID list failed (%08X)", res);
		kfree(bssid_list);
		EXIT2(return -EOPNOTSUPP);
	}

	/* some drivers don't set bssid_list->num_items to 0 if
	   OID_802_11_BSSID_LIST returns no items (prism54 driver, e.g.,) */
	TRACE2("items: %d", bssid_list->num_items);
	cur_item = &bssid_list->bssid[0];
	for (i = 0; i < bssid_list->num_items; i++) {
		TRACE2("item %d: len %d, remaining data %d",
		       i, cur_item->length, data_len);
		/* drop truncated items */
		if (cur_item->length > data_len)
			break;
		event = ndis_translate_scan(dev, info, event,
					    extra + wrqu->data.length,
					    cur_item);
		if (!event) {
			kfree(bssid_list);
			return -E2BIG;
		}
		data_len -= cur_item->length;
		cur_item = (struct ndis_wlan_bssid *)((char *)cur_item +
						      cur_item->length);
	}
	wrqu->data.length = event - extra;
	wrqu->data.flags = 0;
	kfree(bssid_list);
	EXIT2(return 0);
}

static int iw_set_power_mode(struct net_device *dev,
			     struct iw_request_info *info,
			     union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	NDIS_STATUS res;
	enum ndis_power power_mode;

	if (wrqu->power.disabled == 1)
		power_mode = NDIS_POWER_OFF;
	else if (wrqu->power.flags & IW_POWER_MIN)
		power_mode = NDIS_POWER_MIN;
	else // if (wrqu->power.flags & IW_POWER_MAX)
		power_mode = NDIS_POWER_MAX;

	TRACE2("%d", power_mode);
	res = mp_set(wnd, OID_802_11_POWER_MODE,
		     &power_mode, sizeof(power_mode));
	if (res)
		WARNING("setting power mode failed (%08X)", res);
	return 0;
}

static int iw_get_power_mode(struct net_device *dev,
			     struct iw_request_info *info,
			     union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	NDIS_STATUS res;
	enum ndis_power power_mode;

	ENTER2("");
	res = mp_query(wnd, OID_802_11_POWER_MODE,
		       &power_mode, sizeof(power_mode));
	if (res)
		return -ENOTSUPP;

	if (power_mode == NDIS_POWER_OFF)
		wrqu->power.disabled = 1;
	else {
		if (wrqu->power.flags != 0)
			return 0;
		wrqu->power.flags |= IW_POWER_ALL_R;
		wrqu->power.flags |= IW_POWER_TIMEOUT;
		wrqu->power.value = 0;
		wrqu->power.disabled = 0;

		if (power_mode == NDIS_POWER_MIN)
			wrqu->power.flags |= IW_POWER_MIN;
		else // if (power_mode == NDIS_POWER_MAX)
			wrqu->power.flags |= IW_POWER_MAX;
	}
	return 0;
}

static int iw_get_sensitivity(struct net_device *dev,
			      struct iw_request_info *info,
			      union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	NDIS_STATUS res;
	ndis_rssi rssi_trigger;

	ENTER2("");
	res = mp_query(wnd, OID_802_11_RSSI_TRIGGER,
		       &rssi_trigger, sizeof(rssi_trigger));
	if (res)
		return -EOPNOTSUPP;
	wrqu->param.value = rssi_trigger;
	wrqu->param.disabled = (rssi_trigger == 0);
	wrqu->param.fixed = 1;
	return 0;
}

static int iw_set_sensitivity(struct net_device *dev,
			      struct iw_request_info *info,
			      union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	NDIS_STATUS res;
	ndis_rssi rssi_trigger;

	ENTER2("");
	if (wrqu->param.disabled)
		rssi_trigger = 0;
	else
		rssi_trigger = wrqu->param.value;
	res = mp_set(wnd, OID_802_11_RSSI_TRIGGER,
		     &rssi_trigger, sizeof(rssi_trigger));
	if (res == NDIS_STATUS_INVALID_DATA)
		return -EINVAL;
	if (res)
		return -EOPNOTSUPP;
	return 0;
}

static int iw_get_ndis_stats(struct net_device *dev,
			     struct iw_request_info *info,
			     union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	struct iw_statistics *stats = &wnd->iw_stats;
	memcpy(&wrqu->qual, &stats->qual, sizeof(stats->qual));
	return 0;
}

static int iw_get_range(struct net_device *dev, struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	struct iw_range *range = (struct iw_range *)extra;
	struct iw_point *data = &wrqu->data;
	struct ndis_device *wnd = netdev_priv(dev);
	unsigned int i, n;
	NDIS_STATUS res;
	UCHAR rates[NDIS_MAX_RATES_EX];
	ndis_tx_power_level tx_power;

	ENTER2("");
	data->length = sizeof(struct iw_range);
	memset(range, 0, sizeof(struct iw_range));

	range->txpower_capa = IW_TXPOW_MWATT;
	range->num_txpower = 0;

	res = mp_query(wnd, OID_802_11_TX_POWER_LEVEL,
		       &tx_power, sizeof(tx_power));
	if (!res) {
		range->num_txpower = 1;
		range->txpower[0] = tx_power;
	}

	range->we_version_compiled = WIRELESS_EXT;
	range->we_version_source = 19;

	range->retry_capa = IW_RETRY_LIMIT;
	range->retry_flags = IW_RETRY_LIMIT;
	range->min_retry = 0;
	range->max_retry = 255;

	range->num_channels = 1;

	range->max_qual.qual = 100;
	range->max_qual.level = 154;
	range->max_qual.noise = 154;
	range->sensitivity = 3;

	range->max_encoding_tokens = 4;
	range->num_encoding_sizes = 2;
	range->encoding_size[0] = 5;
	range->encoding_size[1] = 13;

	range->num_bitrates = 0;
	memset(&rates, 0, sizeof(rates));
	res = mp_query_info(wnd, OID_802_11_SUPPORTED_RATES,
			    &rates, sizeof(rates), &n, NULL);
	if (res)
		WARNING("getting bit rates failed: %08X", res);
	else {
		for (i = 0; i < n && range->num_bitrates < IW_MAX_BITRATES; i++)
			if (rates[i] & 0x80)
				continue;
			else if (rates[i] & 0x7f) {
				range->bitrate[range->num_bitrates] =
					(rates[i] & 0x7f) * 500000;
				range->num_bitrates++;
			}
	}

	range->num_channels = ARRAY_SIZE(freq_chan);

	for (i = 0; i < ARRAY_SIZE(freq_chan) && i < IW_MAX_FREQUENCIES; i++) {
		range->freq[i].i = i + 1;
		range->freq[i].m = freq_chan[i] * 100000;
		range->freq[i].e = 1;
	}
	range->num_frequency = i;

	range->min_rts = 0;
	range->max_rts = 2347;
	range->min_frag = 256;
	range->max_frag = 2346;

	/* Event capability (kernel + driver) */
	range->event_capa[0] = (IW_EVENT_CAPA_K_0 |
				IW_EVENT_CAPA_MASK(SIOCGIWTHRSPY) |
				IW_EVENT_CAPA_MASK(SIOCGIWAP) |
				IW_EVENT_CAPA_MASK(SIOCGIWSCAN));
	range->event_capa[1] = IW_EVENT_CAPA_K_1;
	range->event_capa[4] = (IW_EVENT_CAPA_MASK(IWEVTXDROP) |
				IW_EVENT_CAPA_MASK(IWEVCUSTOM) |
				IW_EVENT_CAPA_MASK(IWEVREGISTERED) |
				IW_EVENT_CAPA_MASK(IWEVEXPIRED));

	range->enc_capa = 0;

	if (test_bit(Ndis802_11Encryption2Enabled, &wnd->capa.encr))
		range->enc_capa |= IW_ENC_CAPA_CIPHER_TKIP;
	if (test_bit(Ndis802_11Encryption3Enabled, &wnd->capa.encr))
		range->enc_capa |= IW_ENC_CAPA_CIPHER_CCMP;

	if (test_bit(Ndis802_11AuthModeWPA, &wnd->capa.auth) ||
	    test_bit(Ndis802_11AuthModeWPAPSK, &wnd->capa.auth))
		range->enc_capa |= IW_ENC_CAPA_WPA;
	if (test_bit(Ndis802_11AuthModeWPA2, &wnd->capa.auth) ||
	    test_bit(Ndis802_11AuthModeWPA2PSK, &wnd->capa.auth))
		range->enc_capa |= IW_ENC_CAPA_WPA2;

	return 0;
}

void set_default_iw_params(struct ndis_device *wnd)
{
	wnd->iw_auth_key_mgmt = 0;
	wnd->iw_auth_wpa_version = 0;
	set_infra_mode(wnd, Ndis802_11Infrastructure);
	set_ndis_auth_mode(wnd, Ndis802_11AuthModeOpen);
	set_priv_filter(wnd);
	set_iw_encr_mode(wnd, IW_AUTH_CIPHER_NONE, IW_AUTH_CIPHER_NONE);
}

static int deauthenticate(struct ndis_device *wnd)
{
	int ret;

	ENTER2("");
	ret = disassociate(wnd, 1);
	set_default_iw_params(wnd);
	EXIT2(return ret);
}

NDIS_STATUS disassociate(struct ndis_device *wnd, int reset_ssid)
{
	NDIS_STATUS res;
	u8 buf[NDIS_ESSID_MAX_SIZE];
	int i;

	TRACE2("");
	res = mp_set(wnd, OID_802_11_DISASSOCIATE, NULL, 0);
	/* disassociate causes radio to be turned off; if reset_ssid
	 * is given, set ssid to random to enable radio */
	if (reset_ssid) {
		get_random_bytes(buf, sizeof(buf));
		for (i = 0; i < sizeof(buf); i++)
			buf[i] = 'a' + (buf[i] % 26);
		set_essid(wnd, buf, sizeof(buf));
	}
	return res;
}

static int iw_set_mlme(struct net_device *dev, struct iw_request_info *info,
		       union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	struct iw_mlme *mlme = (struct iw_mlme *)extra;

	ENTER2("");
	switch (mlme->cmd) {
	case IW_MLME_DEAUTH:
		return deauthenticate(wnd);
	case IW_MLME_DISASSOC:
		TRACE2("cmd=%d reason_code=%d", mlme->cmd, mlme->reason_code);
		return disassociate(wnd, 1);
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int iw_set_genie(struct net_device *dev,
			struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	/*
	 * NDIS drivers do not allow IEs to be configured; this is
	 * done by the driver based on other configuration. Return 0
	 * to avoid causing issues with user space programs that
	 * expect this function to succeed.
	 */
	return 0;
}

static int iw_set_auth(struct net_device *dev,
		       struct iw_request_info *info,
		       union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	TRACE2("index=%d value=%d", wrqu->param.flags & IW_AUTH_INDEX,
	       wrqu->param.value);
	switch (wrqu->param.flags & IW_AUTH_INDEX) {
	case IW_AUTH_WPA_VERSION:
		wnd->iw_auth_wpa_version = wrqu->param.value;
		break;
	case IW_AUTH_CIPHER_PAIRWISE:
		wnd->iw_auth_cipher_pairwise = wrqu->param.value;
		break;
	case IW_AUTH_CIPHER_GROUP:
		wnd->iw_auth_cipher_group = wrqu->param.value;
		break;
	case IW_AUTH_KEY_MGMT:
		wnd->iw_auth_key_mgmt = wrqu->param.value;
		break;
	case IW_AUTH_80211_AUTH_ALG:
		wnd->iw_auth_80211_alg = wrqu->param.value;
		break;
	case IW_AUTH_WPA_ENABLED:
		if (wrqu->param.value)
			deauthenticate(wnd);
		break;
#ifdef IW_AUTH_MFP
	case IW_AUTH_MFP:
		if (wrqu->param.value == IW_AUTH_MFP_DISABLED ||
		    wrqu->param.value == IW_AUTH_MFP_OPTIONAL)
			break;
		WARNING("MFP not implemented");
		return -EOPNOTSUPP;
#endif
	case IW_AUTH_TKIP_COUNTERMEASURES:
	case IW_AUTH_DROP_UNENCRYPTED:
	case IW_AUTH_RX_UNENCRYPTED_EAPOL:
	case IW_AUTH_PRIVACY_INVOKED:
		TRACE2("%d not implemented: %d",
		       wrqu->param.flags & IW_AUTH_INDEX, wrqu->param.value);
		break;
	default:
		WARNING("invalid cmd %d", wrqu->param.flags & IW_AUTH_INDEX);
		return -EOPNOTSUPP;
	}
	return 0;
}

static int iw_get_auth(struct net_device *dev,
		       struct iw_request_info *info,
		       union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);

	ENTER2("index=%d", wrqu->param.flags & IW_AUTH_INDEX);
	switch (wrqu->param.flags & IW_AUTH_INDEX) {
	case IW_AUTH_WPA_VERSION:
		wrqu->param.value = wnd->iw_auth_wpa_version;
		break;
	case IW_AUTH_CIPHER_PAIRWISE:
		wrqu->param.value = wnd->iw_auth_cipher_pairwise;
		break;
	case IW_AUTH_CIPHER_GROUP:
		wrqu->param.value = wnd->iw_auth_cipher_group;
		break;
	case IW_AUTH_KEY_MGMT:
		wrqu->param.value = wnd->iw_auth_key_mgmt;
		break;
	case IW_AUTH_80211_AUTH_ALG:
		wrqu->param.value = wnd->iw_auth_80211_alg;
		break;
	default:
		WARNING("invalid cmd %d", wrqu->param.flags & IW_AUTH_INDEX);
		return -EOPNOTSUPP;
	}
	return 0;
}

static int iw_set_encodeext(struct net_device *dev,
			    struct iw_request_info *info,
			    union iwreq_data *wrqu, char *extra)
{
	struct iw_encode_ext *ext = (struct iw_encode_ext *)extra;
	struct ndis_device *wnd = netdev_priv(dev);
	struct ndis_add_key ndis_key;
	int i, keyidx;
	NDIS_STATUS res;
	u8 *addr;

	keyidx = wrqu->encoding.flags & IW_ENCODE_INDEX;
	ENTER2("%d", keyidx);
	if (keyidx)
		keyidx--;
	else
		keyidx = wnd->encr_info.tx_key_index;

	if (keyidx < 0 || keyidx >= MAX_ENCR_KEYS)
		return -EINVAL;

	if (ext->alg == WPA_ALG_WEP) {
		if (!test_bit(Ndis802_11Encryption1Enabled, &wnd->capa.encr))
			EXIT2(return -1);
		if (ext->ext_flags & IW_ENCODE_EXT_SET_TX_KEY)
			wnd->encr_info.tx_key_index = keyidx;
		if (add_wep_key(wnd, ext->key, ext->key_len, keyidx))
			EXIT2(return -1);
		else
			EXIT2(return 0);
	}
	if ((wrqu->encoding.flags & IW_ENCODE_DISABLED) ||
	    ext->alg == IW_ENCODE_ALG_NONE || ext->key_len == 0)
		EXIT2(return remove_key(wnd, keyidx, ndis_key.bssid));

	if (ext->key_len > sizeof(ndis_key.key)) {
		TRACE2("incorrect key length (%u)", ext->key_len);
		EXIT2(return -1);
	}

	memset(&ndis_key, 0, sizeof(ndis_key));

	ndis_key.struct_size =
		sizeof(ndis_key) - sizeof(ndis_key.key) + ext->key_len;
	ndis_key.length = ext->key_len;
	ndis_key.index = keyidx;

	if (ext->ext_flags & IW_ENCODE_EXT_RX_SEQ_VALID) {
		for (i = 0; i < 6; i++)
			ndis_key.rsc |= (((u64)ext->rx_seq[i]) << (i * 8));
		TRACE2("0x%llx", ndis_key.rsc);
		ndis_key.index |= 1 << 29;
	}

	addr = ext->addr.sa_data;
	if (ext->ext_flags & IW_ENCODE_EXT_GROUP_KEY) {
		/* group key */
		if (wnd->infrastructure_mode == Ndis802_11IBSS)
			memset(ndis_key.bssid, 0xff, ETH_ALEN);
		else
			get_ap_address(wnd, ndis_key.bssid);
	} else {
		/* pairwise key */
		ndis_key.index |= (1 << 30);
		memcpy(ndis_key.bssid, addr, ETH_ALEN);
	}
	TRACE2(MACSTRSEP, MAC2STR(ndis_key.bssid));

	if (ext->ext_flags & IW_ENCODE_EXT_SET_TX_KEY)
		ndis_key.index |= (1 << 31);

	if (ext->alg == IW_ENCODE_ALG_TKIP && ext->key_len == 32) {
		/* wpa_supplicant gives us the Michael MIC RX/TX keys in
		 * different order than NDIS spec, so swap the order here. */
		memcpy(ndis_key.key, ext->key, 16);
		memcpy(ndis_key.key + 16, ext->key + 24, 8);
		memcpy(ndis_key.key + 24, ext->key + 16, 8);
	} else
		memcpy(ndis_key.key, ext->key, ext->key_len);

	res = mp_set(wnd, OID_802_11_ADD_KEY, &ndis_key, ndis_key.struct_size);
	if (res) {
		TRACE2("adding key failed (%08X), %u",
		       res, ndis_key.struct_size);
		EXIT2(return -1);
	}
	wnd->encr_info.keys[keyidx].length = ext->key_len;
	memcpy(&wnd->encr_info.keys[keyidx].key, ndis_key.key, ext->key_len);
	if (ext->ext_flags & IW_ENCODE_EXT_SET_TX_KEY)
		wnd->encr_info.tx_key_index = keyidx;
	TRACE2("key %d added", keyidx);

	EXIT2(return 0);
}

static int iw_get_encodeext(struct net_device *dev,
			    struct iw_request_info *info,
			    union iwreq_data *wrqu, char *extra)
{
	/* struct iw_encode_ext *ext = (struct iw_encode_ext *) extra; */
	/* TODO */
	ENTER2("");
	return 0;
}

static int iw_set_pmksa(struct net_device *dev, struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	struct iw_pmksa *pmksa = (struct iw_pmksa *)extra;
	struct ndis_pmkid pmkid;
	NDIS_STATUS res;
	struct ndis_device *wnd = netdev_priv(dev);

	/* TODO: must keep local list of PMKIDs since NDIS drivers
	 * expect that all PMKID entries are included whenever a new
	 * one is added. */

	ENTER2("%d", pmksa->cmd);
	if ((pmksa->cmd == IW_PMKSA_ADD || pmksa->cmd == IW_PMKSA_REMOVE) &&
	    (!(wnd->iw_auth_wpa_version & IW_AUTH_WPA_VERSION_WPA2)))
		EXIT2(return -EOPNOTSUPP);

	memset(&pmkid, 0, sizeof(pmkid));
	if (pmksa->cmd == IW_PMKSA_ADD) {
		pmkid.bssid_info_count = 1;
		memcpy(pmkid.bssid_info[0].bssid, pmksa->bssid.sa_data,
		       ETH_ALEN);
		memcpy(pmkid.bssid_info[0].pmkid, pmksa->pmkid, IW_PMKID_LEN);
	}
	pmkid.length = sizeof(pmkid);

	res = mp_set(wnd, OID_802_11_PMKID, &pmkid, pmkid.length);
	if (res == NDIS_STATUS_FAILURE)
		EXIT2(return -EOPNOTSUPP);
	TRACE2("OID_802_11_PMKID -> %d", res);
	if (res)
		return -EINVAL;

	return 0;
}

#define WEXT(id) [id - SIOCIWFIRST]

static const iw_handler	ndis_handler[] = {
	WEXT(SIOCGIWNAME)	= iw_get_network_type,
	WEXT(SIOCSIWESSID)	= iw_set_essid,
	WEXT(SIOCGIWESSID)	= iw_get_essid,
	WEXT(SIOCSIWMODE)	= iw_set_infra_mode,
	WEXT(SIOCGIWMODE)	= iw_get_infra_mode,
	WEXT(SIOCGIWFREQ)	= iw_get_freq,
	WEXT(SIOCSIWFREQ)	= iw_set_freq,
	WEXT(SIOCGIWTXPOW)	= iw_get_tx_power,
	WEXT(SIOCSIWTXPOW)	= iw_set_tx_power,
	WEXT(SIOCGIWRATE)	= iw_get_bitrate,
	WEXT(SIOCSIWRATE)	= iw_set_bitrate,
	WEXT(SIOCGIWRTS)	= iw_get_rts_threshold,
	WEXT(SIOCSIWRTS)	= iw_set_rts_threshold,
	WEXT(SIOCGIWFRAG)	= iw_get_frag_threshold,
	WEXT(SIOCSIWFRAG)	= iw_set_frag_threshold,
	WEXT(SIOCGIWAP)		= iw_get_ap_address,
	WEXT(SIOCSIWAP)		= iw_set_ap_address,
	WEXT(SIOCSIWENCODE)	= iw_set_wep,
	WEXT(SIOCGIWENCODE)	= iw_get_encr,
	WEXT(SIOCSIWSCAN)	= iw_set_scan,
	WEXT(SIOCGIWSCAN)	= iw_get_scan,
	WEXT(SIOCGIWPOWER)	= iw_get_power_mode,
	WEXT(SIOCSIWPOWER)	= iw_set_power_mode,
	WEXT(SIOCGIWRANGE)	= iw_get_range,
	WEXT(SIOCGIWSTATS)	= iw_get_ndis_stats,
	WEXT(SIOCGIWSENS)	= iw_get_sensitivity,
	WEXT(SIOCSIWSENS)	= iw_set_sensitivity,
	WEXT(SIOCGIWNICKN)	= iw_get_nick,
	WEXT(SIOCSIWNICKN)	= iw_set_nick,
	WEXT(SIOCSIWCOMMIT)	= iw_set_dummy,
	WEXT(SIOCSIWMLME)	= iw_set_mlme,
	WEXT(SIOCSIWGENIE)	= iw_set_genie,
	WEXT(SIOCSIWAUTH)	= iw_set_auth,
	WEXT(SIOCGIWAUTH)	= iw_get_auth,
	WEXT(SIOCSIWENCODEEXT)	= iw_set_encodeext,
	WEXT(SIOCGIWENCODEEXT)	= iw_get_encodeext,
	WEXT(SIOCSIWPMKSA)	= iw_set_pmksa,
};

/* private ioctl's */

static int priv_reset(struct net_device *dev, struct iw_request_info *info,
		      union iwreq_data *wrqu, char *extra)
{
	int res;
	ENTER2("");
	res = mp_reset(netdev_priv(dev));
	if (res) {
		WARNING("reset failed: %08X", res);
		return -EOPNOTSUPP;
	}
	return 0;
}

static int priv_deauthenticate(struct net_device *dev,
			       struct iw_request_info *info,
			       union iwreq_data *wrqu, char *extra)
{
	int res;
	ENTER2("");
	res = deauthenticate(netdev_priv(dev));
	return res;
}

static int priv_power_profile(struct net_device *dev,
			      struct iw_request_info *info,
			      union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	struct miniport *mp;
	ULONG profile_inf;

	ENTER2("");
	mp = &wnd->wd->driver->ndis_driver->mp;
	if (!mp->pnp_event_notify)
		EXIT2(return -EOPNOTSUPP);

	/* 1 for AC and 0 for Battery */
	if (wrqu->param.value)
		profile_inf = NdisPowerProfileAcOnLine;
	else
		profile_inf = NdisPowerProfileBattery;

	LIN2WIN4(mp->pnp_event_notify, wnd->nmb->mp_ctx,
		 NdisDevicePnPEventPowerProfileChanged,
		 &profile_inf, sizeof(profile_inf));
	EXIT2(return 0);
}

static int priv_network_type(struct net_device *dev,
			     struct iw_request_info *info,
			     union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	enum network_type network_type;
	NDIS_STATUS res;
	char type;

	ENTER2("");
	type = wrqu->param.value;
	if (type == 'f')
		network_type = Ndis802_11FH;
	else if (type == 'b')
		network_type = Ndis802_11DS;
	else if (type == 'a')
		network_type = Ndis802_11OFDM5;
	else if (type == 'g' || type == 'n')
		network_type = Ndis802_11OFDM24;
	else
		network_type = Ndis802_11Automode;

	res = mp_set_int(wnd, OID_802_11_NETWORK_TYPE_IN_USE, network_type);
	if (res) {
		WARNING("setting network type to %d failed (%08X)",
			network_type, res);
		EXIT2(return -EINVAL);
	}

	EXIT2(return 0);
}

static int priv_media_stream_mode(struct net_device *dev,
				  struct iw_request_info *info,
				  union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	NDIS_STATUS res;
	int mode;

	ENTER2("");
	if (wrqu->param.value > 0)
		mode = Ndis802_11MediaStreamOn;
	else
		mode = Ndis802_11MediaStreamOff;
	res = mp_set_int(wnd, OID_802_11_MEDIA_STREAM_MODE, mode);
	if (res) {
		WARNING("oid failed (%08X)", res);
		EXIT2(return -EINVAL);
	}
	EXIT2(return 0);
}

static int priv_reload_defaults(struct net_device *dev,
				struct iw_request_info *info,
				union iwreq_data *wrqu, char *extra)
{
	struct ndis_device *wnd = netdev_priv(dev);
	int res;
	ENTER2("");
	res = mp_set_int(wnd, OID_802_11_RELOAD_DEFAULTS,
			 Ndis802_11ReloadWEPKeys);
	if (res) {
		WARNING("reloading defaults failed: %08X", res);
		return -EOPNOTSUPP;
	}
	return 0;
}

static const struct iw_priv_args priv_args[] = {
	{PRIV_RESET, 0, 0, "ndis_reset"},
	{PRIV_POWER_PROFILE, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0,
	 "power_profile"},
	{PRIV_DEAUTHENTICATE, 0, 0, "deauthenticate"},
	{PRIV_NETWORK_TYPE, IW_PRIV_TYPE_CHAR | IW_PRIV_SIZE_FIXED | 1, 0,
	 "network_type"},
	{PRIV_MEDIA_STREAM_MODE, IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0,
	 "media_stream"},

	{PRIV_RELOAD_DEFAULTS, 0, 0, "reload_defaults"},
};

#define WEPRIV(id) [id - SIOCIWFIRSTPRIV]

static const iw_handler priv_handler[] = {
	WEPRIV(PRIV_RESET)		= priv_reset,
	WEPRIV(PRIV_POWER_PROFILE)	= priv_power_profile,
	WEPRIV(PRIV_DEAUTHENTICATE)	= priv_deauthenticate,
	WEPRIV(PRIV_NETWORK_TYPE)	= priv_network_type,
	WEPRIV(PRIV_MEDIA_STREAM_MODE)	= priv_media_stream_mode,
	WEPRIV(PRIV_RELOAD_DEFAULTS)	= priv_reload_defaults,
};

const struct iw_handler_def ndis_handler_def = {
	.num_standard	= ARRAY_SIZE(ndis_handler),
	.num_private	= ARRAY_SIZE(priv_handler),
	.num_private_args = ARRAY_SIZE(priv_args),

	.standard	= (iw_handler *)ndis_handler,
	.private	= (iw_handler *)priv_handler,
	.private_args	= (struct iw_priv_args *)priv_args,
	.get_wireless_stats = get_iw_stats,
};

#endif
