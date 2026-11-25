/* display.c — X11 init, colors, font, layout, drawing.
 * SPDX-License-Identifier: MIT
 */
#include "display.h"
#include "menu.h"
#include "find.h"
#include <string.h>

static int alloc_color(App *a, XftColor *c, const char *name)
{
	if (!XftColorAllocName(a->dpy, a->visual, a->cmap, name, c))
		return -1;
	return 0;
}

static XftFont *open_font(App *a, const char *prefer)
{
	static const char *fallbacks[] =
		{
		"DejaVu Sans Mono:size=13",
		"Liberation Mono:size=13",
		"Nimbus Mono PS:size=13",
		"monospace:size=13",
		"fixed:size=13",
		NULL
		};
	XftFont *f;
	int i;
	if (prefer && (f = XftFontOpenName(a->dpy, a->scr, prefer)))
		return f;
	for (i = 0; fallbacks[i]; i++)
		{
		f = XftFontOpenName(a->dpy, a->scr, fallbacks[i]);
		if (f)
			return f;
		}
	return NULL;
}

void disp_layout(App *a)
{
	int pad = 12;
	int btn_h = 30;
	int btn_w = 108;
	int gap = 8;
	int find_h = (a->find_active || a->pipe_active) ? (a->line_h + 8) : 0;

	a->status_y = a->win_h - pad - btn_h - gap - a->line_h - 6 - find_h;
	a->text_x = pad;
	a->text_y = MENU_H + pad;
	a->text_w = a->win_w - pad * 2;
	a->text_h = a->status_y - gap - a->text_y;
	if (a->text_h < a->line_h)
		a->text_h = a->line_h;
	a->cancel_r.w = btn_w;
	a->cancel_r.h = btn_h;
	a->cancel_r.x = a->win_w - pad - btn_w - gap - btn_w;
	a->cancel_r.y = a->win_h - pad - btn_h;
	a->ok_r.w = btn_w;
	a->ok_r.h = btn_h;
	a->ok_r.x = a->win_w - pad - btn_w;
	a->ok_r.y = a->cancel_r.y;
}

void disp_ensure_pixmap(App *a)
{
	if (a->pm && a->pm_w == a->win_w && a->pm_h == a->win_h)
		return;
	if (a->draw)
		XftDrawDestroy(a->draw);
	if (a->pm)
		XFreePixmap(a->dpy, a->pm);
	a->pm = XCreatePixmap(a->dpy, a->win, (unsigned)a->win_w, (unsigned)a->win_h,
	                      (unsigned)DefaultDepth(a->dpy, a->scr));
	a->pm_w = a->win_w;
	a->pm_h = a->win_h;
	a->draw = XftDrawCreate(a->dpy, a->pm, a->visual, a->cmap);
}

int disp_prefix_px(App *a, size_t line_start, size_t pos)
{
	size_t i = line_start;
	int px = 0;
	while (i < pos && i < a->len && a->text[i] != '\n')
		{
		int n = utf8_len((const unsigned char *)a->text + i, a->len - i);
		int cols = cp_width((const unsigned char *)a->text + i, n);
		px += cols * a->char_w;
		i += (size_t)n;
		}
	return px;
}

size_t disp_pos_from_px(App *a, size_t li, int px)
{
	size_t i = a->lines[li];
	size_t end = buf_line_end(a, li);
	int acc = 0;
	if (px < 0)
		return i;
	while (i < end)
		{
		int n = utf8_len((const unsigned char *)a->text + i, a->len - i);
		int cols = cp_width((const unsigned char *)a->text + i, n);
		int w = cols * a->char_w;
		if (acc + w / 2 >= px)
			return i;
		acc += w;
		i += (size_t)n;
		}
	return end;
}

