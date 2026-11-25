/* buffer.c — text buffer, lines, undo, selection, cursor movement.
 * SPDX-License-Identifier: MIT
 */
#include "buffer.h"
#include <string.h>
#include <strings.h>

/* --- internal helpers --- */

static void snap_free(Snap *s)
{
	free(s->text);
	s->text = NULL;
	s->len = 0;
}

static void restore_snap(App *a, const Snap *s)
{
	buf_ensure(a, s->len + 1);
	memcpy(a->text, s->text, s->len);
	a->text[s->len] = 0;
	a->len = s->len;
	a->cursor = s->cur > s->len ? s->len : s->cur;
	buf_clear_sel(a);
	buf_rebuild_lines(a);
}

void buf_delete_sel(App *a)
{
	size_t sa, sb;
	buf_sel_range(a, &sa, &sb);
	if (sa == sb)
		return;
	buf_push_undo(a);
	buf_delete_range(a, sa, sb);
}

/* --- public API --- */

void buf_ensure(App *a, size_t need)
{
	if (need <= a->cap)
		return;
	while (a->cap < need)
		a->cap = a->cap < 64 ? 256 : a->cap * 2;
	a->text = xrealloc(a->text, a->cap + 1);
	a->text[a->len] = 0;
}

void buf_rebuild_lines(App *a)
{
	size_t i;
	a->nlines = 0;
	if (a->lines_cap < 16)
		{
		a->lines_cap = 16;
		a->lines = xrealloc(a->lines, a->lines_cap * sizeof *a->lines);
		}
	a->lines[a->nlines++] = 0;
	for (i = 0; i < a->len; i++)
		{
		if (a->text[i] != '\n')
			continue;
		if (a->nlines + 1 >= a->lines_cap)
			{
			a->lines_cap *= 2;
			a->lines = xrealloc(a->lines, a->lines_cap * sizeof *a->lines);
			}
		a->lines[a->nlines++] = i + 1;
		}
}

size_t buf_line_of(App *a, size_t pos)
{
	size_t lo = 0, hi = a->nlines;
	while (lo + 1 < hi)
		{
		size_t mid = (lo + hi) / 2;
		if (a->lines[mid] <= pos)
			lo = mid;
		else
			hi = mid;
		}
	return lo;
}

size_t buf_line_end(App *a, size_t li)
{
	if (li + 1 < a->nlines)
		return a->lines[li + 1] - 1;
	return a->len;
}

size_t buf_col_at(App *a, size_t pos)
{
	size_t li = buf_line_of(a, pos);
	size_t i = a->lines[li];
	size_t col = 0;
	while (i < pos && i < a->len)
		{
		int n = utf8_len((const unsigned char *)a->text + i, a->len - i);
		if (a->text[i] == '\n')
			break;
		col += (size_t)cp_width((const unsigned char *)a->text + i, n);
		i += (size_t)n;
		}
	return col;
}

size_t buf_pos_at_col(App *a, size_t li, size_t want)
{
	size_t i = a->lines[li];
	size_t end = buf_line_end(a, li);
	size_t col = 0;
	while (i < end)
		{
		int n = utf8_len((const unsigned char *)a->text + i, a->len - i);
		size_t w = (size_t)cp_width((const unsigned char *)a->text + i, n);
		if (col + w > want)
			return i;
		col += w;
		i += (size_t)n;
		}
	return end;
}

void buf_clear_sel(App *a)
{
	a->selecting = 0;
	a->anchor = a->cursor;
}

void buf_sel_range(App *a, size_t *a_out, size_t *b_out)
{
	if (!a->selecting || a->anchor == a->cursor)
		{
		*a_out = *b_out = a->cursor;
		return;
		}
	if (a->anchor < a->cursor)
		{
		*a_out = a->anchor;
		*b_out = a->cursor;
		}
	else
		{
		*a_out = a->cursor;
		*b_out = a->anchor;
		}
}

void buf_push_undo(App *a)
{
	Snap *s;
	if (a->len > (8u << 20))
		return;
	while (a->undo_len > a->undo_pos)
		{
		a->undo_len--;
		snap_free(&a->undo_stack[a->undo_len]);
		}
	if (a->undo_len == UNDO_MAX)
		{
		snap_free(&a->undo_stack[0]);
		memmove(a->undo_stack, a->undo_stack + 1, (UNDO_MAX - 1) * sizeof(Snap));
		a->undo_len--;
		a->undo_pos--;
		}
	s = &a->undo_stack[a->undo_len++];
	s->text = xmalloc(a->len + 1);
	memcpy(s->text, a->text ? a->text : "", a->len);
	s->text[a->len] = 0;
	s->len = a->len;
	s->cur = a->cursor;
	a->undo_pos = a->undo_len;
}

void buf_undo(App *a)
{
	if (a->undo_pos == 0)
		return;
	if (a->undo_pos == a->undo_len)
		{
		buf_push_undo(a);
		if (a->undo_pos < 2)
			return;
		a->undo_pos -= 2;
		}
	else
		{
		a->undo_pos--;
		}
	restore_snap(a, &a->undo_stack[a->undo_pos]);
}

