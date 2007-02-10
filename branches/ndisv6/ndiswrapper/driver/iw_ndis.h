/*
 *  Copyright (C) 2006-2007 Giridhar Pemmasani
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

#ifndef _IW_NDIS_H_
#define _IW_NDIS_H_

#include "ndis.h"

#define	WL_NOISE	-96	/* typical noise level in dBm */
#define	WL_SIGMAX	-32	/* typical maximum signal level in dBm */


int add_wep_key(struct wrap_ndis_device *wnd, char *key, int key_len,
		int index);
NDIS_STATUS set_essid(struct wrap_ndis_device *wnd, const char *ssid,
		      int ssid_len);
NDIS_STATUS set_infra_mode(struct wrap_ndis_device *wnd,
			   enum ndis_dot11_bss_type mode);
int get_ap_address(struct wrap_ndis_device *wnd, mac_address mac);
NDIS_STATUS set_auth_algo(struct wrap_ndis_device *wnd,
			  enum ndis_dot11_auth_algorithm algo_id);
NDIS_STATUS set_cipher_algo(struct wrap_ndis_device *wnd,
			    enum ndis_dot11_cipher_algorithm algo_id);
enum ndis_dot11_auth_algorithm get_auth_mode(struct wrap_ndis_device *wnd);
enum ndis_dot11_cipher_algorithm get_cipher_mode(struct wrap_ndis_device *wnd);
int set_priv_filter(struct wrap_ndis_device *wnd, int flags);
int set_scan(struct wrap_ndis_device *wnd);

#define PRIV_RESET	 		SIOCIWFIRSTPRIV+16
#define PRIV_POWER_PROFILE	 	SIOCIWFIRSTPRIV+17
#define PRIV_NETWORK_TYPE	 	SIOCIWFIRSTPRIV+18
#define PRIV_USB_RESET	 		SIOCIWFIRSTPRIV+19
#define PRIV_MEDIA_STREAM_MODE 		SIOCIWFIRSTPRIV+20
#define PRIV_SET_CIPHER_MODE		SIOCIWFIRSTPRIV+21
#define PRIV_SET_AUTH_MODE		SIOCIWFIRSTPRIV+22
#define PRIV_RELOAD_DEFAULTS		SIOCIWFIRSTPRIV+23

#define RSN_INFO_ELEM		0x30

/* these have to match what is in wpa_supplicant */

typedef enum { WPA_ALG_NONE, WPA_ALG_WEP, WPA_ALG_TKIP, WPA_ALG_CCMP } wpa_alg;
typedef enum { CIPHER_NONE, CIPHER_WEP40, CIPHER_TKIP, CIPHER_CCMP,
	       CIPHER_WEP104 } wpa_cipher;
typedef enum { KEY_MGMT_802_1X, KEY_MGMT_PSK, KEY_MGMT_NONE,
	       KEY_MGMT_802_1X_NO_WPA, KEY_MGMT_WPA_NONE } wpa_key_mgmt;

#if WIRELESS_EXT <= 17
/* WPA support through 'ndiswrapper' driver interface */

#define AUTH_ALG_OPEN_SYSTEM	0x01
#define AUTH_ALG_SHARED_KEY	0x02
#define AUTH_ALG_LEAP		0x04

#define IEEE80211_MODE_INFRA	0
#define IEEE80211_MODE_IBSS	1

struct wpa_key {
	wpa_alg alg;
	u8 *addr;
	int key_index;
	int set_tx;
	u8 *seq;
	size_t seq_len;
	u8 *key;
	size_t key_len;
};

struct wpa_assoc_info {
	const u8 *bssid;
	const u8 *ssid;
	size_t ssid_len;
	int freq;
	const u8 *wpa_ie;
	size_t wpa_ie_len;
	wpa_cipher pairwise_suite;
	wpa_cipher group_suite;
	wpa_key_mgmt key_mgmt_suite;
	int auth_alg;
	int mode;
};

struct wpa_driver_capa {
#define WPA_DRIVER_CAPA_KEY_MGMT_WPA        0x00000001
#define WPA_DRIVER_CAPA_KEY_MGMT_WPA2       0x00000002
#define WPA_DRIVER_CAPA_KEY_MGMT_WPA_PSK    0x00000004
#define WPA_DRIVER_CAPA_KEY_MGMT_WPA2_PSK   0x00000008
#define WPA_DRIVER_CAPA_KEY_MGMT_WPA_NONE   0x00000010
	unsigned int key_mgmt;

#define WPA_DRIVER_CAPA_ENC_WEP40   0x00000001
#define WPA_DRIVER_CAPA_ENC_WEP104  0x00000002
#define WPA_DRIVER_CAPA_ENC_TKIP    0x00000004
#define WPA_DRIVER_CAPA_ENC_CCMP    0x00000008
	unsigned int enc;

#define WPA_DRIVER_AUTH_OPEN        0x00000001
#define WPA_DRIVER_AUTH_SHARED      0x00000002
#define WPA_DRIVER_AUTH_LEAP        0x00000004
	unsigned int auth;

/* Driver generated WPA/RSN IE */
#define WPA_DRIVER_FLAGS_DRIVER_IE  0x00000001
#define WPA_DRIVER_FLAGS_SET_KEYS_AFTER_ASSOC 0x00000002
	unsigned int flags;
};

#define WPA_SET_WPA 			SIOCIWFIRSTPRIV+1
#define WPA_SET_KEY 			SIOCIWFIRSTPRIV+2
#define WPA_ASSOCIATE		 	SIOCIWFIRSTPRIV+3
#define WPA_DISASSOCIATE 		SIOCIWFIRSTPRIV+4
#define WPA_DROP_UNENCRYPTED 		SIOCIWFIRSTPRIV+5
#define WPA_SET_COUNTERMEASURES 	SIOCIWFIRSTPRIV+6
#define WPA_DEAUTHENTICATE	 	SIOCIWFIRSTPRIV+7
#define WPA_SET_AUTH_ALG	 	SIOCIWFIRSTPRIV+8
#define WPA_INIT			SIOCIWFIRSTPRIV+9
#define WPA_DEINIT			SIOCIWFIRSTPRIV+10
#define WPA_GET_CAPA			SIOCIWFIRSTPRIV+11

#endif

#endif // IW_NDIS_H