void disp_reveal_cursor(App *a)
{
	size_t li = buf_line_of(a, a->cursor);
	size_t vis = (size_t)(a->text_h / a->line_h);
	int px;
	if (a->text_w <= 0 || a->text_h <= 0)
		return;
	if (vis == 0)
		vis = 1;
	if (li < a->top_line)
		a->top_line = li;
	if (li >= a->top_line + vis)
		a->top_line = li + 1 - vis;
	px = disp_prefix_px(a, a->lines[li], a->cursor) - a->xoff;
	if (px < 0)
		a->xoff = disp_prefix_px(a, a->lines[li], a->cursor);
	if (px > a->text_w - a->char_w)
		a->xoff = disp_prefix_px(a, a->lines[li], a->cursor) - (a->text_w - a->char_w);
	if (a->xoff < 0)
		a->xoff = 0;
}

void disp_fill_rect(App *a, int x, int y, int w, int h, unsigned long pixel)
{
	if (w <= 0 || h <= 0)
		return;
	XSetForeground(a->dpy, a->gc, pixel);
	XFillRectangle(a->dpy, a->pm, a->gc, x, y, (unsigned)w, (unsigned)h);
}

void disp_stroke_rect(App *a, int x, int y, int w, int h, unsigned long pixel)
{
	XSetForeground(a->dpy, a->gc, pixel);
	XDrawRectangle(a->dpy, a->pm, a->gc, x, y, (unsigned)(w - 1), (unsigned)(h - 1));
}

void disp_draw_button(App *a, Rect r, const char *label, int hot, int primary)
{
	XftColor *bg = primary ? &a->col_okbg : &a->col_btn;
	XftColor *fg = primary ? &a->col_okfg : &a->col_btnfg;
	XGlyphInfo ext;
	int tw, tx, ty;
	disp_fill_rect(a, r.x, r.y, r.w, r.h, bg->pixel);
	if (hot)
		disp_stroke_rect(a, r.x, r.y, r.w, r.h, a->col_caret.pixel);
	else
		disp_stroke_rect(a, r.x, r.y, r.w, r.h, a->col_muted.pixel);
	XftTextExtentsUtf8(a->dpy, a->font, (FcChar8 *)label, (int)strlen(label), &ext);
	tw = ext.xOff;
	tx = r.x + (r.w - tw) / 2;
	ty = r.y + (r.h + a->ascent - 2) / 2;
	XftDrawStringUtf8(a->draw, fg, a->font, tx, ty, (FcChar8 *)label, (int)strlen(label));
}