void buf_delete_range(App *a, size_t a_pos, size_t b_pos)
{
	if (a_pos > b_pos)
		{
		size_t t = a_pos;
		a_pos = b_pos;
		b_pos = t;
		}
	if (a_pos > a->len)
		a_pos = a->len;
	if (b_pos > a->len)
		b_pos = a->len;
	memmove(a->text + a_pos, a->text + b_pos, a->len - b_pos);
	a->len -= b_pos - a_pos;
	a->text[a->len] = 0;
	a->cursor = a_pos;
	buf_clear_sel(a);
	buf_rebuild_lines(a);
}

void buf_insert_bytes(App *a, const char *s, size_t n)
{
	if (n == 0)
		return;
	if (a->selecting && a->anchor != a->cursor)
		buf_delete_sel(a);
	else
		buf_push_undo(a);
	buf_ensure(a, a->len + n + 1);
	memmove(a->text + a->cursor + n, a->text + a->cursor, a->len - a->cursor);
	memcpy(a->text + a->cursor, s, n);
	a->len += n;
	a->text[a->len] = 0;
	a->cursor += n;
	buf_clear_sel(a);
	buf_rebuild_lines(a);
	a->pref_col = buf_col_at(a, a->cursor);
}

void buf_backspace(App *a)
{
	size_t prev;
	if (a->selecting && a->anchor != a->cursor)
		{
		buf_delete_sel(a);
		return;
		}
	if (a->cursor == 0)
		return;
	prev = utf8_prev(a->text, a->cursor);
	buf_push_undo(a);
	memmove(a->text + prev, a->text + a->cursor, a->len - a->cursor);
	a->len -= a->cursor - prev;
	a->text[a->len] = 0;
	a->cursor = prev;
	buf_clear_sel(a);
	buf_rebuild_lines(a);
	a->pref_col = buf_col_at(a, a->cursor);
}

void buf_delete_fwd(App *a)
{
	int n;
	if (a->selecting && a->anchor != a->cursor)
		{
		buf_delete_sel(a);
		return;
		}
	if (a->cursor >= a->len)
		return;
	n = utf8_len((const unsigned char *)a->text + a->cursor, a->len - a->cursor);
	buf_push_undo(a);
	memmove(a->text + a->cursor, a->text + a->cursor + n, a->len - a->cursor - (size_t)n);
	a->len -= (size_t)n;
	a->text[a->len] = 0;
	buf_clear_sel(a);
	buf_rebuild_lines(a);
}

void buf_move_left(App *a, int extend)
{
	size_t n = utf8_prev(a->text, a->cursor);
	if (!extend)
		buf_clear_sel(a);
	else if (!a->selecting)
		{
		a->anchor = a->cursor;
		a->selecting = 1;
		}
	a->cursor = n;
	a->pref_col = buf_col_at(a, a->cursor);
	if (extend && a->cursor == a->anchor)
		a->selecting = 0;
}

void buf_move_right(App *a, int extend)
{
	if (a->cursor < a->len)
		{
		int n = utf8_len((const unsigned char *)a->text + a->cursor, a->len - a->cursor);
		if (!extend)
			buf_clear_sel(a);
		else if (!a->selecting)
			{
			a->anchor = a->cursor;
			a->selecting = 1;
			}
		a->cursor += (size_t)n;
		if (a->cursor > a->len)
			a->cursor = a->len;
		}
	else if (!extend)
		{
		buf_clear_sel(a);
		}
	a->pref_col = buf_col_at(a, a->cursor);
	if (extend && a->cursor == a->anchor)
		a->selecting = 0;
}

void buf_move_vert(App *a, int dir, int extend)
{
	size_t li = buf_line_of(a, a->cursor);
	if (!extend)
		buf_clear_sel(a);
	else if (!a->selecting)
		{
		a->anchor = a->cursor;
		a->selecting = 1;
		}
	if (dir < 0 && li > 0)
		li--;
	else if (dir > 0 && li + 1 < a->nlines)
		li++;
	a->cursor = buf_pos_at_col(a, li, a->pref_col);
	if (extend && a->cursor == a->anchor)
		a->selecting = 0;
}

void buf_move_home(App *a, int extend, int doc)
{
	if (!extend)
		buf_clear_sel(a);
	else if (!a->selecting)
		{
		a->anchor = a->cursor;
		a->selecting = 1;
		}
	a->cursor = doc ? 0 : a->lines[buf_line_of(a, a->cursor)];
	a->pref_col = buf_col_at(a, a->cursor);
}

void buf_move_end(App *a, int extend, int doc)
{
	if (!extend)
		buf_clear_sel(a);
	else if (!a->selecting)
		{
		a->anchor = a->cursor;
		a->selecting = 1;
		}
	a->cursor = doc ? a->len : buf_line_end(a, buf_line_of(a, a->cursor));
	a->pref_col = buf_col_at(a, a->cursor);
}

void buf_select_all(App *a)
{
	a->anchor = 0;
	a->cursor = a->len;
	a->selecting = a->anchor != a->cursor;
	a->pref_col = buf_col_at(a, a->cursor);
}

void buf_kill_line(App *a)
{
	size_t end = buf_line_end(a, buf_line_of(a, a->cursor));
	size_t b = end;
	if (a->cursor == end && end < a->len && a->text[end] == '\n')
		b = end + 1;
	if (b == a->cursor)
		return;
	buf_push_undo(a);
	buf_delete_range(a, a->cursor, b);
}   
