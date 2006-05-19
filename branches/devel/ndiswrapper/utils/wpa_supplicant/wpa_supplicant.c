/*
 * WPA Supplicant
 * Copyright (c) 2003-2004, Jouni Malinen <jkmaline@cc.hut.fi>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 * See README and COPYING for more details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <ctype.h>
#include <netinet/in.h>
#include <fcntl.h>

#include "common.h"
#include "wpa.h"
#include "driver.h"
#include "eloop.h"
#include "wpa_supplicant.h"
#include "config.h"
#include "l2_packet.h"
#include "eapol_sm.h"
#include "wpa_supplicant_i.h"
#include "ctrl_iface.h"

static const char *wpa_supplicant_version =
"wpa_supplicant v0.0 - Copyright (c) 2003-2004, Jouni Malinen "
"<jkmaline@cc.hut.fi>";

static const char *wpa_supplicant_license =
"This program is free software. You can distribute it and/or modify it\n"
"under the terms of the GNU General Public License version 2.\n"
"\n"
"Alternatively, this software may be distributed under the terms of the\n"
"BSD license. See README and COPYING for more details.\n";

static const char *wpa_supplicant_full_license =
"This program is free software; you can redistribute it and/or modify\n"
"it under the terms of the GNU General Public License version 2 as\n"
"published by the Free Software Foundation.\n"
"\n"
"This program is distributed in the hope that it will be useful,\n"
"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
"GNU General Public License for more details.\n"
"\n"
"You should have received a copy of the GNU General Public License\n"
"along with this program; if not, write to the Free Software\n"
"Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA\n"
"\n"
"Alternatively, this software may be distributed under the terms of the\n"
"BSD license.\n"
"\n"
"Redistribution and use in source and binary forms, with or without\n"
"modification, are permitted provided that the following conditions are\n"
"met:\n"
"\n"
"1. Redistributions of source code must retain the above copyright\n"
"   notice, this list of conditions and the following disclaimer.\n"
"\n"
"2. Redistributions in binary form must reproduce the above copyright\n"
"   notice, this list of conditions and the following disclaimer in the\n"
"   documentation and/or other materials provided with the distribution.\n"
"\n"
"3. Neither the name(s) of the above-listed copyright holder(s) nor the\n"
"   names of its contributors may be used to endorse or promote products\n"
"   derived from this software without specific prior written permission.\n"
"\n"
"THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"
"\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\n"
"LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\n"
"A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\n"
"OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\n"
"SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\n"
"LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\n"
"DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\n"
"THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
"(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\n"
"OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
"\n";


static void wpa_supplicant_scan_results(struct wpa_supplicant *wpa_s);


static int wpa_debug_level = MSG_INFO;

void wpa_printf(int level, char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (level >= wpa_debug_level) {
		vprintf(fmt, ap);
		printf("\n");
	}
	va_end(ap);
}


void wpa_hexdump(int level, const char *title, const u8 *buf, size_t len)
{
	size_t i;
	if (level < wpa_debug_level)
		return;
	printf("%s - hexdump(len=%d):", title, len);
	for (i = 0; i < len; i++)
		printf(" %02x", buf[i]);
	printf("\n");
}


void wpa_hexdump_ascii(int level, const char *title, const u8 *buf, size_t len)
{
	int i, llen;
	const u8 *pos = buf;
	const int line_len = 16;

	if (level < wpa_debug_level)
		return;
	printf("%s - hexdump_ascii(len=%d):\n", title, len);
	while (len) {
		llen = len > line_len ? line_len : len;
		printf("    ");
		for (i = 0; i < llen; i++)
			printf(" %02x", pos[i]);
		for (i = llen; i < line_len; i++)
			printf("   ");
		printf("   ");
		for (i = 0; i < llen; i++) {
			if (isprint(pos[i]))
				printf("%c", pos[i]);
			else
				printf("_");
		}
		for (i = llen; i < line_len; i++)
			printf(" ");
		printf("\n");
		pos += llen;
		len -= llen;
	}
}


int wpa_eapol_send(struct wpa_supplicant *wpa_s, int type,
		   u8 *buf, size_t len, int preauth)
{
	u8 *msg;
	size_t msglen;
	struct l2_ethhdr *ethhdr;
	struct ieee802_1x_hdr *hdr;
	int res;

	/* TODO: could add l2_packet_sendmsg that allows fragments to avoid
	 * extra copy here */

	if (preauth && wpa_s->l2_preauth == NULL)
		return -1;

	if (wpa_s->key_mgmt == WPA_KEY_MGMT_PSK && !preauth) {
		/* Current SSID is using WPA-PSK, drop possible frames (mainly,
		 * EAPOL-Start) from EAPOL state machines. */
		wpa_printf(MSG_DEBUG, "WPA: drop TX EAPOL in WPA-PSK mode "
			   "(type=%d len=%d)", type, len);
		return -1;
	}

	if (!preauth && wpa_s->pmksa && type == IEEE802_1X_TYPE_EAPOL_START) {
		/* Trying to use PMKSA caching - do not send EAPOL-Start frames
		 * since they will trigger full EAPOL authentication. */
		wpa_printf(MSG_DEBUG, "RSN: PMKSA caching - do not send "
			   "EAPOL-Start");
		return -1;
	}

	msglen = sizeof(*ethhdr) + sizeof(*hdr) + len;
	msg = malloc(msglen);
	if (msg == NULL)
		return -1;

	ethhdr = (struct l2_ethhdr *) msg;
	memcpy(ethhdr->h_dest, preauth ? wpa_s->preauth_bssid : wpa_s->bssid,
	       ETH_ALEN);
	memcpy(ethhdr->h_source, wpa_s->own_addr, ETH_ALEN);
	ethhdr->h_proto = htons(preauth ? ETH_P_RSN_PREAUTH : ETH_P_EAPOL);

	hdr = (struct ieee802_1x_hdr *) (ethhdr + 1);
	hdr->version = wpa_s->conf->eapol_version;
	hdr->type = type;
	hdr->length = htons(len);

	memcpy((u8 *) (hdr + 1), buf, len);

	wpa_hexdump(MSG_MSGDUMP, "TX EAPOL", msg, msglen);
	res = l2_packet_send(preauth ? wpa_s->l2_preauth : wpa_s->l2, msg,
			     msglen);
	free(msg);
	return res;
}


