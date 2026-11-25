/* find.c — find bar state and logic.
 * SPDX-License-Identifier: MIT
 */
#include "find.h"
#include <string.h>

void find_bar_draw(App *a)
{
	int fy;
	char prompt[320];
	if (!a->find_active)
		return;
	fy = a->status_y - a->line_h - 8;
	disp_fill_rect(a, a->text_x, fy, a->text_w, a->line_h + 4, a->col_paper.pixel);
	disp_stroke_rect(a, a->text_x, fy, a->text_w, a->line_h + 4, a->col_line.pixel);
	snprintf(prompt, sizeof prompt, "Find: %s", a->find_buf);
	XftDrawStringUtf8(a->draw, &a->col_fg, a->font, a->text_x + 8, fy + a->ascent + 2,
	                  (FcChar8 *)prompt, (int)strlen(prompt));
	if (a->caret_on)
		{
		int cx = a->text_x + 8 + (int)strlen("Find: ") * a->char_w + (int)a->find_len * a->char_w;
		disp_fill_rect(a, cx, fy + 3, 1, a->line_h, a->col_caret.pixel);
		}
}

void find_next(App *a)
{
	size_t start, i;
	size_t flen = a->find_len;
	if (flen == 0)
		return;
	start = a->cursor + 1;
	if (start >= a->len)
		start = 0;
	for (i = start; i + flen <= a->len; i++)
		{
		if (memcmp(a->text + i, a->find_buf, flen) == 0)
			{
			a->anchor = i;
			a->cursor = i + flen;
			a->selecting = 1;
			disp_reveal_cursor(a);
			return;
			}
		}
	for (i = 0; i < start && i + flen <= a->len; i++)
		{
		if (memcmp(a->text + i, a->find_buf, flen) == 0)
			{
			a->anchor = i;
			a->cursor = i + flen;
			a->selecting = 1;
			disp_reveal_cursor(a);
			return;
			}
		}
}

void find_open(App *a)
{
	a->find_active = 1;
	a->find_len = 0;
	a->find_buf[0] = 0;
}

void find_close(App *a)
{
	a->find_active = 0;
	a->find_buf[0] = 0;
	a->find_len = 0;
}   