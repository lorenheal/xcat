/* utf8.c — UTF-8 helpers for xcat.
 * SPDX-License-Identifier: MIT
 */
#include "utf8.h"
#include <wchar.h>

int utf8_len(const unsigned char *s, size_t avail)
{
	unsigned char c;
	if (avail == 0)
		return 0;
	c = s[0];
	if (c < 0x80)
		return 1;
	if ((c & 0xE0) == 0xC0 && avail >= 2 && (s[1] & 0xC0) == 0x80 && c >= 0xC2)
		return 2;
	if ((c & 0xF0) == 0xE0 && avail >= 3 &&
	    (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80)
		return 3;
	if ((c & 0xF8) == 0xF0 && avail >= 4 && c <= 0xF4 &&
	    (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80)
		return 4;
	return 1;
}

size_t utf8_prev(const char *text, size_t pos)
{
	size_t i;
	if (pos == 0)
		return 0;
	i = pos - 1;
	while (i > 0 && ((unsigned char)text[i] & 0xC0) == 0x80 && pos - i < 4)
		i--;
	if (utf8_len((const unsigned char *)text + i, pos - i) == (int)(pos - i))
		return i;
	return pos - 1;
}

int cp_width(const unsigned char *s, int n)
{
	wchar_t wc;
	int w;
	if (n == 1 && s[0] == '\t')
		return TAB_COLS;
	if (n == 1 && s[0] < 0x20)
		return 1;
	if (mbtowc(&wc, (const char *)s, (size_t)n) <= 0)
		return 1;
	w = wcwidth(wc);
	return w > 0 ? w : 1;
}   