int wpa_eapol_set_wep_key(struct wpa_supplicant *wpa_s, int unicast,
			  int keyidx, u8 *key, size_t keylen)
{
	if (wpa_s == NULL || wpa_s->driver == NULL ||
	    wpa_s->driver->set_key == NULL)
		return -1;

	return wpa_s->driver->set_key(wpa_s->ifname, WPA_ALG_WEP,
				      unicast ? wpa_s->bssid :
				      (u8 *) "\xff\xff\xff\xff\xff\xff",
				      keyidx, unicast, "", 0, key, keylen);
}


void wpa_supplicant_notify_eapol_done(struct wpa_supplicant *wpa_s)
{
	wpa_printf(MSG_DEBUG, "WPA: EAPOL processing complete");
	eloop_cancel_timeout(wpa_supplicant_scan, wpa_s, NULL);
	wpa_supplicant_cancel_auth_timeout(wpa_s);
}


const char * wpa_ssid_txt(u8 *ssid, size_t ssid_len)
{
	static char ssid_txt[MAX_SSID_LEN + 1];
	char *pos;

	if (ssid_len > MAX_SSID_LEN)
		ssid_len = MAX_SSID_LEN;
	memcpy(ssid_txt, ssid, ssid_len);
	ssid_txt[ssid_len] = '\0';
	for (pos = ssid_txt; *pos != '\0'; pos++) {
		if ((u8) *pos < 32 || (u8) *pos >= 127)
			*pos = '_';
	}
	return ssid_txt;
}


void wpa_supplicant_req_scan(struct wpa_supplicant *wpa_s, int sec, int usec)
{
	eloop_cancel_timeout(wpa_supplicant_scan, wpa_s, NULL);
	eloop_register_timeout(sec, usec, wpa_supplicant_scan, wpa_s, NULL);
}


static void wpa_supplicant_timeout(void *eloop_ctx, void *timeout_ctx)
{
	struct wpa_supplicant *wpa_s = eloop_ctx;
	wpa_printf(MSG_INFO, "Authentication with " MACSTR " timed out.",
		   MAC2STR(wpa_s->bssid));
	wpa_s->reassociate = 1;
	wpa_supplicant_req_scan(wpa_s, 0, 0);
}


static void wpa_supplicant_req_auth_timeout(struct wpa_supplicant *wpa_s,
					    int sec, int usec)
{
	eloop_cancel_timeout(wpa_supplicant_timeout, wpa_s, NULL);
	eloop_register_timeout(sec, usec, wpa_supplicant_timeout, wpa_s, NULL);
}


void wpa_supplicant_cancel_auth_timeout(struct wpa_supplicant *wpa_s)
{
	eloop_cancel_timeout(wpa_supplicant_timeout, wpa_s, NULL);
}


static void wpa_supplicant_cleanup(struct wpa_supplicant *wpa_s)
{
	l2_packet_deinit(wpa_s->l2);
	wpa_s->l2 = NULL;

	if (wpa_s->dot1x_s > -1)
		close(wpa_s->dot1x_s);

	wpa_supplicant_ctrl_iface_deinit(wpa_s);
	wpa_config_free(wpa_s->conf);
	wpa_s->conf = NULL;

	free(wpa_s->assoc_wpa_ie);
	wpa_s->assoc_wpa_ie = NULL;

	free(wpa_s->ap_wpa_ie);
	wpa_s->ap_wpa_ie = NULL;

	free(wpa_s->confname);
	wpa_s->confname = NULL;

	eapol_sm_deinit(wpa_s->eapol);
	wpa_s->eapol = NULL;

	rsn_preauth_deinit(wpa_s);

	pmksa_cache_free(wpa_s);
}


static void wpa_clear_keys(struct wpa_supplicant *wpa_s, u8 *addr)
{
	wpa_s->driver->set_key(wpa_s->ifname, WPA_ALG_NONE,
			       "\xff\xff\xff\xff\xff\xff", 0, 0, NULL,
			       0, NULL, 0);
	wpa_s->driver->set_key(wpa_s->ifname, WPA_ALG_NONE,
			       "\xff\xff\xff\xff\xff\xff", 1, 0, NULL,
			       0, NULL, 0);
	wpa_s->driver->set_key(wpa_s->ifname, WPA_ALG_NONE,
			       "\xff\xff\xff\xff\xff\xff", 2, 0, NULL,
			       0, NULL, 0);
	wpa_s->driver->set_key(wpa_s->ifname, WPA_ALG_NONE,
			       "\xff\xff\xff\xff\xff\xff", 3, 0, NULL,
			       0, NULL, 0);
	if (addr) {
		wpa_s->driver->set_key(wpa_s->ifname, WPA_ALG_NONE, addr,
				       0, 0, NULL, 0, NULL, 0);
	}
}


static void wpa_supplicant_stop_countermeasures(void *eloop_ctx,
						void *sock_ctx)
{
	struct wpa_supplicant *wpa_s = eloop_ctx;

	if (wpa_s->countermeasures) {
		wpa_s->countermeasures = 0;
		wpa_s->driver->set_countermeasures(wpa_s->ifname, 0);
		wpa_printf(MSG_INFO, "WPA: TKIP countermeasures stopped");
		wpa_supplicant_req_scan(wpa_s, 0, 0);
	}
}


