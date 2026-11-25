/* input.c — keyboard, mouse, and scroll handling.
 * SPDX-License-Identifier: MIT
 */
#include "input.h"
#include "buffer.h"
#include "display.h"
#include "clipboard.h"
#include "find.h"
#include "fileio.h"
#include "menu.h"
#include <string.h>

int input_in_rect(Rect r, int x, int y)
{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

void input_scroll_by(App *a, int lines_delta)
{
	if (lines_delta < 0)
		{
		size_t d = (size_t)(-lines_delta);
		a->top_line = a->top_line > d ? a->top_line - d : 0;
		}
	else
		{
		a->top_line += (size_t)lines_delta;
		if (a->nlines == 0)
			a->top_line = 0;
		else if (a->top_line >= a->nlines)
			a->top_line = a->nlines - 1;
		}
}

void input_place_click(App *a, int x, int y, int extend)
{
	int rel;
	size_t li;
	if (y < a->text_y)
		y = a->text_y;
	rel = (y - a->text_y) / a->line_h;
	if (rel < 0)
		rel = 0;
	li = a->top_line + (size_t)rel;
	if (a->nlines == 0)
		return;
	if (li >= a->nlines)
		li = a->nlines - 1;
	if (!extend)
		buf_clear_sel(a);
	else if (!a->selecting)
		{
		a->anchor = a->cursor;
		a->selecting = 1;
		}
	a->cursor = disp_pos_from_px(a, li, x - a->text_x + a->xoff);
	a->pref_col = buf_col_at(a, a->cursor);
	a->caret_on = 1;
}

void input_handle_key(App *a, XKeyEvent *ev)
{
	char buf[64];
	KeySym ks = 0;
	Status st = 0;
	int n = 0;
	int shift = (ev->state & ShiftMask) != 0;
	int ctrl = (ev->state & ControlMask) != 0;
	int meta = (ev->state & Mod1Mask) != 0;

	if (a->ic)
		n = Xutf8LookupString(a->ic, ev, buf, (int)sizeof buf - 1, &ks, &st);
	else
		n = XLookupString(ev, buf, (int)sizeof buf - 1, &ks, NULL);
	if (n < 0)
		n = 0;
	buf[n] = 0;
	a->caret_on = 1;

	/* pipe bar intercepts keys when active */
	if (a->pipe_active)
		{
		if (ks == XK_Return || ks == XK_KP_Enter)
			{
			fileio_pipe_run(a);
			return;
			}
		if (ks == XK_Escape)
			{
			fileio_pipe_close(a);
			return;
			}
		if (ks == XK_BackSpace)
			{
			if (a->pipe_len > 0)
				a->pipe_buf[--a->pipe_len] = 0;
			return;
			}
		if (n > 0 && (unsigned char)buf[0] >= 0x20 && a->pipe_len < sizeof a->pipe_buf - 1)
			{
			memcpy(a->pipe_buf + a->pipe_len, buf, (size_t)n);
			a->pipe_len += (size_t)n;
			a->pipe_buf[a->pipe_len] = 0;
			return;
			}
		return;
		}

	/* find bar intercepts keys when active */
	if (a->find_active)
		{
		if (ks == XK_Return || ks == XK_KP_Enter)
			{
			find_next(a);
			return;
			}
		if (ks == XK_Escape)
			{
			find_close(a);
			return;
			}
		if (ks == XK_BackSpace)
			{
			if (a->find_len > 0)
				a->find_buf[--a->find_len] = 0;
			return;
			}
		if (n > 0 && (unsigned char)buf[0] >= 0x20 && a->find_len < sizeof a->find_buf - 1)
			{
			memcpy(a->find_buf + a->find_len, buf, (size_t)n);
			a->find_len += (size_t)n;
			a->find_buf[a->find_len] = 0;
			return;
			}
		return;
		}

	if (ctrl && (ks == XK_f || ks == XK_F))
		{
		find_open(a);
		return;
		}

	if (ctrl && !meta && (ks == XK_Return || ks == XK_KP_Enter || ks == XK_s || ks == XK_S))
		{
		accept(a);
		return;
		}
	if (ks == XK_Escape || (ctrl && (ks == XK_g || ks == XK_G)))
		{
		cancel(a);
		return;
		}
	if (ctrl && (ks == XK_a || ks == XK_A))
		{
		buf_select_all(a);
		return;
		}
	if (ctrl && (ks == XK_c || ks == XK_C || ks == XK_Insert))
		{
		clip_copy_sel(a);
		return;
		}
	if (ctrl && (ks == XK_x || ks == XK_X))
		{
		clip_cut_sel(a);
		disp_reveal_cursor(a);
		return;
		}
	if ((ctrl && (ks == XK_v || ks == XK_V)) || (shift && ks == XK_Insert))
		{
		clip_request_paste(a, a->xa_clip);
		return;
		}
	if (ctrl && (ks == XK_z || ks == XK_Z))
		{
		buf_undo(a);
		disp_reveal_cursor(a);
		return;
		}
	if (ctrl && (ks == XK_k || ks == XK_K))
		{
		buf_kill_line(a);
		disp_reveal_cursor(a);
		return;
		}
	if (ks == XK_BackSpace || (ctrl && ks == XK_h))
		{
		buf_backspace(a);
		disp_reveal_cursor(a);
		return;
		}
	if (ks == XK_Delete || ks == XK_KP_Delete)
		{
		buf_delete_fwd(a);
		disp_reveal_cursor(a);
		return;
		}
	if (ks == XK_Left || ks == XK_KP_Left)
		{
		buf_move_left(a, shift);
		disp_reveal_cursor(a);
		return;
		}
	if (ks == XK_Right || ks == XK_KP_Right)
		{
		buf_move_right(a, shift);
		disp_reveal_cursor(a);
		return;
		}
	if (ks == XK_Up || ks == XK_KP_Up)
		{
		buf_move_vert(a, -1, shift);
		disp_reveal_cursor(a);
		return;
		}
	if (ks == XK_Down || ks == XK_KP_Down)
		{
		buf_move_vert(a, 1, shift);
		disp_reveal_cursor(a);
		return;
		}
	if (ks == XK_Home || ks == XK_KP_Home)
		{
		buf_move_home(a, shift, ctrl);
		disp_reveal_cursor(a);
		return;
		}
	if (ks == XK_End || ks == XK_KP_End)
		{
		buf_move_end(a, shift, ctrl);
		disp_reveal_cursor(a);
		return;
		}
	if (ks == XK_Page_Up || ks == XK_KP_Page_Up)
		{
		size_t vis = (size_t)(a->text_h / a->line_h);
		size_t i;
		if (vis < 1)
			vis = 1;
		for (i = 0; i < vis; i++)
			buf_move_vert(a, -1, shift);
		disp_reveal_cursor(a);
		return;
		}
	if (ks == XK_Page_Down || ks == XK_KP_Page_Down)
		{
		size_t vis = (size_t)(a->text_h / a->line_h);
		size_t i;
		if (vis < 1)
			vis = 1;
		for (i = 0; i < vis; i++)
			buf_move_vert(a, 1, shift);
		disp_reveal_cursor(a);
		return;
		}
	if (ks == XK_Return || ks == XK_KP_Enter)
		{
		buf_insert_bytes(a, "\n", 1);
		disp_reveal_cursor(a);
		return;
		}
	if (ks == XK_Tab && !ctrl)
		{
		buf_insert_bytes(a, "\t", 1);
		disp_reveal_cursor(a);
		return;
		}
	if (!ctrl && !meta && n > 0 && (unsigned char)buf[0] >= 0x20)
		{
		buf_insert_bytes(a, buf, (size_t)n);
		disp_reveal_cursor(a);
		}
}   