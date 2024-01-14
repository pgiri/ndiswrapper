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

#include <linux/random.h>

#include "ntoskernel.h"
#include "crt_exports.h"

#if LINUX_VERSION_CODE > KERNEL_VERSION(5,18,0)
struct rnd_state rand_state;
#endif
    
#ifdef CONFIG_X86_64
/* Windows long is 32-bit, so strip single 'l' in integer formats */
static void strip_l_modifier(char *str)
{
	char *ptr = str;
	int in_format = 0;
	char *lptr = NULL;
	char last = 0;
	char *end_ptr;
	char *wptr;

	/* Replace single 'l' inside integer formats with '\0' */
	for (ptr = str; *ptr; ptr++) {
		if (!in_format) {
			if (*ptr == '%')
				in_format = 1;
			last = *ptr;
			continue;
		}
		switch (*ptr) {
		case 'd':
		case 'i':
		case 'o':
		case 'u':
		case 'x':
		case 'X':
		case 'p':
		case 'n':
		case 'm':
			if (lptr) {
				*lptr = '\0';
				lptr = NULL;
			}
			in_format = 0;
			break;
		case 'c':
		case 'C':
		case 's':
		case 'S':
		case 'f':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
		case 'a':
		case 'A':
			lptr = NULL;
			in_format = 0;
			break;
		case '%':
			lptr = NULL;
			if (last == '%')
				in_format = 0;
			else
				in_format = 1;	/* ignore previous junk */
			break;
		case 'l':
			if (last == 'l')
				lptr = NULL;
			else
				lptr = ptr;
			break;
		default:
			break;
		}
		last = *ptr;
	}

	/* Purge zeroes from the resulting string */
	end_ptr = ptr;
	wptr = str;
	for (ptr = str; ptr < end_ptr; ptr++)
		if (*ptr != 0)
			*(wptr++) = *ptr;
	*wptr = 0;
}

/*
 * va_list on x86_64 Linux is designed to allow passing arguments in registers
 * even to variadic functions.  va_list is a structure holding pointers to the
 * register save area, which holds the arguments passed in registers, and to
 * the stack, which may have the arguments that did not fit the registers.
 * va_list also holds offsets in the register save area for the next general
 * purpose and floating point registers that the next va_arg() would fetch.
 *
 * Unlike Linux, the Windows va_list is just a pointer to the stack.  No
 * arguments are passed in the registers.  That's why we construct the Linux
 * va_list so that the register save area is never used.  For that goal, we set
 * the offsets to the maximal allowed values, meaning that the arguments passed
 * in the registers have been exhausted.  The values are 48 for general purpose
 * registers (6 registers, 8 bytes each) and 304 for floating point registers
 * (16 registers, 16 bytes each, on top of general purpose register).
 */

struct x86_64_va_list {
	int gp_offset;
	int fp_offset;
	void *overflow_arg_area;
	void *reg_save_area;
};

#define VA_LIST_DECL(_args) \
	va_list _args##new; \
	struct x86_64_va_list *_args##x;
#define VA_LIST_PREP(_args) \
do { \
	_args##x = (struct x86_64_va_list *)&_args##new; \
	_args##x->gp_offset = 6 * 8;		/* GP registers exhausted */ \
	_args##x->fp_offset = 6 * 8 + 16 * 16;	/* FP registers exhausted */ \
	_args##x->overflow_arg_area = (void *)_args; \
	_args##x->reg_save_area = NULL; \
} while (0)
#define VA_LIST_CONV(_args) (_args##new)
#define VA_LIST_FREE(_args)
#define FMT_DECL(_fmt) \
	char *_fmt##copy; \
	int _fmt##len;