void wpa_supplicant_event(struct wpa_supplicant *wpa_s, wpa_event_type event,
			  union wpa_event_data *data)
{
	int pairwise, l, len;
	time_t now;
	u8 bssid[ETH_ALEN], *p;

	switch (event) {
	case EVENT_ASSOC:
		wpa_s->wpa_state = WPA_ASSOCIATED;
		wpa_printf(MSG_DEBUG, "Association event - clear replay "
			   "counter");
		memset(wpa_s->rx_replay_counter, 0, WPA_REPLAY_COUNTER_LEN);
		wpa_s->rx_replay_counter_set = 0;
		if (wpa_s->driver->get_bssid(wpa_s->ifname, bssid) >= 0 &&
		    memcmp(bssid, wpa_s->bssid, ETH_ALEN) != 0) {
			wpa_printf(MSG_DEBUG, "Associated to a new BSS: "
				   "BSSID=" MACSTR, MAC2STR(bssid));
			memcpy(wpa_s->bssid, bssid, ETH_ALEN);
			wpa_clear_keys(wpa_s, bssid);
		}
		eapol_sm_notify_portValid(wpa_s->eapol, FALSE);
		/* 802.1X::portControl = Auto */
		eapol_sm_notify_portEnabled(wpa_s->eapol, TRUE);
		wpa_supplicant_req_auth_timeout(
			wpa_s, wpa_s->key_mgmt == WPA_KEY_MGMT_IEEE8021X ?
			70 : 10, 0);
		break;
	case EVENT_DISASSOC:
		wpa_s->wpa_state = WPA_DISCONNECTED;
		wpa_printf(MSG_DEBUG, "Disconnect event - remove keys");
		wpa_clear_keys(wpa_s, wpa_s->bssid);
		wpa_supplicant_req_scan(wpa_s, 3, 0);
		memset(wpa_s->bssid, 0, ETH_ALEN);
		eapol_sm_notify_portEnabled(wpa_s->eapol, FALSE);
		eapol_sm_notify_portValid(wpa_s->eapol, FALSE);
		break;
	case EVENT_MICHAEL_MIC_FAILURE:
		wpa_printf(MSG_WARNING, "Michael MIC failure detected");
		pairwise = (data && data->michael_mic_failure.unicast);
		wpa_supplicant_key_request(wpa_s, 1, pairwise);
		time(&now);
		if (wpa_s->last_michael_mic_error &&
		    now - wpa_s->last_michael_mic_error <= 60) {
			/* initialize countermeasures */
			wpa_s->countermeasures = 1;
			wpa_printf(MSG_WARNING, "TKIP countermeasures "
				   "started");

			/* Need to wait for completion of request frame. We do
			 * not get any callback for the message completion, so
			 * just wait a short while and hope for the best. */
			usleep(10000);

			wpa_s->driver->set_countermeasures(wpa_s->ifname, 1);
			wpa_supplicant_disassociate(
				wpa_s, REASON_MICHAEL_MIC_FAILURE);
			eloop_cancel_timeout(
				wpa_supplicant_stop_countermeasures, wpa_s,
				NULL);
			eloop_register_timeout(
				60, 0, wpa_supplicant_stop_countermeasures,
				wpa_s, NULL);
		}
		wpa_s->last_michael_mic_error = now;
		break;
	case EVENT_SCAN_RESULTS:
		wpa_supplicant_scan_results(wpa_s);
		break;
	case EVENT_ASSOCINFO:
		wpa_printf(MSG_DEBUG, "Association info event");
		wpa_hexdump(MSG_DEBUG, "req_ies", data->assoc_info.req_ies,
			    data->assoc_info.req_ies_len);
		if (wpa_s->assoc_wpa_ie) {
			free(wpa_s->assoc_wpa_ie);
			wpa_s->assoc_wpa_ie = NULL;
			wpa_s->assoc_wpa_ie_len = 0;
		}

		p = data->assoc_info.req_ies;
		l = data->assoc_info.req_ies_len;

		/* Go through the IEs and make a copy of the WPA IE, if
		 * present. */
		while (l > (2 + 6)) {
			len = p[1] + 2;
			if ((p[0] == GENERIC_INFO_ELEM) && (p[1] > 6) &&
			    (memcmp(&p[2], "\x00\x50\xF2\x01\x01\x00", 6) ==
			     0)) {
				wpa_s->assoc_wpa_ie = malloc(len);
				if (wpa_s->assoc_wpa_ie == NULL)
					break;
				wpa_s->assoc_wpa_ie_len = len;
				memcpy(wpa_s->assoc_wpa_ie, p, len);
				wpa_hexdump(MSG_DEBUG, "assoc_wpa_ie",
					    wpa_s->assoc_wpa_ie,
					    wpa_s->assoc_wpa_ie_len);
				break;
			}
			l -= len;
			p += len;
		}
		break;
	default:
		wpa_printf(MSG_INFO, "Unknown event %d", event);
		break;
	}
}


static void wpa_supplicant_terminate(int sig, void *eloop_ctx,
				     void *signal_ctx)
{
	wpa_printf(MSG_DEBUG, "Signal %d received - terminating", sig);
	eloop_terminate();
}