void disp_draw_all(App *a)
{
	size_t vis, li;
	int y;
	char status[160];
	size_t sel_a, sel_b;
	XRectangle clip;

	disp_ensure_pixmap(a);
	disp_layout(a);
	disp_fill_rect(a, 0, 0, a->win_w, a->win_h, a->col_bg.pixel);
	disp_fill_rect(a, a->text_x - 4, a->text_y - 4, a->text_w + 8, a->text_h + 8, a->col_paper.pixel);
	disp_stroke_rect(a, a->text_x - 4, a->text_y - 4, a->text_w + 8, a->text_h + 8, a->col_line.pixel);

	clip.x = (short)a->text_x;
	clip.y = (short)a->text_y;
	clip.width = (unsigned short)a->text_w;
	clip.height = (unsigned short)a->text_h;
	XftDrawSetClipRectangles(a->draw, 0, 0, &clip, 1);

	vis = (size_t)(a->text_h / a->line_h);
	if (vis == 0)
		vis = 1;
	buf_sel_range(a, &sel_a, &sel_b);
	for (li = a->top_line, y = a->text_y; li < a->nlines && li < a->top_line + vis; li++, y += a->line_h)
		{
		size_t start = a->lines[li];
		size_t end = buf_line_end(a, li);
		int x = a->text_x - a->xoff;
		size_t i = start;
		if (sel_a != sel_b && sel_b > start && sel_a < end)
			{
			size_t sa = sel_a > start ? sel_a : start;
			size_t sb = sel_b < end ? sel_b : end;
			int x1 = a->text_x + disp_prefix_px(a, start, sa) - a->xoff;
			int x2 = a->text_x + disp_prefix_px(a, start, sb) - a->xoff;
			if (x2 > x1)
				disp_fill_rect(a, x1, y, x2 - x1, a->line_h, a->col_sel.pixel);
			}
		while (i < end)
			{
			int n = utf8_len((const unsigned char *)a->text + i, a->len - i);
			int cols = cp_width((const unsigned char *)a->text + i, n);
			int cw = cols * a->char_w;
			if (x + cw >= a->text_x && x < a->text_x + a->text_w)
				{
				char tmp[8];
				const char *drawp = a->text + i;
				int dn = n;
				if (n == 1 && (unsigned char)a->text[i] < 0x20 && a->text[i] != '\t')
					{
					tmp[0] = (char)0xC2;
					tmp[1] = (char)0xB7;
					drawp = tmp;
					dn = 2;
					}
				else if (a->text[i] == '\t')
					{
					drawp = NULL;
					}
				if (drawp)
					XftDrawStringUtf8(a->draw, &a->col_fg, a->font, x, y + a->ascent,
					                  (FcChar8 *)drawp, dn);
				}
			x += cw;
			i += (size_t)n;
			if (x > a->text_x + a->text_w)
				break;
			}
		if (a->caret_on && a->cursor >= start && a->cursor <= end)
			{
			int cx = a->text_x + disp_prefix_px(a, start, a->cursor) - a->xoff;
			if (cx >= a->text_x - 1 && cx < a->text_x + a->text_w)
				disp_fill_rect(a, cx, y + 1, 2, a->line_h - 2, a->col_caret.pixel);
			}
		}

	XftDrawSetClip(a->draw, NULL);

	{
	size_t li_c = a->nlines ? buf_line_of(a, a->cursor) + 1 : 1;
	size_t col = buf_col_at(a, a->cursor) + 1;
	snprintf(status, sizeof status,
	         "Ln %zu, Col %zu    %zu lines    %zu bytes"
	         "          Ctrl+Enter accept    Esc cancel",
	         li_c, col, a->nlines, a->len);
	XftDrawStringUtf8(a->draw, &a->col_muted, a->font, a->text_x, a->status_y + a->ascent,
	                  (FcChar8 *)status, (int)strlen(status));
	}

	menu_bar_draw(a);
	find_bar_draw(a);
	if (a->pipe_active)
		{
		char prompt[560];
		int y = a->status_y + a->line_h + 4;
		snprintf(prompt, sizeof prompt, "Pipe: %s", a->pipe_buf);
		XSetForeground(a->dpy, a->gc, a->col_paper.pixel);
		XFillRectangle(a->dpy, a->pm, a->gc, a->text_x, y, (unsigned)a->text_w, (unsigned)(a->line_h + 4));
		XSetForeground(a->dpy, a->gc, a->col_line.pixel);
		XDrawRectangle(a->dpy, a->pm, a->gc, a->text_x, y, (unsigned)a->text_w, (unsigned)(a->line_h + 4));
		XftDrawStringUtf8(a->draw, &a->col_fg, a->font, a->text_x + 8, y + a->ascent,
		                  (FcChar8 *)prompt, (int)strlen(prompt));
		}

	disp_draw_button(a, a->cancel_r, "Cancel", a->hot_cancel, 0);
	disp_draw_button(a, a->ok_r, "OK", a->hot_ok, 1);
	XCopyArea(a->dpy, a->pm, a->win, a->gc, 0, 0, (unsigned)a->win_w, (unsigned)a->win_h, 0, 0);
}