#define FMT_PREP(_fmt) \
do { \
	_fmt##len = strlen(format) + 1; \
	_fmt##copy = kmalloc(_fmt##len, irql_gfp()); \
	if (_fmt##copy) { \
		memcpy(_fmt##copy, format, _fmt##len); \
		strip_l_modifier(_fmt##copy); \
	} \
} while (0)
#define FMT_CONV(_fmt) (_fmt##copy ? _fmt##copy : format)
#define FMT_FREE(_fmt) kfree(_fmt##copy)

#else /* !CONFIG_X86_64 */

#define VA_LIST_DECL(_args)
#define VA_LIST_PREP(_args)
#define VA_LIST_CONV(_args) (_args)
#define VA_LIST_FREE(_args)
#define FMT_DECL(_fmt)
#define FMT_PREP(_fmt)
#define FMT_CONV(_fmt) (format)
#define FMT_FREE(_fmt)

#endif /* !CONFIG_X86_64 */

__attribute__((format(printf, 2, 3)))
noregparm INT WIN_FUNC(_win_sprintf,12)
	(char *buf, const char *format, ...)
{
	va_list args;
	int res;
	FMT_DECL(format)

	FMT_PREP(format);
	va_start(args, format);
	res = vsprintf(buf, FMT_CONV(format), args);
	va_end(args);
	FMT_FREE(format);

	TRACE2("buf: %p: %s", buf, buf);
	return res;
}

noregparm INT WIN_FUNC(swprintf,12)
	(wchar_t *buf, const wchar_t *format, ...)
{
	TODO();
	EXIT2(return 0);
}

noregparm INT WIN_FUNC(_win_vsprintf,3)
	(char *str, const char *format, va_list ap)
{
	INT i;
	VA_LIST_DECL(ap)
	FMT_DECL(format)

	VA_LIST_PREP(ap);
	FMT_PREP(format);

	i = vsprintf(str, FMT_CONV(format), VA_LIST_CONV(ap));
	TRACE2("str: %p: %s", str, str);

	FMT_FREE(format);
	VA_LIST_FREE(ap);
	EXIT2(return i);
}

__attribute__((format(printf, 3, 4)))
noregparm INT WIN_FUNC(_win_snprintf,12)
	(char *buf, SIZE_T count, const char *format, ...)
{
	va_list args;
	int res;
	FMT_DECL(format)

	FMT_PREP(format);
	va_start(args, format);
	res = vsnprintf(buf, count, FMT_CONV(format), args);
	va_end(args);
	TRACE2("buf: %p: %s", buf, buf);

	FMT_FREE(format);
	return res;
}

__attribute__((format(printf, 3, 4)))
noregparm INT WIN_FUNC(_win__snprintf,12)
	(char *buf, SIZE_T count, const char *format, ...)
{
	va_list args;
	int res;
	FMT_DECL(format)

	FMT_PREP(format);
	va_start(args, format);
	res = vsnprintf(buf, count, FMT_CONV(format), args);
	va_end(args);
	TRACE2("buf: %p: %s", buf, buf);

	FMT_FREE(format);
	return res;
}

noregparm INT WIN_FUNC(_win_vsnprintf,4)
	(char *str, SIZE_T size, const char *format, va_list ap)
{
	INT i;
	VA_LIST_DECL(ap)
	FMT_DECL(format)

	VA_LIST_PREP(ap);
	FMT_PREP(format);

	i = vsnprintf(str, size, FMT_CONV(format), VA_LIST_CONV(ap));
	TRACE2("str: %p: %s", str, str);

	FMT_FREE(format);
	VA_LIST_FREE(ap);
	EXIT2(return i);
}

noregparm INT WIN_FUNC(_win__vsnprintf,4)
	(char *str, SIZE_T size, const char *format, va_list ap)
{
	INT i;
	VA_LIST_DECL(ap)
	FMT_DECL(format)

	VA_LIST_PREP(ap);
	FMT_PREP(format);

	i = vsnprintf(str, size, FMT_CONV(format), VA_LIST_CONV(ap));
	TRACE2("str: %p: %s", str, str);

	FMT_FREE(format);
	VA_LIST_FREE(ap);
	EXIT2(return i);
}

noregparm INT WIN_FUNC(_win__vsnwprintf,4)
	(wchar_t *str, SIZE_T size, const wchar_t *format, va_list ap)
{
	int ret;

	TODO();		/* format expansion not implemented */
	_win_wcsncpy(str, format, size);
	ret = _win_wcslen(format);
	if (ret >= size)
		ret = -1;
	return ret;
}

noregparm char *WIN_FUNC(_win_strncpy,3)
	(char *dst, char *src, SIZE_T n)
{
	return strncpy(dst, src, n);
}

noregparm SIZE_T WIN_FUNC(_win_strlen,1)
	(const char *s)
{
	return strlen(s);
}

noregparm INT WIN_FUNC(_win_strncmp,3)
	(const char *s1, const char *s2, SIZE_T n)
{
	return strncmp(s1, s2, n);
}

noregparm INT WIN_FUNC(_win_strcmp,2)
	(const char *s1, const char *s2)
{
	return strcmp(s1, s2);
}

noregparm INT WIN_FUNC(_win_stricmp,2)
	(const char *s1, const char *s2)
{
	return stricmp(s1, s2);
}

noregparm char *WIN_FUNC(_win_strncat,3)
	(char *dest, const char *src, SIZE_T n)
{
	return strncat(dest, src, n);
}

noregparm INT WIN_FUNC(_win_wcscmp,2)
	(const wchar_t *s1, const wchar_t *s2)
{
	while (*s1 && *s1 == *s2) {
		s1++;
		s2++;
	}
	return *s1 - *s2;
}

noregparm INT WIN_FUNC(_win_wcsicmp,2)
	(const wchar_t *s1, const wchar_t *s2)
{
	while (*s1 && tolower((char)*s1) == tolower((char)*s2)) {
		s1++;
		s2++;
	}
	return tolower((char)*s1) - tolower((char)*s2);
}

noregparm SIZE_T WIN_FUNC(_win_wcslen,1)
	(const wchar_t *s)
{
	const wchar_t *t = s;
	while (*t)
		t++;
	return t - s;
}

noregparm wchar_t *WIN_FUNC(_win_wcsncpy,3)
	(wchar_t *dest, const wchar_t *src, SIZE_T n)
{
	const wchar_t *s;
	wchar_t *d;
	s = src + n;
	d = dest;
	while (src < s && (*d++ = *src++))
		;
	if (s > src)
		memset(d, 0, (s - src) * sizeof(wchar_t));
	return dest;
}

noregparm wchar_t *WIN_FUNC(_win_wcscpy,2)
	(wchar_t *dest, const wchar_t *src)
{
	wchar_t *d = dest;
	while ((*d++ = *src++))
		;
	return dest;
}

noregparm wchar_t *WIN_FUNC(_win_wcscat,2)
	(wchar_t *dest, const wchar_t *src)
{
	wchar_t *d;
	d = dest;
	while (*d)
		d++;
	while ((*d++ = *src++))
		;
	return dest;
}

noregparm INT WIN_FUNC(_win_towupper,1)
	(wchar_t c)
{
	return toupper(c);
}

noregparm INT WIN_FUNC(_win_towlower,1)
	(wchar_t c)
{
	return tolower(c);
}

noregparm INT WIN_FUNC(_win_tolower,1)
	(INT c)
{
	return tolower(c);
}

noregparm INT WIN_FUNC(_win_toupper,1)
	(INT c)
{
	return toupper(c);
}

noregparm void *WIN_FUNC(_win_strcpy,2)
	(void *to, const void *from)
{
	return strcpy(to, from);
}

noregparm char *WIN_FUNC(_win_strstr,2)
	(const char *s1, const char *s2)
{
	return strstr(s1, s2);
}

noregparm char *WIN_FUNC(_win_strchr,2)
	(const char *s, int c)
{
	return strchr(s, c);
}

noregparm char *WIN_FUNC(_win_strrchr,2)
	(const char *s, int c)
{
	return strrchr(s, c);
}

noregparm void *WIN_FUNC(_win_memmove,3)
	(void *to, void *from, SIZE_T count)
{
	return memmove(to, from, count);
}

noregparm void *WIN_FUNC(_win_memchr,3)
	(const void *s, INT c, SIZE_T n)
{
	return memchr(s, c, n);
}

noregparm void *WIN_FUNC(_win_memcpy,3)
	(void *to, const void *from, SIZE_T n)
{
	return memcpy(to, from, n);
}

noregparm void *WIN_FUNC(_win_memset,3)
	(void *s, char c, SIZE_T count)
{
	return memset(s, c, count);
}

noregparm int WIN_FUNC(_win_memcmp,3)
	(void *s1, void *s2, SIZE_T n)
{
	return memcmp(s1, s2, n);
}

noregparm void WIN_FUNC(_win_srand,1)
	(UINT seed)
{
    #if LINUX_VERSION_CODE <= KERNEL_VERSION(5,18,0)
		prandom_seed(seed);
    #else		
		prandom_seed_state(&rand_state, seed);
    #endif
}

noregparm int WIN_FUNC(rand,0)
	(void)
{
	char buf[6];
	int i, n;

    #if LINUX_VERSION_CODE <= KERNEL_VERSION(5,18,0)
		get_random_bytes(buf, sizeof(buf));
    #else		
		prandom_bytes_state(&rand_state, buf, sizeof(buf));
    #endif
	for (n = i = 0; i < sizeof(buf); i++)
		n += buf[i];
	return n;
}

noregparm int WIN_FUNC(_win_atoi,1)
	(const char *ptr)
{
	int i = simple_strtol(ptr, NULL, 10);
	return i;
}

noregparm int WIN_FUNC(_win_isdigit,1)
	(int c)
{
	return isdigit(c);
}

noregparm int WIN_FUNC(_win_isprint,1)
	(int c)
{
	return isprint(c);
}

wstdcall s64 WIN_FUNC(_alldiv,2)
	(s64 a, s64 b)
{
	return a / b;
}

wstdcall u64 WIN_FUNC(_aulldiv,2)
	(u64 a, u64 b)
{
	return a / b;
}

wstdcall s64 WIN_FUNC(_allmul,2)
	(s64 a, s64 b)
{
	return a * b;
}

wstdcall u64 WIN_FUNC(_aullmul,2)
	(u64 a, u64 b)
{
	return a * b;
}

wstdcall s64 WIN_FUNC(_allrem,2)
	(s64 a, s64 b)
{
	return a % b;
}

wstdcall u64 WIN_FUNC(_aullrem,2)
	(u64 a, u64 b)
{
	return a % b;
}

regparm3 s64 WIN_FUNC(_allshl,2)
	(s64 a, u8 b)
{
	return a << b;
}

regparm3 u64 WIN_FUNC(_aullshl,2)
	(u64 a, u8 b)
{
	return a << b;
}

regparm3 s64 WIN_FUNC(_allshr,2)
	(s64 a, u8 b)
{
	return a >> b;
}

regparm3 u64 WIN_FUNC(_aullshr,2)
	(u64 a, u8 b)
{
	return a >> b;
}

int stricmp(const char *s1, const char *s2)
{
	while (*s1 && tolower(*s1) == tolower(*s2)) {
		s1++;
		s2++;
	}
	return *s1 - *s2;
}

void dump_bytes(const char *ctx, const u8 *from, int len)
{
	int i, j;
	u8 *buf;

	buf = kmalloc(len * 3 + 1, irql_gfp());
	if (!buf) {
		ERROR("couldn't allocate memory");
		return;
	}
	for (i = j = 0; i < len; i++, j += 3) {
		sprintf(&buf[j], "%02x ", from[i]);
	}
	buf[j] = 0;
	printk(KERN_DEBUG "%s: %p: %s\n", ctx, from, buf);
	kfree(buf);
}