static void wpa_supplicant_reconfig(int sig, void *eloop_ctx,
				    void *signal_ctx)
{
	struct wpa_supplicant *wpa_s = eloop_ctx;
	struct wpa_config *conf;
	int reconf_ctrl;
	wpa_printf(MSG_DEBUG, "Signal %d received - reconfiguring", sig);
	if (wpa_s->confname == NULL)
		return;
	conf = wpa_config_read(wpa_s->confname);
	if (conf == NULL) {
		wpa_printf(MSG_ERROR, "Failed to parse the configuration file "
			   "'%s' - exiting", wpa_s->confname);
		eloop_terminate();
		return;
	}

	reconf_ctrl = !!conf->ctrl_interface != !!wpa_s->conf->ctrl_interface
		|| (conf->ctrl_interface && wpa_s->conf->ctrl_interface &&
		    strcmp(conf->ctrl_interface, wpa_s->conf->ctrl_interface)
		    != 0);

	if (reconf_ctrl)
		wpa_supplicant_ctrl_iface_deinit(wpa_s);

	wpa_s->current_ssid = NULL;
	eapol_sm_notify_config(wpa_s->eapol, NULL, 0);
	rsn_preauth_deinit(wpa_s);
	wpa_config_free(wpa_s->conf);
	wpa_s->conf = conf;
	if (reconf_ctrl)
		wpa_supplicant_ctrl_iface_init(wpa_s);
	wpa_supplicant_req_scan(wpa_s, 0, 0);
	wpa_printf(MSG_DEBUG, "Reconfiguration completed");
}


void wpa_supplicant_scan(void *eloop_ctx, void *timeout_ctx)
{
	struct wpa_supplicant *wpa_s = eloop_ctx;
	struct wpa_ssid *ssid;

	if (wpa_s->wpa_state == WPA_DISCONNECTED)
		wpa_s->wpa_state = WPA_SCANNING;

	ssid = wpa_s->conf->ssid;
	if (wpa_s->prev_scan_ssid != BROADCAST_SSID_SCAN) {
		while (ssid) {
			if (ssid == wpa_s->prev_scan_ssid) {
				ssid = ssid->next;
				break;
			}
			ssid = ssid->next;
		}
	}
	while (ssid) {
		if (ssid->scan_ssid)
			break;
		ssid = ssid->next;
	}

	wpa_printf(MSG_DEBUG, "Starting AP scan (%s SSID)",
		   ssid ? "specific": "broadcast");
	if (ssid) {
		wpa_hexdump_ascii(MSG_DEBUG, "Scan SSID",
				  ssid->ssid, ssid->ssid_len);
		wpa_s->prev_scan_ssid = ssid;
	} else
		wpa_s->prev_scan_ssid = BROADCAST_SSID_SCAN;

	if (wpa_s->driver->scan(wpa_s->ifname, wpa_s,
				ssid ? ssid->ssid : NULL,
				ssid ? ssid->ssid_len : 0)) {
		wpa_printf(MSG_WARNING, "Failed to initiate AP scan.");
	}

	eloop_register_timeout(5, 0, wpa_supplicant_scan, wpa_s, NULL);
}


static wpa_cipher cipher_suite2driver(int cipher)
{
	switch (cipher) {
	case WPA_CIPHER_NONE:
		return CIPHER_NONE;
	case WPA_CIPHER_WEP40:
		return CIPHER_WEP40;
	case WPA_CIPHER_WEP104:
		return CIPHER_WEP104;
	case WPA_CIPHER_CCMP:
		return CIPHER_CCMP;
	case WPA_CIPHER_TKIP:
	default:
		return CIPHER_TKIP;
	}
}


static wpa_key_mgmt key_mgmt2driver(int key_mgmt)
{
	switch (key_mgmt) {
	case WPA_KEY_MGMT_NONE:
	case WPA_KEY_MGMT_IEEE8021X_NO_WPA:
		return KEY_MGMT_NONE;
	case WPA_KEY_MGMT_IEEE8021X:
		return KEY_MGMT_802_1X;
	case WPA_KEY_MGMT_PSK:
	default:
		return KEY_MGMT_PSK;
	}
}


static int wpa_supplicant_set_suites(struct wpa_supplicant *wpa_s,
				     struct wpa_scan_result *bss,
				     struct wpa_ssid *ssid,
				     u8 *wpa_ie, int *wpa_ie_len)
{
	struct wpa_ie_data ie;
	int sel, proto;
	u8 *ap_ie;
	size_t ap_ie_len;

	if (bss->rsn_ie_len && (ssid->proto & WPA_PROTO_RSN)) {
		wpa_printf(MSG_DEBUG, "RSN: using IEEE 802.11i/D9.0");
		proto = WPA_PROTO_RSN;
		ap_ie = bss->rsn_ie;
		ap_ie_len = bss->rsn_ie_len;
	} else {
		wpa_printf(MSG_DEBUG, "WPA: using IEEE 802.11i/D3.0");
		proto = WPA_PROTO_WPA;
		ap_ie = bss->wpa_ie;
		ap_ie_len = bss->wpa_ie_len;
	}

	if (wpa_parse_wpa_ie(wpa_s, ap_ie, ap_ie_len, &ie)) {
		wpa_printf(MSG_WARNING, "WPA: Failed to parse WPA IE for the "
			   "selected BSS.");
		return -1;
	}

	wpa_s->current_supports_preauth =
		!!(ie.capabilities & WPA_CAPABILITY_PREAUTH);

	wpa_s->proto = proto;
	free(wpa_s->ap_wpa_ie);
	wpa_s->ap_wpa_ie = malloc(ap_ie_len);
	memcpy(wpa_s->ap_wpa_ie, ap_ie, ap_ie_len);
	wpa_s->ap_wpa_ie_len = ap_ie_len;

	sel = ie.group_cipher & ssid->group_cipher;
	if (sel & WPA_CIPHER_CCMP) {
		wpa_s->group_cipher = WPA_CIPHER_CCMP;
	} else if (sel & WPA_CIPHER_TKIP) {
		wpa_s->group_cipher = WPA_CIPHER_TKIP;
	} else if (sel & WPA_CIPHER_WEP104) {
		wpa_s->group_cipher = WPA_CIPHER_WEP104;
	} else if (sel & WPA_CIPHER_WEP40) {
		wpa_s->group_cipher = WPA_CIPHER_WEP40;
	} else {
		wpa_printf(MSG_WARNING, "WPA: Failed to select group cipher.");
		return -1;
	}

	sel = ie.pairwise_cipher & ssid->pairwise_cipher;
	if (sel & WPA_CIPHER_CCMP) {
		wpa_s->pairwise_cipher = WPA_CIPHER_CCMP;
	} else if (sel & WPA_CIPHER_TKIP) {
		wpa_s->pairwise_cipher = WPA_CIPHER_TKIP;
	} else if (sel & WPA_CIPHER_NONE) {
		wpa_s->pairwise_cipher = WPA_CIPHER_NONE;
	} else {
		wpa_printf(MSG_WARNING, "WPA: Failed to select pairwise "
			   "cipher.");
		return -1;
	}

	sel = ie.key_mgmt & ssid->key_mgmt;
	if (sel & WPA_KEY_MGMT_IEEE8021X) {
		wpa_s->key_mgmt = WPA_KEY_MGMT_IEEE8021X;
	} else if (sel & WPA_KEY_MGMT_PSK) {
		wpa_s->key_mgmt = WPA_KEY_MGMT_PSK;
	} else {
		wpa_printf(MSG_WARNING, "WPA: Failed to select authenticated "
			   "key management type.");
		return -1;
	}


	/* Starting new association, so clear the possibly used WPA IE from the
	 * previous association. */
	free(wpa_s->assoc_wpa_ie);
	wpa_s->assoc_wpa_ie = NULL;
	wpa_s->assoc_wpa_ie_len = 0;

	*wpa_ie_len = wpa_gen_wpa_ie(wpa_s, wpa_ie);
	if (*wpa_ie_len < 0) {
		wpa_printf(MSG_WARNING, "WPA: Failed to generate WPA IE.");
		return -1;
	}
	wpa_hexdump(MSG_DEBUG, "WPA: Own WPA IE", wpa_ie, *wpa_ie_len);

	if (ssid->key_mgmt & WPA_KEY_MGMT_PSK)
		memcpy(wpa_s->pmk, ssid->psk, PMK_LEN);
	else if (wpa_s->cur_pmksa)
		memcpy(wpa_s->pmk, wpa_s->cur_pmksa->pmk, PMK_LEN);
	else {
		memset(wpa_s->pmk, 0, PMK_LEN);
		wpa_s->ext_pmk_received = 0;
	}

	return 0;
}