void disp_open_ui(App *a)
{
	XSetWindowAttributes attr;
	XSizeHints hints;
	XClassHint classh;
	XWMHints wmh;
	unsigned long mask;

	a->dpy = XOpenDisplay(NULL);
	if (!a->dpy)
		die("cannot open display (is DISPLAY set?)", 2);
	(void)XSetIOErrorHandler(x_io_error);
	a->scr = DefaultScreen(a->dpy);
	a->visual = DefaultVisual(a->dpy, a->scr);
	a->cmap = DefaultColormap(a->dpy, a->scr);
	a->font = open_font(a, a->font_name);
	if (!a->font)
		die("cannot open an Xft font", 2);
	a->ascent = a->font->ascent;
	a->line_h = a->font->ascent + a->font->descent + 2;
	{
	XGlyphInfo ext;
	XftTextExtentsUtf8(a->dpy, a->font, (FcChar8 *)"M", 1, &ext);
	a->char_w = ext.xOff > 0 ? ext.xOff : 8;
	}
	if (alloc_color(a, &a->col_fg, "#1c1914") ||
	    alloc_color(a, &a->col_bg, "#efeae2") ||
	    alloc_color(a, &a->col_paper, "#fbf8f2") ||
	    alloc_color(a, &a->col_sel, "#d4e4f5") ||
	    alloc_color(a, &a->col_caret, "#b84324") ||
	    alloc_color(a, &a->col_muted, "#6e675c") ||
	    alloc_color(a, &a->col_okbg, "#245c45") ||
	    alloc_color(a, &a->col_okfg, "#f7f4ee") ||
	    alloc_color(a, &a->col_btn, "#e4ddd0") ||
	    alloc_color(a, &a->col_btnfg, "#1c1914") ||
	    alloc_color(a, &a->col_line, "#cfc6b8"))
		die("cannot allocate colours", 2);

	attr.background_pixel = a->col_bg.pixel;
	attr.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
	                  ButtonReleaseMask | PointerMotionMask | StructureNotifyMask |
	                  FocusChangeMask;
	attr.colormap = a->cmap;
	mask = CWBackPixel | CWEventMask | CWColormap;
	a->win = XCreateWindow(a->dpy, RootWindow(a->dpy, a->scr), 80, 80,
	                       (unsigned)a->win_w, (unsigned)a->win_h, 0,
	                       DefaultDepth(a->dpy, a->scr), InputOutput, a->visual, mask, &attr);
	a->gc = XCreateGC(a->dpy, a->win, 0, NULL);
	a->xa_wm_del = XInternAtom(a->dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(a->dpy, a->win, &a->xa_wm_del, 1);
	a->xa_clip = XInternAtom(a->dpy, "CLIPBOARD", False);
	a->xa_utf8 = XInternAtom(a->dpy, "UTF8_STRING", False);
	a->xa_targets = XInternAtom(a->dpy, "TARGETS", False);
	a->xa_compound = XInternAtom(a->dpy, "COMPOUND_TEXT", False);
	a->xa_paste = XInternAtom(a->dpy, "XCAT_PASTE", False);

	hints.flags = PMinSize | PSize;
	hints.min_width = 420;
	hints.min_height = 220;
	hints.width = a->win_w;
	hints.height = a->win_h;
	XSetWMNormalHints(a->dpy, a->win, &hints);
	classh.res_name = "xcat";
	classh.res_class = "Xcat";
	XSetClassHint(a->dpy, a->win, &classh);
	wmh.flags = InputHint;
	wmh.input = True;
	XSetWMHints(a->dpy, a->win, &wmh);
	XStoreName(a->dpy, a->win, a->title);
	XSetIconName(a->dpy, a->win, a->title);
	{
	XTextProperty tp;
	if (Xutf8TextListToTextProperty(a->dpy, &a->title, 1, XUTF8StringStyle, &tp) == Success)
		{
		XSetWMName(a->dpy, a->win, &tp);
		XFree(tp.value);
		}
	}
	XStoreName(a->dpy, a->win, a->title);
	XSetIconName(a->dpy, a->win, a->title);

	a->im = XOpenIM(a->dpy, NULL, NULL, NULL);
	if (a->im)
		{
		a->ic = XCreateIC(a->im, XNInputStyle,
		                  XIMPreeditNothing | XIMStatusNothing,
		                  XNClientWindow, a->win, XNFocusWindow, a->win, NULL);
		}
	XMapRaised(a->dpy, a->win);
	XFlush(a->dpy);
}   
