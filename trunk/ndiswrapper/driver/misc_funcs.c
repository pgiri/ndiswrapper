/*
 *  Copyright (C) 2003-2004 Pontus Fuchs, Giridhar Pemmasani
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
#include <linux/types.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <linux/ctype.h>
#include <linux/net.h>

#include "ndis.h"
#include "ntoskernel.h"

unsigned long RtlCompareMemory(char *b, char *a, unsigned long len)
{
	unsigned long i;
	DBGTRACE("%s: Entry\n", __FUNCTION__);

	for(i = 0; (i < len) && a[i] == b[i]; i++)
		;
	return i;
}

STDCALL long RtlCompareString(const struct ustring *s1,
							  const struct ustring *s2, int case_insensitive)
{
	unsigned int len;
	long ret = 0;
	const char *p1, *p2;
	
	DBGTRACE("%s: entry\n", __FUNCTION__);
	len = min(s1->len, s2->len);
	p1 = s1->buf;
	p2 = s2->buf;
	
	if (case_insensitive)
		while (!ret && len--)
			ret = toupper(*p1++) - toupper(*p2++);
	else
		while (!ret && len--)
			ret = *p1++ - *p2++;
	if (!ret)
		ret = s1->len - s2->len;
	return ret;
}


STDCALL long RtlCompareUnicodeString(const struct ustring *s1,
				     const struct ustring *s2,
				     int case_insensitive)
{
	unsigned int len;
	long ret = 0;
	const __u16 *p1, *p2;
	
	DBGTRACE("%s: entry\n", __FUNCTION__);
	len = min(s1->len, s2->len);
	p1 = (__u16 *)s1->buf;
	p2 = (__u16 *)s2->buf;
	
	if (case_insensitive)
		while (!ret && len--)
			ret = toupper((__u8)*p1++) - toupper((__u8)*p2++);
	else
		while (!ret && len--)
			ret = *p1++ - *p2++;
	if (!ret)
		ret = s1->len - s2->len;
	return ret;
}

STDCALL int RtlEqualString(const struct ustring *s1,
			   const struct ustring *s2, int case_insensitive)
{
	DBGTRACE("%s: entry\n", __FUNCTION__);
	if (s1->len != s2->len)
		return 0;
	return !RtlCompareString(s1, s2, case_insensitive);
}

STDCALL int RtlEqualUnicodeString(const struct ustring *s1,
				  const struct ustring *s2,
				  int case_insensitive)
{
	if (s1->len != s2->len)
		return 0;
	return !RtlCompareUnicodeString(s1, s2, case_insensitive);
}

STDCALL void RtlCopyUnicodeString(struct ustring *dst,
				  const struct ustring *src)
{
	DBGTRACE("%s: entry\n", __FUNCTION__);
	if (src)
	{
		unsigned int len = min(src->len, dst->buflen);
		memcpy(dst->buf, src->buf, len);
		dst->len = len;
		/* append terminating '\0' if enough space */
		if (len < dst->buflen)
			dst->buf[len] = 0;
	}
	else dst->len = 0;
}

STDCALL int RtlAnsiStringToUnicodeString(struct ustring *dst, struct ustring *src, unsigned int dup)
{
	int i;
	__u16 *d;
	__u8 *s;

	DBGTRACE("%s: dup: %d src: %s\n", __FUNCTION__, dup, src->buf);
	if(dup)
	{
		char *buf = kmalloc((src->buflen+1) * sizeof(__u16), GFP_KERNEL);
		if(!buf)
			return NDIS_STATUS_FAILURE;
		dst->buf = buf;
		dst->buflen = (src->buflen+1) * sizeof(__u16);
	}
	else if (dst->buflen < (src->len+1) * sizeof(__u16))
		return NDIS_STATUS_FAILURE;

	dst->len = src->len * sizeof(__u16);
	d = (__u16 *)dst->buf;
	s = (__u8 *)src->buf;
	for(i = 0; i < src->len; i++)
	{
		d[i] = (__u16)s[i];
	}
	d[i] = 0;
	
	return NDIS_STATUS_SUCCESS;
}

STDCALL int RtlUnicodeStringToAnsiString(struct ustring *dst, struct ustring *src, unsigned int dup)
{
	int i;
	__u16 *s;
	__u8 *d;

//	DBGTRACE("%s dup: %d src->len: %d src->buflen: %d, dst: %p\n", __FUNCTION__, dup, src->len, src->buflen, dst);
	if(dup)
	{
		char *buf = kmalloc((src->buflen+1) / sizeof(__u16), GFP_KERNEL);
		if(!buf)
			return NDIS_STATUS_FAILURE;
		dst->buf = buf;
		dst->buflen = (src->buflen+1) / sizeof(__u16);
	}
	else if (dst->buflen < (src->len+1) / sizeof(__u16))
		return NDIS_STATUS_FAILURE;

	dst->len = src->len / sizeof(__u16);
	s = (__u16 *)src->buf;
	d = (__u8 *)dst->buf;
	for(i = 0; i < dst->len; i++)
		d[i] = (__u8)s[i];
	d[i] = 0;

//	DBGTRACE(" buf: %s\n", dst->buf);
	return NDIS_STATUS_SUCCESS;
}

STDCALL int RtlIntegerToUnicodeString(unsigned long value, unsigned long base,
									  struct ustring *ustring)
{
	char string[sizeof(unsigned long) * 8 + 1];
	struct ustring ansi;
	int i;

	DBGTRACE("%s: entry\n", __FUNCTION__);
	if (base == 0)
		base = 10;
	if (!(base == 2 || base == 8 || base == 10 || base == 16))
		return NDIS_STATUS_INVALID_PARAMETER;
	for (i = 0; value && i < sizeof(string); i++)
	{
		int r;
		r = value % base;
		value /= base;
		if (r < 10)
			string[i] = r + '0';
		else
			string[i] = r + 'a' - 10;
	}

	if (i < sizeof(string))
		string[i] = 0;
	else
		return NDIS_STATUS_BUFFER_TOO_SHORT;

	ansi.buf = string;
	ansi.len = strlen(string);
	ansi.buflen = sizeof(string);
	return RtlAnsiStringToUnicodeString(ustring, &ansi, 0);
}

void RtlFreeUnicodeString(void){UNIMPL();}
void RtlUnwind(void){UNIMPL();}

/*
 * This is the packet_recycler that gets scheduled from NdisMIndicateReceivePacket
 */
void packet_recycler(void *param)
{
	struct ndis_handle *handle = (struct ndis_handle*) param;

	DBGTRACE("%s Packet recycler running\n", __FUNCTION__);
	while(1)
	{
		struct ndis_packet * packet;

		spin_lock(&handle->recycle_packets_lock);
		packet = 0;
		if(!list_empty(&handle->recycle_packets))
		{
			packet = (struct ndis_packet*) handle->recycle_packets.next;

			list_del(handle->recycle_packets.next);
			DBGTRACE("%s Picking packet at %p!\n", __FUNCTION__, packet);
			packet = (struct ndis_packet*) ((char*)packet - ((char*) &packet->recycle_list - (char*) &packet->nr_pages));
		}

		spin_unlock(&handle->recycle_packets_lock);
		
		if(!packet)
			break;

		handle->driver->miniport_char.return_packet(handle->adapter_ctx,  packet);
	}
}

void inline my_dumpstack(void)
{
	int *sp = (int*) getSp();
	int i;
	for(i = 0; i < 20; i++)
	{
		printk("%08x\n", sp[i]);
	}
}

int getSp(void)
{
	volatile int i;
	asm("movl %esp,(%esp,1)");
	return i;
}