static void wpa_supplicant_associate(struct wpa_supplicant *wpa_s,
				     struct wpa_scan_result *bss,
				     struct wpa_ssid *ssid)
{
	u8 wpa_ie[80];
	int wpa_ie_len;

	wpa_s->reassociate = 0;
	wpa_printf(MSG_INFO, "Trying to associate with " MACSTR " (SSID='%s' "
		   "freq=%d MHz)", MAC2STR(bss->bssid),
		   wpa_ssid_txt(ssid->ssid, ssid->ssid_len), bss->freq);

	if ((bss->wpa_ie_len || bss->rsn_ie_len) &&
	    (ssid->key_mgmt & (WPA_KEY_MGMT_IEEE8021X | WPA_KEY_MGMT_PSK))) {
		wpa_s->cur_pmksa = pmksa_cache_get(wpa_s, bss->bssid, NULL);
		if (wpa_s->cur_pmksa) {
			wpa_hexdump(MSG_DEBUG, "RSN: PMKID",
				    wpa_s->cur_pmksa->pmkid, PMKID_LEN);
		}
		if (wpa_supplicant_set_suites(wpa_s, bss, ssid,
					      wpa_ie, &wpa_ie_len)) {
			wpa_printf(MSG_WARNING, "WPA: Failed to set WPA key "
				   "management and encryption suites");
			return;
		}
	} else {
		if (ssid->key_mgmt & WPA_KEY_MGMT_IEEE8021X_NO_WPA)
			wpa_s->key_mgmt = WPA_KEY_MGMT_IEEE8021X_NO_WPA;
		else
			wpa_s->key_mgmt = WPA_KEY_MGMT_NONE;
		free(wpa_s->ap_wpa_ie);
		wpa_s->ap_wpa_ie = NULL;
		wpa_s->ap_wpa_ie_len = 0;
		free(wpa_s->assoc_wpa_ie);
		wpa_s->assoc_wpa_ie = NULL;
		wpa_s->assoc_wpa_ie_len = 0;
		wpa_s->pairwise_cipher = WPA_CIPHER_NONE;
		wpa_s->group_cipher = WPA_CIPHER_NONE;
		wpa_ie_len = 0;
		wpa_s->cur_pmksa = FALSE;
	}

	wpa_clear_keys(wpa_s, bss->bssid);
	wpa_s->wpa_state = WPA_ASSOCIATING;
	wpa_s->driver->associate(wpa_s->ifname, bss->bssid,
				 bss->ssid, bss->ssid_len, bss->freq,
				 wpa_ie, wpa_ie_len,
				 cipher_suite2driver(wpa_s->pairwise_cipher),
				 cipher_suite2driver(wpa_s->group_cipher),
				 key_mgmt2driver(wpa_s->key_mgmt));

	wpa_supplicant_req_auth_timeout(
		wpa_s, wpa_s->key_mgmt == WPA_KEY_MGMT_IEEE8021X ? 70 : 10, 0);

	if (wpa_s->key_mgmt == WPA_KEY_MGMT_PSK) {
		eapol_sm_notify_eap_success(wpa_s->eapol, FALSE);
		eapol_sm_notify_eap_fail(wpa_s->eapol, FALSE);
	}
	wpa_s->current_ssid = ssid;
	eapol_sm_notify_config(wpa_s->eapol, ssid,
			       wpa_s->key_mgmt ==
			       WPA_KEY_MGMT_IEEE8021X_NO_WPA);
}


void wpa_supplicant_disassociate(struct wpa_supplicant *wpa_s,
				 int reason_code)
{
	u8 *addr = NULL;
	wpa_s->wpa_state = WPA_DISCONNECTED;
	if (memcmp(wpa_s->bssid, "\x00\x00\x00\x00\x00\x00", ETH_ALEN) != 0) {
		wpa_s->driver->disassociate(wpa_s->ifname, wpa_s->bssid,
					    reason_code);
		addr = wpa_s->bssid;
	}
	wpa_clear_keys(wpa_s, addr);
	wpa_s->current_ssid = NULL;
	eapol_sm_notify_config(wpa_s->eapol, NULL, 0);
	eapol_sm_notify_portEnabled(wpa_s->eapol, FALSE);
	eapol_sm_notify_portValid(wpa_s->eapol, FALSE);
}


static void wpa_supplicant_scan_results(struct wpa_supplicant *wpa_s)
{
#define SCAN_AP_LIMIT 50
	struct wpa_scan_result results[SCAN_AP_LIMIT];
	int num, i;
	struct wpa_scan_result *bss, *selected = NULL;
	struct wpa_ssid *ssid;

	num = wpa_s->driver->get_scan_results(wpa_s->ifname, results,
					      SCAN_AP_LIMIT);
	wpa_printf(MSG_DEBUG, "Scan results: %d", num);
	if (num < 0)
		return;
	if (num > SCAN_AP_LIMIT) {
		wpa_printf(MSG_INFO, "Not enough room for all APs (%d < %d)",
			   num, SCAN_AP_LIMIT);
		num = SCAN_AP_LIMIT;
	}

	bss = NULL;
	ssid = NULL;
	/* First, try to find WPA-enabled AP */
	for (i = 0; i < num && !selected; i++) {
		bss = &results[i];
		wpa_printf(MSG_DEBUG, "%d: " MACSTR " ssid='%s' "
			   "wpa_ie_len=%d rsn_ie_len=%d",
			   i, MAC2STR(bss->bssid),
			   wpa_ssid_txt(bss->ssid, bss->ssid_len),
			   bss->wpa_ie_len, bss->rsn_ie_len);
		if (bss->wpa_ie_len == 0 && bss->rsn_ie_len == 0)
			continue;

		ssid = wpa_s->conf->ssid;
		while (ssid) {
			struct wpa_ie_data ie;
			if (bss->ssid_len == ssid->ssid_len &&
			    memcmp(bss->ssid, ssid->ssid, bss->ssid_len) == 0
			    &&
			    (!ssid->bssid_set ||
			     memcmp(bss->bssid, ssid->bssid, ETH_ALEN) == 0) &&
			    (((ssid->proto & WPA_PROTO_RSN) &&
			      wpa_parse_wpa_ie(wpa_s, bss->rsn_ie,
					       bss->rsn_ie_len, &ie) == 0) ||
			     ((ssid->proto & WPA_PROTO_WPA) &&
			      wpa_parse_wpa_ie(wpa_s, bss->wpa_ie,
					       bss->wpa_ie_len, &ie) == 0)) &&
			    (ie.proto & ssid->proto) &&
			    (ie.pairwise_cipher & ssid->pairwise_cipher) &&
			    (ie.group_cipher & ssid->group_cipher) &&
			    (ie.key_mgmt & ssid->key_mgmt)) {
				selected = bss;
				break;
			}
			ssid = ssid->next;
		}
	}

	/* If no WPA-enabled AP found, try to find non-WPA AP, if configuration
	 * allows this. */
	for (i = 0; i < num && !selected; i++) {
		bss = &results[i];
		ssid = wpa_s->conf->ssid;
		while (ssid) {
			if (bss->ssid_len == ssid->ssid_len &&
			    memcmp(bss->ssid, ssid->ssid, bss->ssid_len) == 0
			    &&
			    (!ssid->bssid_set ||
			     memcmp(bss->bssid, ssid->bssid, ETH_ALEN) == 0) &&
			    ((ssid->key_mgmt & WPA_KEY_MGMT_NONE) ||
			     (ssid->key_mgmt & WPA_KEY_MGMT_IEEE8021X_NO_WPA)))
			{
				selected = bss;
				break;
			}
			ssid = ssid->next;
		}
	}

	if (selected) {
		if (wpa_s->reassociate ||
		    memcmp(selected->bssid, wpa_s->bssid, ETH_ALEN) != 0)
			wpa_supplicant_associate(wpa_s, selected, ssid);
		else {
			wpa_printf(MSG_DEBUG, "Already associated with the "
				   "selected AP.");
			rsn_preauth_scan_results(wpa_s, results, num);
		}
	} else
		wpa_printf(MSG_DEBUG, "No suitable AP found.");
}


static void wpa_supplicant_dot1x_receive(int sock, void *eloop_ctx,
					 void *sock_ctx)
{
	struct wpa_supplicant *wpa_s = eloop_ctx;
	u8 buf[128];
	int res;

	res = recv(sock, buf, sizeof(buf), 0);
	wpa_printf(MSG_DEBUG, "WPA: Receive from dot1x (Xsupplicant) socket "
		   "==> %d", res);
	if (res < 0) {
		perror("recv");
		return;
	}

	if (res != PMK_LEN) {
		wpa_printf(MSG_WARNING, "WPA: Invalid master key length (%d) "
			   "from dot1x", res);
		return;
	}

	wpa_hexdump(MSG_DEBUG, "WPA: Master key (dot1x)", buf, PMK_LEN);
	if (wpa_s->key_mgmt & WPA_KEY_MGMT_IEEE8021X) {
		memcpy(wpa_s->pmk, buf, PMK_LEN);
		wpa_s->ext_pmk_received = 1;
	} else {
		wpa_printf(MSG_INFO, "WPA: Not in IEEE 802.1X mode - dropping "
			   "dot1x PMK update (%d)", wpa_s->key_mgmt);
	}
}


static int wpa_supplicant_802_1x_init(struct wpa_supplicant *wpa_s)
{
	int s;
	struct sockaddr_un addr;

	s = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (s < 0) {
		perror("socket");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_LOCAL;
	addr.sun_path[0] = '\0';
	snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
		 "wpa_supplicant");
	if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("bind");
		close(s);
		return -1;
	}

	wpa_s->dot1x_s = s;
	eloop_register_read_sock(s, wpa_supplicant_dot1x_receive, wpa_s,
				 NULL);
	return 0;
}


#ifdef CONFIG_DRIVER_HOSTAP
extern struct wpa_driver_ops wpa_driver_hostap_ops; /* driver_hostap.c */
#endif /* CONFIG_DRIVER_HOSTAP */
#ifdef CONFIG_DRIVER_PRISM54
extern struct wpa_driver_ops wpa_driver_prism54_ops; /* driver_prism54.c */
#endif /* CONFIG_DRIVER_PRISM54 */
#ifdef CONFIG_DRIVER_HERMES
extern struct wpa_driver_ops wpa_driver_hermes_ops; /* driver_hermes.c */
#endif /* CONFIG_DRIVER_HERMES */
#ifdef CONFIG_NDISWRAPPER
extern struct wpa_driver_ops wpa_ndiswrapper_ops; /* driver_ndiswrapper.c */
#endif /* CONFIG_DRIVER_NDISWRAPPER */

static int wpa_supplicant_set_driver(struct wpa_supplicant *wpa_s,
				     const char *name)
{
	struct wpa_driver_ops *def_drv;

	if (wpa_s == NULL)
		return -1;

#ifdef CONFIG_DRIVER_HOSTAP
	def_drv = &wpa_driver_hostap_ops;
#elif CONFIG_DRIVER_PRISM54
	def_drv = &wpa_driver_prism54_ops;
#elif CONFIG_DRIVER_HERMES
	def_drv = &wpa_driver_hermes_ops;
#elif CONFIG_NDISWRAPPER
	def_drv = &wpa_ndiswrapper_ops;
#else
#error No driver support included in .config.
#error See README for more information.
#endif

	if (name == NULL)
		wpa_s->driver = def_drv;
#ifdef CONFIG_DRIVER_HOSTAP
	else if (strcmp(name, "hostap") == 0)
		wpa_s->driver = &wpa_driver_hostap_ops;
#endif /* CONFIG_DRIVER_HOSTAP */
#ifdef CONFIG_DRIVER_PRISM54
	else if (strcmp(name, "prism54") == 0)
		wpa_s->driver = &wpa_driver_prism54_ops;
#endif /* CONFIG_DRIVER_PRISM54 */
#ifdef CONFIG_DRIVER_HERMES
	else if (strcmp(name, "hermes") == 0)
		wpa_s->driver = &wpa_driver_hermes_ops;
#endif /* CONFIG_DRIVER_HERMES */
	else {
		printf("Unsupported driver '%s'.\n", name);
		return -1;
	}
	return 0;
}


static void wpa_supplicant_fd_workaround(void)
{
	int s, i;
	/* When started from pcmcia-cs scripts, wpa_supplicant might start with
	 * fd 0, 1, and 2 closed. This will cause some issues because many
	 * places in wpa_supplicant are still printing out to stdout. As a
	 * workaround, make sure that fd's 0, 1, and 2 are not used for other
	 * sockets. */
	for (i = 0; i < 3; i++) {
		s = open("/dev/null", O_RDWR);
		if (s > 2) {
			close(s);
			break;
		}
	}
}


static void usage(void)
{
	printf("%s\n\n%s\n"
	       "usage:\n"
	       "  wpa_supplicant [-BddehLqqvw] -i<ifname> -c<config file> "
	       "[-D<driver>]\n"
	       "\n"
	       "drivers:\n"
#ifdef CONFIG_DRIVER_HOSTAP
	       "  hostap = Host AP driver (Intersil Prism2/2.5/3)\n"
#endif  /* CONFIG_DRIVER_HOSTAP */
#ifdef CONFIG_DRIVER_PRISM54
	       "  prism54 = Prism54.org driver (Intersil Prism GT/Duette/"
	       "Indigo)\n"
#endif  /* CONFIG_DRIVER_PRISM54 */
#ifdef CONFIG_DRIVER_HERMES
	       "  hermes = Agere Systems Inc. driver (Hermes-I/Hermes-II)\n"
#endif  /* CONFIG_DRIVER_HERMES */
	       "options:\n"
	       "  -B = run daemon in the background\n"
	       "  -d = increase debugging verbosity (-dd even more)\n"
#ifdef IEEE8021X_EAPOL
	       "  -e = use external IEEE 802.1X Supplicant (e.g., "
	       "xsupplicant)\n"
	       "       (this disables the internal Supplicant)\n"
#endif /* IEEE8021X_EAPOL */
	       "  -h = show this help text\n"
	       "  -L = show license (GPL and BSD)\n"
	       "  -q = decrease debugging verbosity (-qq even less)\n"
	       "  -v = show version\n"
	       "  -w = wait for interface to be added, if needed\n",
	       wpa_supplicant_version, wpa_supplicant_license);
}


static void license(void)
{
	printf("%s\n\n%s\n",
	       wpa_supplicant_version, wpa_supplicant_full_license);
}


int main(int argc, char *argv[])
{
	struct wpa_supplicant wpa_s;
	char *ifname = NULL;
	int c;
	const char *confname = NULL, *driver = NULL;
	int daemonize = 0, wait_for_interface = 0, disable_eapol = 0;

	memset(&wpa_s, 0, sizeof(wpa_s));

	for (;;) {
		c = getopt(argc, argv, "Bc:D:dehi:Lqvw");
		if (c < 0)
			break;
		switch (c) {
		case 'B':
			daemonize++;
			break;
		case 'c':
			confname = optarg;
			break;
		case 'D':
			driver = optarg;
			break;
		case 'd':
			wpa_debug_level--;
			break;
#ifdef IEEE8021X_EAPOL
		case 'e':
			disable_eapol++;
			break;
#endif /* IEEE8021X_EAPOL */
		case 'h':
			usage();
			return -1;
		case 'i':
			ifname = optarg;
			break;
		case 'L':
			license();
			return -1;
		case 'q':
			wpa_debug_level++;
			break;
		case 'v':
			printf("%s\n", wpa_supplicant_version);
			return -1;
		case 'w':
			wait_for_interface++;
			break;
		default:
			usage();
			return -1;
		}
	}

	wpa_supplicant_fd_workaround();
	eloop_init(&wpa_s);

	if (wpa_supplicant_set_driver(&wpa_s, driver) < 0) {
		return -1;
	}

	if (confname) {
		wpa_s.confname = rel2abs_path(confname);
		if (wpa_s.confname == NULL) {
			wpa_printf(MSG_ERROR, "Failed to get absolute path "
				   "for configuration file '%s'.", confname);
			return -1;
		}
		wpa_printf(MSG_DEBUG, "Configuration file '%s' -> '%s'",
			   confname, wpa_s.confname);
		wpa_s.conf = wpa_config_read(wpa_s.confname);
		if (wpa_s.conf == NULL) {
			printf("Failed to read configuration file '%s'.\n",
			       wpa_s.confname);
			return 1;
		}
	}

	if (wpa_s.conf == NULL || wpa_s.conf->ssid == NULL) {
		usage();
		printf("\nNo networks (SSID) configured.\n");
		return -1;
	}

	if (ifname == NULL) {
		usage();
		printf("\nInterface name is required.\n");
		return -1;
	}
	if (strlen(ifname) >= sizeof(wpa_s.ifname)) {
		printf("Too long interface name '%s'.\n", ifname);
		return -1;
	}
	strncpy(wpa_s.ifname, ifname, sizeof(wpa_s.ifname));

	if (wpa_supplicant_ctrl_iface_init(&wpa_s)) {
		printf("Failed to initialize control interface '%s'.\n",
		       wpa_s.conf->ctrl_interface);
		return -1;
	}

	if (wait_for_interface && daemonize) {
		wpa_printf(MSG_DEBUG, "Daemonize..");
		if (daemon(0, 0)) {
			perror("daemon");
			return -1;
		}
	}

	if (!disable_eapol) {
		wpa_s.eapol = eapol_sm_init(&wpa_s, 0);
		if (wpa_s.eapol == NULL) {
			printf("Failed to initialize EAPOL state machines.\n");
			return -1;
		}
	}

	/* RSNA Supplicant Key Management - INITIALIZE */
	eapol_sm_notify_portEnabled(wpa_s.eapol, FALSE);
	eapol_sm_notify_portValid(wpa_s.eapol, FALSE);

	/* Register driver event handler before L2 receive handler so that
	 * association events are processed before EAPOL-Key packets if both
	 * become available for the same select() call. */
	wpa_s.events_priv = wpa_s.driver->events_init(&wpa_s);
	if (wpa_s.events_priv == NULL) {
		fprintf(stderr, "Failed to initialize driver event "
			"processing\n");
		return -1;
	}

	for (;;) {
		wpa_s.l2 = l2_packet_init(wpa_s.ifname, ETH_P_EAPOL,
					  wpa_supplicant_rx_eapol, &wpa_s);
		if (wpa_s.l2)
			break;
		else if (!wait_for_interface)
			return -1;
		printf("Waiting for interface..\n");
		sleep(5);
	}
	if (l2_packet_get_own_addr(wpa_s.l2, wpa_s.own_addr)) {
		fprintf(stderr, "Failed to get own L2 address\n");
		return -1;
	}

	if (wpa_supplicant_init_counter(&wpa_s))
		return -1;

	if (wpa_s.driver->set_wpa(wpa_s.ifname, 1) < 0) {
		fprintf(stderr, "Failed to enable WPA in the driver.\n");
		return -1;
	}

	wpa_clear_keys(&wpa_s, NULL);

	/* Make sure that TKIP countermeasures are not left enabled (could
	 * happen if wpa_supplicant is killed during countermeasures. */
	wpa_s.driver->set_countermeasures(wpa_s.ifname, 0);

	wpa_s.driver->set_drop_unencrypted(wpa_s.ifname, 1);

	eloop_register_timeout(0, 100000, wpa_supplicant_scan, &wpa_s, NULL);

	if (disable_eapol)
		wpa_supplicant_802_1x_init(&wpa_s);

	if (!wait_for_interface && daemonize) {
		wpa_printf(MSG_DEBUG, "Daemonize..");
		if (daemon(0, 0)) {
			perror("daemon");
			return -1;
		}
	}

	eloop_register_signal(SIGINT, wpa_supplicant_terminate, NULL);
	eloop_register_signal(SIGTERM, wpa_supplicant_terminate, NULL);
	eloop_register_signal(SIGHUP, wpa_supplicant_reconfig, NULL);

	eloop_run();

	wpa_supplicant_disassociate(&wpa_s, REASON_DEAUTH_LEAVING);
	if (wpa_s.driver->set_wpa(wpa_s.ifname, 0) < 0) {
		fprintf(stderr, "Failed to disable WPA in the driver.\n");
	}

	if (wpa_s.events_priv)
		wpa_s.driver->events_deinit(&wpa_s, wpa_s.events_priv);

	wpa_s.driver->set_drop_unencrypted(wpa_s.ifname, 0);
	wpa_s.driver->set_countermeasures(wpa_s.ifname, 0);

	if (wpa_s.driver->cleanup)
		wpa_s.driver->cleanup(wpa_s.ifname);

	wpa_supplicant_cleanup(&wpa_s);

	eloop_destroy();

	return 0;
}