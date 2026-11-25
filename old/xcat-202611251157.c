/*************** xcat.c **************/
/*
 * xcat — an X11 text editor for Unix pipelines, in the role of cat.
 *
 * With no file arguments, read standard input. With file arguments,
 * concatenate those files (a "-" reads standard input at that point).
 * Show the result in a window, then either write the edited buffer to
 * standard output (OK) or report failure on the controlling terminal (Cancel).
 *
 * Build:  make
 * Use:    xcat a.txt b.txt | next-command
 *         some-command | xcat | next-command
 *
 * Portable to systems with POSIX and X11 (libX11 + libXft): Linux, the BSDs,
 * Solaris, and macOS via XQuartz. No Linux-specific interfaces.
 *
 * SPDX-License-Identifier: MIT
 */

#define _XOPEN_SOURCE 700

#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <unistd.h>
#include <wchar.h>

#define UNDO_MAX 48
#define TAB_COLS 4
#define APP_NAME "xcat"

#define MENU_H 24
#define MENU_PAD 8

#define FILE_BUFFER_SIZE (4096)

typedef struct
	{
	int x, y, w, h;
	} Rect;

typedef struct
	{
	char *text;
	size_t len;
	size_t cur;
	} Snap;

typedef struct
	{
	const char *label;
	const char **items;
	int nitems;
	} Menu;

static void cancel(void);
static void draw_all(void);
static void append_fd(int fd, const char *what);

static Display *dpy;
static int scr;
static Window win;
static GC gc;
static Pixmap pm;
static int pm_w, pm_h;
static XftDraw *draw;
static XftFont *font;
static Visual *visual;
static Colormap cmap;
static XIM im;
static XIC ic;

static XftColor col_fg, col_bg, col_sel, col_caret, col_muted;
static XftColor col_okbg, col_okfg, col_btn, col_btnfg, col_paper, col_line;

static int win_w = 760, win_h = 520;
static int char_w = 8, line_h = 16, ascent = 12;
static int text_x, text_y, text_w, text_h, status_y;
static Rect ok_r, cancel_r;
static int hot_ok, hot_cancel;

static char *text;
static size_t len, cap, cursor, anchor, top_line, pref_col;
static int selecting;
static int xoff;
static int caret_on = 1;
static int dirty_blink;

static size_t *lines;
static size_t nlines, lines_cap;

static Snap undo_stack[UNDO_MAX];
static int undo_len, undo_pos;

static char *clip;
static size_t clip_len;
static int own_clip;

static Atom xa_wm_del, xa_clip, xa_utf8, xa_targets, xa_compound, xa_paste;
static int paste_phase; /* 0 idle, 1 waiting utf8, 2 waiting string */
static Atom paste_sel;

static char *title = "xcat";
static const char *font_name = "monospace:size=13";
static int exit_kind; /* 0 ok, 1 cancel */

/* ─── menu bar ─── */
static const char *file_items[] = { "Open", "Insert", "Save", "Pipe", "Quit" };
static const char *edit_items[] = { "Cut", "Copy", "Paste" };
static const char *search_items[] = { "Find" };

static Menu menus[] =
	{
		{ "File",   file_items,   5 },
		{ "Edit",   edit_items,   3 },
		{ "Search", search_items, 1 },
	};
#define NMENUS 3

static int ctx_mode;
static int ctx_x, ctx_y;

static int menu_open = -1;
static int menu_hot = -1;
static int menu_drop_x, menu_drop_y, menu_drop_w, menu_drop_h;

/* ─── find bar ─── */
static int find_active;
static char find_buf[256];
static size_t find_len;

static int pipe_active;
static char pipe_buf[512];
static size_t pipe_len;

static char *first_file = NULL;   /* first real filename from argv, for Save default */

static int tty_msg(const char *msg)
	{
	FILE *t = fopen("/dev/tty", "w");
	if (!t)
		return 0;
	fprintf(t, APP_NAME ": %s\n", msg);
	fflush(t);
	fclose(t);
	return 1;
	}

static int x_io_error(Display *d)
	{
	(void)d;
	if (!tty_msg("lost connection to the X server"))
		fprintf(stderr, APP_NAME ": lost connection to the X server\n");
	_exit(2);
	}

static void die(const char *msg, int code)
	{
	if (!tty_msg(msg))
		fprintf(stderr, APP_NAME ": %s\n", msg);
	exit(code);
	}

static void *xmalloc(size_t n)
	{
	void *p = malloc(n ? n : 1);
	if (!p)
		die("out of memory", 2);
	return p;
	}

static void *xrealloc(void *p, size_t n)
	{
	void *q = realloc(p, n ? n : 1);
	if (!q)
		die("out of memory", 2);
	return q;
	}

static void usage(void)
	{
	fprintf(stderr,
	        "usage: " APP_NAME " [-t title] [-fn font] [-g WxH] [file ...]\n"
	        "\n"
	        "Concatenate the named files (or stdin if none are given; \"-\" means\n"
	        "stdin) into an X11 editor. OK writes the buffer to stdout and exits 0.\n"
	        "Cancel reports failure on the controlling terminal and exits 1.\n"
	        "\n"
	        "Keys: arrows, Home/End, PgUp/PgDn, Shift+arrows select,\n"
	        "Ctrl+A select all, Ctrl+C/X/V clipboard, Ctrl+Z undo,\n"
	        "Ctrl+F find, Ctrl+Enter or Ctrl+S accept, Esc cancel.\n");
	}

static int utf8_len(const unsigned char *s, size_t avail)
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

static size_t utf8_prev(size_t pos)
	{
	size_t i;
	if (pos == 0)
		return 0;
	i = pos - 1;
	while (i > 0 && ((unsigned char)text[i] & 0xC0) == 0x80 && pos - i < 4)
		i--;
	if (utf8_len((unsigned char *)text + i, pos - i) == (int)(pos - i))
		return i;
	return pos - 1;
	}

static int cp_width(const unsigned char *s, int n)
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

static void ensure(size_t need)
	{
	if (need <= cap)
		return;
	while (cap < need)
		cap = cap < 64 ? 256 : cap * 2;
	text = xrealloc(text, cap + 1);
	text[len] = 0;
	}

static void rebuild_lines(void)
	{
	size_t i;
	nlines = 0;
	if (lines_cap < 16)
		{
		lines_cap = 16;
		lines = xrealloc(lines, lines_cap * sizeof *lines);
		}
	lines[nlines++] = 0;
	for (i = 0; i < len; i++)
		{
		if (text[i] != '\n')
			continue;
		if (nlines + 1 >= lines_cap)
			{
			lines_cap *= 2;
			lines = xrealloc(lines, lines_cap * sizeof *lines);
			}
		lines[nlines++] = i + 1;
		}
	}

static size_t line_of(size_t pos)
	{
	size_t lo = 0, hi = nlines;
	while (lo + 1 < hi)
		{
		size_t mid = (lo + hi) / 2;
		if (lines[mid] <= pos)
			lo = mid;
		else
			hi = mid;
		}
	return lo;
	}

static size_t line_end(size_t li)
	{
	if (li + 1 < nlines)
		return lines[li + 1] - 1;
	return len;
	}

static size_t col_at(size_t pos)
	{
	size_t li = line_of(pos);
	size_t i = lines[li];
	size_t col = 0;
	while (i < pos && i < len)
		{
		int n = utf8_len((unsigned char *)text + i, len - i);
		if (text[i] == '\n')
			break;
		col += (size_t)cp_width((unsigned char *)text + i, n);
		i += (size_t)n;
		}
	return col;
	}

static size_t pos_at_col(size_t li, size_t want)
	{
	size_t i = lines[li];
	size_t end = line_end(li);
	size_t col = 0;
	while (i < end)
		{
		int n = utf8_len((unsigned char *)text + i, len - i);
		size_t w = (size_t)cp_width((unsigned char *)text + i, n);
		if (col + w > want)
			return i;
		col += w;
		i += (size_t)n;
		}
	return end;
	}

static void clear_sel(void)
	{
	selecting = 0;
	anchor = cursor;
	}

static void sel_range(size_t *a, size_t *b)
	{
	if (!selecting || anchor == cursor)
		{
		*a = *b = cursor;
		return;
		}
	if (anchor < cursor)
		{
		*a = anchor;
		*b = cursor;
		}
	else
		{
		*a = cursor;
		*b = anchor;
		}
	}

static void snap_free(Snap *s)
	{
	free(s->text);
	s->text = NULL;
	s->len = 0;
	}

static void push_undo(void)
	{
	Snap *s;
	if (len > (8u << 20))
		return;
	while (undo_len > undo_pos)
		{
		undo_len--;
		snap_free(&undo_stack[undo_len]);
		}
	if (undo_len == UNDO_MAX)
		{
		snap_free(&undo_stack[0]);
		memmove(undo_stack, undo_stack + 1, (UNDO_MAX - 1) * sizeof(Snap));
		undo_len--;
		undo_pos--;
		}
	s = &undo_stack[undo_len++];
	s->text = xmalloc(len + 1);
	memcpy(s->text, text ? text : "", len);
	s->text[len] = 0;
	s->len = len;
	s->cur = cursor;
	undo_pos = undo_len;
	}

static void restore_snap(const Snap *s)
	{
	ensure(s->len + 1);
	memcpy(text, s->text, s->len);
	text[s->len] = 0;
	len = s->len;
	cursor = s->cur > len ? len : s->cur;
	clear_sel();
	rebuild_lines();
	}

static void undo(void)
	{
	if (undo_pos == 0)
		return;
	if (undo_pos == undo_len)
		{
		push_undo();
		if (undo_pos < 2)
			return;
		undo_pos -= 2;
		}
	else
		{
		undo_pos--;
		}
	restore_snap(&undo_stack[undo_pos]);
	}

static void delete_range(size_t a, size_t b)
	{
	if (a > b)
		{
		size_t t = a;
		a = b;
		b = t;
		}
	if (a > len)
		a = len;
	if (b > len)
		b = len;
	memmove(text + a, text + b, len - b);
	len -= b - a;
	text[len] = 0;
	cursor = a;
	clear_sel();
	rebuild_lines();
	}

static void delete_sel(void)
	{
	size_t a, b;
	sel_range(&a, &b);
	if (a == b)
		return;
	push_undo();
	delete_range(a, b);
	}

static void insert_bytes(const char *s, size_t n)
	{
	if (n == 0)
		return;
	if (selecting && anchor != cursor)
		delete_sel();
	else
		push_undo();
	ensure(len + n + 1);
	memmove(text + cursor + n, text + cursor, len - cursor);
	memcpy(text + cursor, s, n);
	len += n;
	text[len] = 0;
	cursor += n;
	clear_sel();
	rebuild_lines();
	pref_col = col_at(cursor);
	}

static void backspace(void)
	{
	size_t prev;
	if (selecting && anchor != cursor)
		{
		delete_sel();
		return;
		}
	if (cursor == 0)
		return;
	prev = utf8_prev(cursor);
	push_undo();
	memmove(text + prev, text + cursor, len - cursor);
	len -= cursor - prev;
	text[len] = 0;
	cursor = prev;
	clear_sel();
	rebuild_lines();
	pref_col = col_at(cursor);
	}

static void delete_fwd(void)
	{
	int n;
	if (selecting && anchor != cursor)
		{
		delete_sel();
		return;
		}
	if (cursor >= len)
		return;
	n = utf8_len((unsigned char *)text + cursor, len - cursor);
	push_undo();
	memmove(text + cursor, text + cursor + n, len - cursor - (size_t)n);
	len -= (size_t)n;
	text[len] = 0;
	clear_sel();
	rebuild_lines();
	}

static void move_left(int extend)
	{
	size_t n = utf8_prev(cursor);
	if (!extend)
		clear_sel();
	else if (!selecting)
		{
		anchor = cursor;
		selecting = 1;
		}
	cursor = n;
	pref_col = col_at(cursor);
	if (extend && cursor == anchor)
		selecting = 0;
	}

static void move_right(int extend)
	{
	if (cursor < len)
		{
		int n = utf8_len((unsigned char *)text + cursor, len - cursor);
		if (!extend)
			clear_sel();
		else if (!selecting)
			{
			anchor = cursor;
			selecting = 1;
			}
		cursor += (size_t)n;
		if (cursor > len)
			cursor = len;
		}
	else if (!extend)
		{
		clear_sel();
		}
	pref_col = col_at(cursor);
	if (extend && cursor == anchor)
		selecting = 0;
	}

static void move_vert(int dir, int extend)
	{
	size_t li = line_of(cursor);
	if (!extend)
		clear_sel();
	else if (!selecting)
		{
		anchor = cursor;
		selecting = 1;
		}
	if (dir < 0 && li > 0)
		li--;
	else if (dir > 0 && li + 1 < nlines)
		li++;
	cursor = pos_at_col(li, pref_col);
	if (extend && cursor == anchor)
		selecting = 0;
	}

static void move_home(int extend, int doc)
	{
	if (!extend)
		clear_sel();
	else if (!selecting)
		{
		anchor = cursor;
		selecting = 1;
		}
	cursor = doc ? 0 : lines[line_of(cursor)];
	pref_col = col_at(cursor);
	}

static void move_end(int extend, int doc)
	{
	if (!extend)
		clear_sel();
	else if (!selecting)
		{
		anchor = cursor;
		selecting = 1;
		}
	cursor = doc ? len : line_end(line_of(cursor));
	pref_col = col_at(cursor);
	}

static void select_all(void)
	{
	anchor = 0;
	cursor = len;
	selecting = anchor != cursor;
	pref_col = col_at(cursor);
	}

static void copy_sel(void)
	{
	size_t a, b;
	sel_range(&a, &b);
	if (a == b)
		return;
	free(clip);
	clip_len = b - a;
	clip = xmalloc(clip_len + 1);
	memcpy(clip, text + a, clip_len);
	clip[clip_len] = 0;
	XSetSelectionOwner(dpy, xa_clip, win, CurrentTime);
	XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
	own_clip = 1;
	}

static void cut_sel(void)
	{
	size_t a, b;
	sel_range(&a, &b);
	if (a == b)
		return;
	copy_sel();
	delete_sel();
	}

static void request_paste(Atom sel)
	{
	paste_sel = sel;
	paste_phase = 1;
	XConvertSelection(dpy, sel, xa_utf8, xa_paste, win, CurrentTime);
	}

static void kill_line(void)
	{
	size_t end = line_end(line_of(cursor));
	size_t b = end;
	if (cursor == end && end < len && text[end] == '\n')
		b = end + 1;
	if (b == cursor)
		return;
	push_undo();
	delete_range(cursor, b);
	}

static int parse_geom(const char *s)
	{
	int w, h;
	if (sscanf(s, "%dx%d", &w, &h) != 2 || w < 200 || h < 160)
		return -1;
	win_w = w;
	win_h = h;
	return 0;
	}

static int alloc_color(XftColor *c, const char *name)
	{
	if (!XftColorAllocName(dpy, visual, cmap, name, c))
		return -1;
	return 0;
	}

static XftFont *open_font(const char *prefer)
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
	if (prefer && (f = XftFontOpenName(dpy, scr, prefer)))
		return f;
	for (i = 0; fallbacks[i]; i++)
		{
		f = XftFontOpenName(dpy, scr, fallbacks[i]);
		if (f)
			return f;
		}
	return NULL;
	}

static void layout(void)
	{
	int pad = 12;
	int btn_h = 30;
	int btn_w = 108;
	int gap = 8;
	int find_h = (find_active || pipe_active) ? (line_h + 8) : 0;

	status_y = win_h - pad - btn_h - gap - line_h - 6 - find_h;
	text_x = pad;
	text_y = MENU_H + pad;
	text_w = win_w - pad * 2;
	text_h = status_y - gap - text_y;
	if (text_h < line_h)
		text_h = line_h;
	cancel_r.w = btn_w;
	cancel_r.h = btn_h;
	cancel_r.x = win_w - pad - btn_w - gap - btn_w;
	cancel_r.y = win_h - pad - btn_h;
	ok_r.w = btn_w;
	ok_r.h = btn_h;
	ok_r.x = win_w - pad - btn_w;
	ok_r.y = cancel_r.y;
	}

static void ensure_pixmap(void)
	{
	if (pm && pm_w == win_w && pm_h == win_h)
		return;
	if (draw)
		XftDrawDestroy(draw);
	if (pm)
		XFreePixmap(dpy, pm);
	pm = XCreatePixmap(dpy, win, (unsigned)win_w, (unsigned)win_h,
	                   (unsigned)DefaultDepth(dpy, scr));
	pm_w = win_w;
	pm_h = win_h;
	draw = XftDrawCreate(dpy, pm, visual, cmap);
	}

static int prefix_px(size_t line_start, size_t pos)
	{
	size_t i = line_start;
	int px = 0;
	while (i < pos && i < len && text[i] != '\n')
		{
		int n = utf8_len((unsigned char *)text + i, len - i);
		int cols = cp_width((unsigned char *)text + i, n);
		px += cols * char_w;
		i += (size_t)n;
		}
	return px;
	}

static size_t pos_from_px(size_t li, int px)
	{
	size_t i = lines[li];
	size_t end = line_end(li);
	int acc = 0;
	if (px < 0)
		return i;
	while (i < end)
		{
		int n = utf8_len((unsigned char *)text + i, len - i);
		int cols = cp_width((unsigned char *)text + i, n);
		int w = cols * char_w;
		if (acc + w / 2 >= px)
			return i;
		acc += w;
		i += (size_t)n;
		}
	return end;
	}

static void reveal_cursor(void)
	{
	size_t li = line_of(cursor);
	size_t vis = (size_t)(text_h / line_h);
	int px;
	if (text_w <= 0 || text_h <= 0)
		return;
	if (vis == 0)
		vis = 1;
	if (li < top_line)
		top_line = li;
	if (li >= top_line + vis)
		top_line = li + 1 - vis;
	px = prefix_px(lines[li], cursor) - xoff;
	if (px < 0)
		xoff = prefix_px(lines[li], cursor);
	if (px > text_w - char_w)
		xoff = prefix_px(lines[li], cursor) - (text_w - char_w);
	if (xoff < 0)
		xoff = 0;
	}

static void fill_rect(int x, int y, int w, int h, unsigned long pixel)
	{
	if (w <= 0 || h <= 0)
		return;
	XSetForeground(dpy, gc, pixel);
	XFillRectangle(dpy, pm, gc, x, y, (unsigned)w, (unsigned)h);
	}

static void stroke_rect(int x, int y, int w, int h, unsigned long pixel)
	{
	XSetForeground(dpy, gc, pixel);
	XDrawRectangle(dpy, pm, gc, x, y, (unsigned)(w - 1), (unsigned)(h - 1));
	}

static void draw_button(Rect r, const char *label, int hot, int primary)
	{
	XftColor *bg = primary ? &col_okbg : &col_btn;
	XftColor *fg = primary ? &col_okfg : &col_btnfg;
	XGlyphInfo ext;
	int tw, tx, ty;
	fill_rect(r.x, r.y, r.w, r.h, bg->pixel);
	if (hot)
		stroke_rect(r.x, r.y, r.w, r.h, col_caret.pixel);
	else
		stroke_rect(r.x, r.y, r.w, r.h, col_muted.pixel);
	XftTextExtentsUtf8(dpy, font, (FcChar8 *)label, (int)strlen(label), &ext);
	tw = ext.xOff;
	tx = r.x + (r.w - tw) / 2;
	ty = r.y + (r.h + ascent - 2) / 2;
	XftDrawStringUtf8(draw, fg, font, tx, ty, (FcChar8 *)label, (int)strlen(label));
	}

/* ─── menu drawing ─── */

static int menu_title_x(int i)
	{
	int x = MENU_PAD;
	int j;
	for (j = 0; j < i; j++)
		{
		XGlyphInfo ext;
		XftTextExtentsUtf8(dpy, font, (FcChar8 *)menus[j].label,
		                   (int)strlen(menus[j].label), &ext);
		x += ext.xOff + 24;
		}
	return x;
	}

static void menu_dropdown_geom(void)
	{
	int i, wmax = 0;
	XGlyphInfo ext;
	if (menu_open < 0)
		return;
	if (ctx_mode)
		{
		menu_drop_x = ctx_x;
		menu_drop_y = ctx_y;
		}
	else
		{
		menu_drop_x = menu_title_x(menu_open) - 2;
		menu_drop_y = MENU_H + 6;
		}
	for (i = 0; i < menus[menu_open].nitems; i++)
		{
		XftTextExtentsUtf8(dpy, font, (FcChar8 *)menus[menu_open].items[i],
		                   (int)strlen(menus[menu_open].items[i]), &ext);
		if (ext.xOff + 32 > wmax)
			wmax = ext.xOff + 32;
		}
	menu_drop_w = wmax;
	menu_drop_h = menus[menu_open].nitems * line_h + 6;
	}

static void menu_bar_draw(void)
	{
	int i, x;
	fill_rect(0, 0, win_w, MENU_H, col_paper.pixel);
	fill_rect(0, MENU_H - 1, win_w, 1, col_line.pixel);

	for (i = 0; i < NMENUS; i++)
		{
		XGlyphInfo ext;
		int tx;
		x = menu_title_x(i);
		XftTextExtentsUtf8(dpy, font, (FcChar8 *)menus[i].label,
		                   (int)strlen(menus[i].label), &ext);
		if (i == menu_open && !ctx_mode)
			fill_rect(x - 4, 0, ext.xOff + 8, MENU_H, col_sel.pixel);
		tx = x;
		XftDrawStringUtf8(draw, &col_fg, font, tx, MENU_H - (MENU_H - ascent) / 2,
		                  (FcChar8 *)menus[i].label, (int)strlen(menus[i].label));
		}

	if (menu_open >= 0)
		{
		menu_dropdown_geom();
		fill_rect(menu_drop_x, menu_drop_y, menu_drop_w, menu_drop_h, col_paper.pixel);
		stroke_rect(menu_drop_x, menu_drop_y, menu_drop_w, menu_drop_h, col_line.pixel);
		for (i = 0; i < menus[menu_open].nitems; i++)
			{
			int iy = menu_drop_y + 7 + i * line_h;
			if (i == menu_hot)
				fill_rect(menu_drop_x + 1, iy, menu_drop_w - 2, line_h, col_sel.pixel);
			XftDrawStringUtf8(draw, &col_fg, font,
			                  menu_drop_x + 12, iy + ascent,
			                  (FcChar8 *)menus[menu_open].items[i],
			                  (int)strlen(menus[menu_open].items[i]));
			}
		}
	}

/* ─── find bar ─── */

static void find_bar_draw(void)
	{
	int fy;
	char prompt[320];
	if (!find_active)
		return;
	fy = status_y - line_h - 8;
	fill_rect(text_x, fy, text_w, line_h + 4, col_paper.pixel);
	stroke_rect(text_x, fy, text_w, line_h + 4, col_line.pixel);
	snprintf(prompt, sizeof prompt, "Find: %s", find_buf);
	XftDrawStringUtf8(draw, &col_fg, font, text_x + 8, fy + ascent + 2,
	                  (FcChar8 *)prompt, (int)strlen(prompt));
	if (caret_on)
		{
		int cx = text_x + 8 + (int)strlen("Find: ") * char_w + (int)find_len * char_w;
		fill_rect(cx, fy + 3, 1, line_h, col_caret.pixel);
		}
	}

static void find_next(void)
	{
	size_t start, i;
	size_t flen = find_len;
	if (flen == 0)
		return;
	start = cursor + 1;
	if (start >= len)
		start = 0;
	for (i = start; i + flen <= len; i++)
		{
		if (memcmp(text + i, find_buf, flen) == 0)
			{
			anchor = i;
			cursor = i + flen;
			selecting = 1;
			reveal_cursor();
			return;
			}
		}
	for (i = 0; i < start && i + flen <= len; i++)
		{
		if (memcmp(text + i, find_buf, flen) == 0)
			{
			anchor = i;
			cursor = i + flen;
			selecting = 1;
			reveal_cursor();
			return;
			}
		}
	}

static void find_open(void)
	{
	find_active = 1;
	find_len = 0;
	find_buf[0] = 0;
	}

static void find_close(void)
	{
	find_active = 0;
	find_buf[0] = 0;
	find_len = 0;
	}
/* ─── file picker ─── */

#define FP_MAX_ENTRIES 512
#define FP_ENTRY_LEN   256

typedef struct
	{
	Window win;
	GC gc;
	Pixmap pm;
	int pm_w, pm_h;
	XftDraw *draw;
	int w, h;
	char path[4096];
	char entries[FP_MAX_ENTRIES][FP_ENTRY_LEN];
	int entry_is_dir[FP_MAX_ENTRIES];
	int nentries;
	int sel;
	int top;
	int active;
	int replace;
	int save_mode;
	char name_buf[512];
	size_t name_len;
	} FilePicker;

static FilePicker fp;

static void fp_load_dir(void)
	{
	DIR *d;
	struct dirent *de;
	size_t plen;
	int i;

	fp.nentries = 0;
	fp.sel = 0;
	fp.top = 0;

	d = opendir(fp.path);
	if (!d)
		return;

	/* first entry: "<..>" if not root */
	plen = strlen(fp.path);
	if (plen > 1)
		{
		strcpy(fp.entries[fp.nentries], "<..>");
		fp.entry_is_dir[fp.nentries] = 1;
		fp.nentries++;
		}

	while ((de = readdir(d)) && fp.nentries < FP_MAX_ENTRIES)
		{
		char full[FILE_BUFFER_SIZE];
		struct stat st;
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		snprintf(full, sizeof full, "%s/%s", fp.path, de->d_name);
		if (stat(full, &st) != 0)
			continue;
		snprintf(fp.entries[fp.nentries], FP_ENTRY_LEN, "%s", de->d_name);
		fp.entry_is_dir[fp.nentries] = S_ISDIR(st.st_mode);
		fp.nentries++;
		}
	closedir(d);

	/* sort: directories first, then files, both case-insensitive */
	for (i = 1; i < fp.nentries; i++)
		{
		char tmp[FP_ENTRY_LEN];
		int tmpd;
		int j = i;
		while (j > 0)
			{
			int a_is_dir = fp.entry_is_dir[j];
			int b_is_dir = fp.entry_is_dir[j - 1];
			int less = 0;
			if (b_is_dir && !a_is_dir)
				less = 1;
			else if (!b_is_dir && a_is_dir)
				less = 0;
			else if (a_is_dir == b_is_dir)
				less = strcasecmp(fp.entries[j], fp.entries[j - 1]) < 0;
			if (!less)
				break;
			snprintf(tmp, FP_ENTRY_LEN, "%s", fp.entries[j - 1]);
			tmpd = fp.entry_is_dir[j - 1];
			snprintf(fp.entries[j - 1], FP_ENTRY_LEN, "%s", fp.entries[j]);
			fp.entry_is_dir[j - 1] = fp.entry_is_dir[j];
			snprintf(fp.entries[j], FP_ENTRY_LEN, "%s", tmp);
			fp.entry_is_dir[j] = tmpd;
			j--;
			}
		}
	}

static void fp_ensure_pixmap(void)
	{
	if (fp.pm && fp.pm_w == fp.w && fp.pm_h == fp.h)
		return;
	if (fp.draw)
		XftDrawDestroy(fp.draw);
	if (fp.pm)
		XFreePixmap(dpy, fp.pm);
	fp.pm = XCreatePixmap(dpy, fp.win, (unsigned)fp.w, (unsigned)fp.h,
	                      (unsigned)DefaultDepth(dpy, scr));
	fp.pm_w = fp.w;
	fp.pm_h = fp.h;
	fp.draw = XftDrawCreate(dpy, fp.pm, visual, cmap);
	}
static void fp_fill(int x, int y, int w, int h, unsigned long pixel)
	{
	if (w <= 0 || h <= 0)
		return;
	XSetForeground(dpy, fp.gc, pixel);
	XFillRectangle(dpy, fp.pm, fp.gc, x, y, (unsigned)w, (unsigned)h);
	}

static void fp_stroke(int x, int y, int w, int h, unsigned long pixel)
	{
	XSetForeground(dpy, fp.gc, pixel);
	XDrawRectangle(dpy, fp.pm, fp.gc, x, y, (unsigned)(w - 1), (unsigned)(h - 1));
	}

static void fp_draw(void)
	{
	int pad = 8;
	int lh = line_h;
	int list_y = pad + lh + 4;
	int name_h = fp.save_mode ? (lh + 4) : 0;
	int list_h = fp.h - list_y - pad - lh - 4 - name_h;
	int vis = list_h / lh;
	int i;
	char status[256];
	int list_x = pad;
	int list_w = fp.w - pad * 2;

	fp_ensure_pixmap();
	fp_fill(0, 0, fp.w, fp.h, col_bg.pixel);
	fp_fill(0, 0, fp.w, fp.h, col_paper.pixel);
	fp_stroke(0, 0, fp.w, fp.h, col_line.pixel);

	/* path bar */
		{
		char pbar[4200];
		snprintf(pbar, sizeof pbar, "%s", fp.path);
		fp_fill(pad, pad, list_w, lh, col_bg.pixel);
		fp_stroke(pad, pad, list_w, lh, col_line.pixel);
		XftDrawStringUtf8(fp.draw, &col_fg, font, pad + 6, pad + ascent,
		                  (FcChar8 *)pbar, (int)strlen(pbar));
		}

	/* entry list */
	for (i = 0; i < vis && i + fp.top < fp.nentries; i++)
		{
		int idx = fp.top + i;
		int ey = list_y + i * lh;
		const char *name = fp.entries[idx];
		char disp[FP_ENTRY_LEN + 2];
		if (fp.entry_is_dir[idx])
			snprintf(disp, sizeof disp, "%s/", name);
		else
			snprintf(disp, sizeof disp, "%s", name);

		if (idx == fp.sel)
			fp_fill(list_x, ey, list_w, lh, col_sel.pixel);

		XftDrawStringUtf8(fp.draw, &col_fg, font, list_x + 6, ey + ascent,
		                  (FcChar8 *)disp, (int)strlen(disp));
		}

	/* filename entry for Save */
	if (fp.save_mode)
		{
		int ny = list_y + list_h + 2;
		char label[600];
		snprintf(label, sizeof label, "Name: %s", fp.name_buf);
		fp_fill(pad, ny, list_w, lh, col_bg.pixel);
		fp_stroke(pad, ny, list_w, lh, col_line.pixel);
		XftDrawStringUtf8(fp.draw, &col_fg, font, pad + 6, ny + ascent,
		                  (FcChar8 *)label, (int)strlen(label));
		}

	/* status bar */
	snprintf(status, sizeof status,
	         fp.save_mode ?
	         "Enter: save    Esc: cancel" :
	         "%d entries    Enter: open    Backspace: up    Esc: cancel",
	         fp.nentries);
	XftDrawStringUtf8(fp.draw, &col_muted, font, pad, fp.h - pad - 2,
	                  (FcChar8 *)status, (int)strlen(status));

	XCopyArea(dpy, fp.pm, fp.win, fp.gc, 0, 0,
	          (unsigned)fp.w, (unsigned)fp.h, 0, 0);
	}

static void fp_close(void)
	{
	if (fp.draw)
		XftDrawDestroy(fp.draw);
	if (fp.pm)
		XFreePixmap(dpy, fp.pm);
	if (fp.win)
		XDestroyWindow(dpy, fp.win);
	fp.win = 0;
	fp.pm = 0;
	fp.draw = NULL;
	fp.active = 0;
	}

static void fp_open(void)
	{
	XSetWindowAttributes attr;
	XWMHints wmh;
	XSizeHints hints;
	unsigned long mask;
	XWindowAttributes wa;

	fp.w = 520;
	fp.h = 380;

	/* center over main window */
	XGetWindowAttributes(dpy, win, &wa);
	fp.w = fp.w > wa.width ? wa.width : fp.w;
	fp.h = fp.h > wa.height ? wa.height : fp.h;
		{
		int x = wa.x + (wa.width - fp.w) / 2;
		int y = wa.y + (wa.height - fp.h) / 2;
		if (x < 0) x = 0;
		if (y < 0) y = 0;

		memset(&attr, 0, sizeof attr);
		attr.background_pixel = col_paper.pixel;
		attr.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
		                  StructureNotifyMask;
		mask = CWBackPixel | CWEventMask;
		fp.win = XCreateWindow(dpy, RootWindow(dpy, scr),
		                       (unsigned)x, (unsigned)y,
		                       (unsigned)fp.w, (unsigned)fp.h, 0,
		                       DefaultDepth(dpy, scr), InputOutput,
		                       visual, mask, &attr);
		fp.gc = XCreateGC(dpy, fp.win, 0, NULL);
		XSetTransientForHint(dpy, fp.win, win);
		hints.flags = PMinSize;
		hints.min_width = 320;
		hints.min_height = 200;
		XSetWMNormalHints(dpy, fp.win, &hints);
		wmh.flags = InputHint;
		wmh.input = True;
		XSetWMHints(dpy, fp.win, &wmh);
		XStoreName(dpy, fp.win, fp.save_mode ? "Save File" :
		            (fp.replace ? "Open File" : "Insert File"));
		XSetWMProtocols(dpy, fp.win, &xa_wm_del, 1);

		/* start in cwd, or in the directory of the first filename for Save */
		if (!getcwd(fp.path, sizeof fp.path))
			snprintf(fp.path, sizeof fp.path, "/");
		fp.name_len = 0;
		fp.name_buf[0] = 0;
		if (fp.save_mode && first_file)
			{
			const char *base = first_file;
			char *slash = strrchr(first_file, '/');
			if (slash)
				{
				size_t dlen = (size_t)(slash - first_file);
				if (dlen > 0 && dlen < sizeof fp.path)
					{
					memcpy(fp.path, first_file, dlen);
					fp.path[dlen] = 0;
					}
				base = slash + 1;
				}
			snprintf(fp.name_buf, sizeof fp.name_buf, "%s", base);
			fp.name_len = strlen(fp.name_buf);
			}
		fp_load_dir();
		fp.active = 1;
		XMapRaised(dpy, fp.win);
		XFlush(dpy);
		}
	}

static void fp_handle_key(XKeyEvent *ev)
	{
	KeySym ks = 0;
	char buf[64];
	int n = XLookupString(ev, buf, (int)sizeof buf - 1, &ks, NULL);
	(void)n;

	if (ks == XK_Escape)
		{
		fp_close();
		draw_all();
		return;
		}
	/* in Save mode, printable keys edit the filename field */
	if (fp.save_mode && ks != XK_Return && ks != XK_KP_Enter &&
	    ks != XK_Up && ks != XK_Down && ks != XK_KP_Up && ks != XK_KP_Down &&
	    ks != XK_Page_Up && ks != XK_Page_Down && ks != XK_BackSpace)
		{
		if (n > 0 && (unsigned char)buf[0] >= 0x20 &&
		    fp.name_len + (size_t)n < sizeof fp.name_buf - 1)
			{
			memcpy(fp.name_buf + fp.name_len, buf, (size_t)n);
			fp.name_len += (size_t)n;
			fp.name_buf[fp.name_len] = 0;
			fp_draw();
			return;
			}
		}
	if (fp.save_mode && ks == XK_BackSpace && fp.name_len > 0)
		{
		fp.name_buf[--fp.name_len] = 0;
		fp_draw();
		return;
		}
	if (ks == XK_Return || ks == XK_KP_Enter)
		{
		/* select current entry */
		if (fp.save_mode && fp.name_len > 0)
			{
			/* typed name takes precedence: save to current dir / name */
			char dest[FILE_BUFFER_SIZE];
			snprintf(dest, sizeof dest, "%s/%s", fp.path, fp.name_buf);
			int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd >= 0)
				{
				size_t off = 0;
				while (off < len)
					{
					ssize_t n = write(fd, text + off, len - off);
					if (n < 0)
						{
						if (errno == EINTR) continue;
						tty_msg(strerror(errno));
						break;
						}
					off += (size_t)n;
					}
				close(fd);
				if (off == len)
					tty_msg("saved");
				}
			else
				tty_msg(strerror(errno));
			fp_close();
			draw_all();
			return;
			}
		if (fp.sel >= 0 && fp.sel < fp.nentries)
			{
			char full[FILE_BUFFER_SIZE];
			if (strcmp(fp.entries[fp.sel], "<..>") == 0)
				{
				/* go up */
				size_t plen = strlen(fp.path);
				if (plen > 1)
					{
					while (plen > 1 && fp.path[plen - 1] == '/')
						fp.path[--plen] = 0;
					while (plen > 1 && fp.path[plen - 1] != '/')
						plen--;
					fp.path[plen] = 0;
					}
				fp_load_dir();
				}
			else if (fp.entry_is_dir[fp.sel])
				{
				snprintf(full, sizeof full, "%s/%s", fp.path, fp.entries[fp.sel]);
				snprintf(fp.path, sizeof fp.path, "%s", full);
				fp_load_dir();
				}
			else if (fp.save_mode)
				{
				/* write buffer to the named file (or selected entry) */
				char dest[FILE_BUFFER_SIZE];
				const char *use = fp.name_len ? fp.name_buf : fp.entries[fp.sel];
				snprintf(dest, sizeof dest, "%s/%s", fp.path, use);
					{
					int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
					if (fd >= 0)
						{
						size_t off = 0;
						while (off < len)
							{
							ssize_t n = write(fd, text + off, len - off);
							if (n < 0)
								{
								if (errno == EINTR) continue;
								tty_msg(strerror(errno));
								break;
								}
							off += (size_t)n;
							}
						close(fd);
						if (off == len)
							tty_msg("saved");
						}
					else
						tty_msg(strerror(errno));
					}
				fp_close();
				draw_all();
				return;
				}
			else
				{
				snprintf(full, sizeof full, "%s/%s", fp.path, fp.entries[fp.sel]);
					{
					int fd = open(full, O_RDONLY);
					if (fd >= 0)
						{
						size_t save_cursor = cursor;
						size_t tail_len = len - cursor;
						char *tail;

						push_undo();
						if (fp.replace)
							{
							len = 0;
							text[0] = 0;
							cursor = 0;
							save_cursor = 0;
							tail_len = 0;
							}

						tail = xmalloc(tail_len + 1);
						memcpy(tail, text + save_cursor, tail_len);
						tail[tail_len] = 0;

						len = save_cursor;
						text[len] = 0;

						append_fd(fd, full);
						close(fd);

						if (tail_len > 0)
							{
							ensure(len + tail_len + 1);
							memcpy(text + len, tail, tail_len);
							len += tail_len;
							text[len] = 0;
							}
						free(tail);

						rebuild_lines();
						cursor = len - tail_len;
						clear_sel();
						reveal_cursor();
						}
					else
						{
						tty_msg(strerror(errno));
						}
					}
				fp_close();
				draw_all();
				return;
				}
			}
		fp_draw();
		return;
		}
	if (ks == XK_BackSpace)
		{
		/* go up one level */
		size_t plen = strlen(fp.path);
		if (plen > 1)
			{
			while (plen > 1 && fp.path[plen - 1] == '/')
				fp.path[--plen] = 0;
			while (plen > 1 && fp.path[plen - 1] != '/')
				plen--;
			fp.path[plen] = 0;
			}
		fp_load_dir();
		fp_draw();
		return;
		}
	if (ks == XK_Up || ks == XK_KP_Up)
		{
		if (fp.sel > 0)
			fp.sel--;
		if (fp.sel < fp.top)
			fp.top = fp.sel;
		fp_draw();
		return;
		}
	if (ks == XK_Down || ks == XK_KP_Down)
		{
		if (fp.sel + 1 < fp.nentries)
			fp.sel++;
			{
			int vis = (fp.h - 8 - line_h - 4 - 8 - line_h - 4) / line_h;
			if (vis < 1) vis = 1;
			if (fp.sel >= fp.top + vis)
				fp.top = fp.sel - vis + 1;
			}
		fp_draw();
		return;
		}
	if (ks == XK_Page_Up)
		{
		int vis = (fp.h - 8 - line_h - 4 - 8 - line_h - 4) / line_h;
		if (vis < 1) vis = 1;
		fp.sel -= vis;
		if (fp.sel < 0) fp.sel = 0;
		fp.top = fp.sel < fp.top ? fp.sel : fp.top - vis;
		if (fp.top < 0) fp.top = 0;
		fp_draw();
		return;
		}
	if (ks == XK_Page_Down)
		{
		int vis = (fp.h - 8 - line_h - 4 - 8 - line_h - 4) / line_h;
		if (vis < 1) vis = 1;
		fp.sel += vis;
		if (fp.sel >= fp.nentries) fp.sel = fp.nentries - 1;
		if (fp.sel >= fp.top + vis)
			fp.top = fp.sel - vis + 1;
		fp_draw();
		return;
		}
	}

static void fp_handle_button(XButtonEvent *ev)
	{
	int pad = 8;
	int lh = line_h;
	int list_y = pad + lh + 4;
	int name_h = fp.save_mode ? (lh + 4) : 0;
	int list_h = fp.h - list_y - pad - lh - 4 - name_h;
	int vis = list_h / lh;

	if (ev->button != Button1)
		return;

	/* click in list area */
	if (ev->y >= list_y && ev->y < list_y + vis * lh)
		{
		int idx = fp.top + (ev->y - list_y) / lh;
		if (idx >= 0 && idx < fp.nentries)
			{
			fp.sel = idx;
			fp_draw();
			return;
			}
		}

	/* double-click equivalent: single click on a file selects it,
	   but we require Enter to confirm. For convenience, if clicking
	   directly on a file entry, insert it immediately. */
	/* (kept simple — just highlight) */
	}

/* ─── end file picker ─── */

/* ─── file operations (stubs) ─── */

static void file_open(void)
	{
	fp.replace = 1;
	fp.save_mode = 0;
	fp_open();
	}

static void file_insert(void)
	{
	fp.replace = 0;
	fp.save_mode = 0;
	fp_open();
	}

static void file_save(void)
	{
	fp.replace = 0;
	fp.save_mode = 1;
	fp_open();
	}

static void pipe_open(void)
	{
	pipe_active = 1;
	pipe_len = 0;
	pipe_buf[0] = 0;
	layout();
	draw_all();
	}

static void pipe_close(void)
	{
	pipe_active = 0;
	pipe_buf[0] = 0;
	pipe_len = 0;
	layout();
	}

static void pipe_run(void)
	{
	char tmp[] = "/tmp/xcatXXXXXX";
	int tfd;
	FILE *p;
	char cmd[600];
	char *out;
	size_t out_len, out_cap;
	char buf[8192];
	ssize_t n;

	if (pipe_len == 0)
		{
		pipe_close();
		return;
		}
	tfd = mkstemp(tmp);
	if (tfd < 0)
		{
		tty_msg("cannot create temp file");
		pipe_close();
		return;
		}
		{
		size_t off = 0;
		while (off < len)
			{
			ssize_t w = write(tfd, text + off, len - off);
			if (w < 0)
				{
				if (errno == EINTR) continue;
				close(tfd);
				unlink(tmp);
				tty_msg(strerror(errno));
				pipe_close();
				return;
				}
			off += (size_t)w;
			}
		}
	close(tfd);

	snprintf(cmd, sizeof cmd, "%s < %s", pipe_buf, tmp);
	p = popen(cmd, "r");
	if (!p)
		{
		unlink(tmp);
		tty_msg("cannot run command");
		pipe_close();
		return;
		}
	out = NULL;
	out_len = 0;
	out_cap = 0;
	while ((n = fread(buf, 1, sizeof buf, p)) > 0)
		{
		if (out_len + (size_t)n + 1 > out_cap)
			{
			out_cap = out_cap ? out_cap * 2 : 4096;
			while (out_cap < out_len + (size_t)n + 1)
				out_cap *= 2;
			out = xrealloc(out, out_cap);
			}
		memcpy(out + out_len, buf, (size_t)n);
		out_len += (size_t)n;
		}
	pclose(p);
	unlink(tmp);

	push_undo();
	ensure(out_len + 1);
	if (out)
		memcpy(text, out, out_len);
	text[out_len] = 0;
	len = out_len;
	cursor = 0;
	clear_sel();
	rebuild_lines();
	free(out);
	pipe_close();
	reveal_cursor();
	}

static void file_quit(void)
	{
	cancel();
	}

static void menu_execute(int mi, int item)
	{
	switch (mi)
		{
		case 0: /* File */
			switch (item)
				{
				case 0:
					file_open();
					break;
				case 1:
					file_insert();
					break;
				case 2:
					file_save();
					break;
				case 3:
					pipe_open();
					break;
				case 4:
					file_quit();
					break;
				}
			break;
		case 1: /* Edit */
			switch (item)
				{
				case 0:
					cut_sel();
					reveal_cursor();
					break;
				case 1:
					copy_sel();
					break;
				case 2:
					request_paste(xa_clip);
					break;
				}
			break;
		case 2: /* Search */
			switch (item)
				{
				case 0:
					find_open();
					break;
				}
			break;
		}
	ctx_mode = 0;
	menu_open = -1;
	menu_hot = -1;
	}

static void draw_all(void)
	{
	size_t vis, li;
	int y;
	char status[160];
	size_t sel_a, sel_b;
	XRectangle clip;

	ensure_pixmap();
	layout();
	fill_rect(0, 0, win_w, win_h, col_bg.pixel);
	fill_rect(text_x - 4, text_y - 4, text_w + 8, text_h + 8, col_paper.pixel);
	stroke_rect(text_x - 4, text_y - 4, text_w + 8, text_h + 8, col_line.pixel);

	/*	menu_bar_draw(); */

	clip.x = (short)text_x;
	clip.y = (short)text_y;
	clip.width = (unsigned short)text_w;
	clip.height = (unsigned short)text_h;
	XftDrawSetClipRectangles(draw, 0, 0, &clip, 1);

	vis = (size_t)(text_h / line_h);
	if (vis == 0)
		vis = 1;
	sel_range(&sel_a, &sel_b);
	for (li = top_line, y = text_y; li < nlines && li < top_line + vis; li++, y += line_h)
		{
		size_t start = lines[li];
		size_t end = line_end(li);
		int x = text_x - xoff;
		size_t i = start;
		if (sel_a != sel_b && sel_b > start && sel_a < end)
			{
			size_t sa = sel_a > start ? sel_a : start;
			size_t sb = sel_b < end ? sel_b : end;
			int x1 = text_x + prefix_px(start, sa) - xoff;
			int x2 = text_x + prefix_px(start, sb) - xoff;
			if (x2 > x1)
				fill_rect(x1, y, x2 - x1, line_h, col_sel.pixel);
			}
		while (i < end)
			{
			int n = utf8_len((unsigned char *)text + i, len - i);
			int cols = cp_width((unsigned char *)text + i, n);
			int cw = cols * char_w;
			if (x + cw >= text_x && x < text_x + text_w)
				{
				char tmp[8];
				const char *drawp = text + i;
				int dn = n;
				if (n == 1 && (unsigned char)text[i] < 0x20 && text[i] != '\t')
					{
					tmp[0] = (char)0xC2;
					tmp[1] = (char)0xB7;
					drawp = tmp;
					dn = 2;
					}
				else if (text[i] == '\t')
					{
					drawp = NULL;
					}
				if (drawp)
					XftDrawStringUtf8(draw, &col_fg, font, x, y + ascent,
					                  (FcChar8 *)drawp, dn);
				}
			x += cw;
			i += (size_t)n;
			if (x > text_x + text_w)
				break;
			}
		if (caret_on && cursor >= start && cursor <= end)
			{
			int cx = text_x + prefix_px(start, cursor) - xoff;
			if (cx >= text_x - 1 && cx < text_x + text_w)
				fill_rect(cx, y + 1, 2, line_h - 2, col_caret.pixel);
			}
		}

	XftDrawSetClip(draw, NULL);

		{
		size_t li_c = nlines ? line_of(cursor) + 1 : 1;
		size_t col = col_at(cursor) + 1;
		snprintf(status, sizeof status,
		         "Ln %zu, Col %zu    %zu lines    %zu bytes"
		         "          Ctrl+Enter accept    Esc cancel",
		         li_c, col, nlines, len);
		XftDrawStringUtf8(draw, &col_muted, font, text_x, status_y + ascent,
		                  (FcChar8 *)status, (int)strlen(status));
		}

	menu_bar_draw();
	find_bar_draw();
	if (pipe_active)
		{
		char prompt[560];
		int y = status_y + line_h + 4;
		snprintf(prompt, sizeof prompt, "Pipe: %s", pipe_buf);
		XSetForeground(dpy, gc, col_paper.pixel);
		XFillRectangle(dpy, pm, gc, text_x, y, (unsigned)text_w, (unsigned)(line_h + 4));
		XSetForeground(dpy, gc, col_line.pixel);
		XDrawRectangle(dpy, pm, gc, text_x, y, (unsigned)text_w, (unsigned)(line_h + 4));
		XftDrawStringUtf8(draw, &col_fg, font, text_x + 8, y + ascent,
		                  (FcChar8 *)prompt, (int)strlen(prompt));
		}

	draw_button(cancel_r, "Cancel", hot_cancel, 0);
	draw_button(ok_r, "OK", hot_ok, 1);
	XCopyArea(dpy, pm, win, gc, 0, 0, (unsigned)win_w, (unsigned)win_h, 0, 0);
	}

static int in_rect(Rect r, int x, int y)
	{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
	}

static void accept(void)
	{
	size_t off = 0;
	while (off < len)
		{
		ssize_t n = write(STDOUT_FILENO, text + off, len - off);
		if (n < 0)
			{
			if (errno == EINTR)
				continue;
			die("write to stdout failed", 2);
			}
		off += (size_t)n;
		}
	exit_kind = 0;
	}

static void cancel(void)
	{
	if (!tty_msg("cancelled"))
		fprintf(stderr, APP_NAME ": cancelled\n");
	exit_kind = 1;
	}

static void handle_key(XKeyEvent *ev)
	{
	char buf[64];
	KeySym ks = 0;
	Status st = 0;
	int n = 0;
	int shift = (ev->state & ShiftMask) != 0;
	int ctrl = (ev->state & ControlMask) != 0;
	int meta = (ev->state & Mod1Mask) != 0;

	if (ic)
		n = Xutf8LookupString(ic, ev, buf, (int)sizeof buf - 1, &ks, &st);
	else
		n = XLookupString(ev, buf, (int)sizeof buf - 1, &ks, NULL);
	if (n < 0)
		n = 0;
	buf[n] = 0;
	caret_on = 1;

	/* pipe bar intercepts keys when active */
	if (pipe_active)
		{
		if (ks == XK_Return || ks == XK_KP_Enter)
			{
			pipe_run();
			return;
			}
		if (ks == XK_Escape)
			{
			pipe_close();
			return;
			}
		if (ks == XK_BackSpace)
			{
			if (pipe_len > 0)
				pipe_buf[--pipe_len] = 0;
			return;
			}
		if (n > 0 && (unsigned char)buf[0] >= 0x20 && pipe_len < sizeof pipe_buf - 1)
			{
			memcpy(pipe_buf + pipe_len, buf, (size_t)n);
			pipe_len += (size_t)n;
			pipe_buf[pipe_len] = 0;
			return;
			}
		return;
		}

	/* find bar intercepts keys when active */
	if (find_active)
		{
		if (ks == XK_Return || ks == XK_KP_Enter)
			{
			find_next();
			return;
			}
		if (ks == XK_Escape)
			{
			find_close();
			return;
			}
		if (ks == XK_BackSpace)
			{
			if (find_len > 0)
				find_buf[--find_len] = 0;
			return;
			}
		if (n > 0 && (unsigned char)buf[0] >= 0x20 && find_len < sizeof find_buf - 1)
			{
			memcpy(find_buf + find_len, buf, (size_t)n);
			find_len += (size_t)n;
			find_buf[find_len] = 0;
			return;
			}
		return;
		}

	if (ctrl && (ks == XK_f || ks == XK_F))
		{
		find_open();
		return;
		}

	if (ctrl && !meta && (ks == XK_Return || ks == XK_KP_Enter || ks == XK_s || ks == XK_S))
		{
		accept();
		return;
		}
	if (ks == XK_Escape || (ctrl && (ks == XK_g || ks == XK_G)))
		{
		cancel();
		return;
		}
	if (ctrl && (ks == XK_a || ks == XK_A))
		{
		select_all();
		return;
		}
	if (ctrl && (ks == XK_c || ks == XK_C || ks == XK_Insert))
		{
		copy_sel();
		return;
		}
	if (ctrl && (ks == XK_x || ks == XK_X))
		{
		cut_sel();
		reveal_cursor();
		return;
		}
	if ((ctrl && (ks == XK_v || ks == XK_V)) || (shift && ks == XK_Insert))
		{
		request_paste(xa_clip);
		return;
		}
	if (ctrl && (ks == XK_z || ks == XK_Z))
		{
		undo();
		reveal_cursor();
		return;
		}
	if (ctrl && (ks == XK_k || ks == XK_K))
		{
		kill_line();
		reveal_cursor();
		return;
		}
	if (ks == XK_BackSpace || (ctrl && ks == XK_h))
		{
		backspace();
		reveal_cursor();
		return;
		}
	if (ks == XK_Delete || ks == XK_KP_Delete)
		{
		delete_fwd();
		reveal_cursor();
		return;
		}
	if (ks == XK_Left || ks == XK_KP_Left)
		{
		move_left(shift);
		reveal_cursor();
		return;
		}
	if (ks == XK_Right || ks == XK_KP_Right)
		{
		move_right(shift);
		reveal_cursor();
		return;
		}
	if (ks == XK_Up || ks == XK_KP_Up)
		{
		move_vert(-1, shift);
		reveal_cursor();
		return;
		}
	if (ks == XK_Down || ks == XK_KP_Down)
		{
		move_vert(1, shift);
		reveal_cursor();
		return;
		}
	if (ks == XK_Home || ks == XK_KP_Home)
		{
		move_home(shift, ctrl);
		reveal_cursor();
		return;
		}
	if (ks == XK_End || ks == XK_KP_End)
		{
		move_end(shift, ctrl);
		reveal_cursor();
		return;
		}
	if (ks == XK_Page_Up || ks == XK_KP_Page_Up)
		{
		size_t vis = (size_t)(text_h / line_h);
		size_t i;
		if (vis < 1)
			vis = 1;
		for (i = 0; i < vis; i++)
			move_vert(-1, shift);
		reveal_cursor();
		return;
		}
	if (ks == XK_Page_Down || ks == XK_KP_Page_Down)
		{
		size_t vis = (size_t)(text_h / line_h);
		size_t i;
		if (vis < 1)
			vis = 1;
		for (i = 0; i < vis; i++)
			move_vert(1, shift);
		reveal_cursor();
		return;
		}
	if (ks == XK_Return || ks == XK_KP_Enter)
		{
		insert_bytes("\n", 1);
		reveal_cursor();
		return;
		}
	if (ks == XK_Tab && !ctrl)
		{
		insert_bytes("\t", 1);
		reveal_cursor();
		return;
		}
	if (!ctrl && !meta && n > 0 && (unsigned char)buf[0] >= 0x20)
		{
		insert_bytes(buf, (size_t)n);
		reveal_cursor();
		}
	}

static void place_click(int x, int y, int extend)
	{
	int rel;
	size_t li;
	if (y < text_y)
		y = text_y;
	rel = (y - text_y) / line_h;
	if (rel < 0)
		rel = 0;
	li = top_line + (size_t)rel;
	if (nlines == 0)
		return;
	if (li >= nlines)
		li = nlines - 1;
	if (!extend)
		clear_sel();
	else if (!selecting)
		{
		anchor = cursor;
		selecting = 1;
		}
	cursor = pos_from_px(li, x - text_x + xoff);
	pref_col = col_at(cursor);
	caret_on = 1;
	}

static void handle_selection_request(XSelectionRequestEvent *req)
	{
	XSelectionEvent sev;
	Atom prop = req->property;
	memset(&sev, 0, sizeof sev);
	sev.type = SelectionNotify;
	sev.display = req->display;
	sev.requestor = req->requestor;
	sev.selection = req->selection;
	sev.target = req->target;
	sev.time = req->time;
	sev.property = None;
	if (prop == None)
		prop = req->target;
	if (clip && (req->target == xa_utf8 || req->target == XA_STRING))
		{
		XChangeProperty(dpy, req->requestor, prop, req->target, 8,
		                PropModeReplace, (unsigned char *)clip, (int)clip_len);
		sev.property = prop;
		}
	else if (req->target == xa_targets)
		{
		Atom tg[4];
		tg[0] = xa_targets;
		tg[1] = xa_utf8;
		tg[2] = XA_STRING;
		tg[3] = xa_compound;
		XChangeProperty(dpy, req->requestor, prop, XA_ATOM, 32,
		                PropModeReplace, (unsigned char *)tg, 4);
		sev.property = prop;
		}
	XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&sev);
	}

static void handle_selection_notify(XSelectionEvent *sev)
	{
	Atom type;
	int fmt;
	unsigned long nitems, left;
	unsigned char *data = NULL;
	if (!paste_phase)
		return;
	if (sev->property == None)
		{
		if (paste_phase == 1)
			{
			paste_phase = 2;
			XConvertSelection(dpy, paste_sel, XA_STRING, xa_paste, win, CurrentTime);
			return;
			}
		paste_phase = 0;
		return;
		}
	if (XGetWindowProperty(dpy, win, xa_paste, 0, 1 << 20, True,
	                       AnyPropertyType, &type, &fmt, &nitems, &left, &data) == Success &&
	        data)
		{
		size_t nbytes = fmt == 8 ? nitems : nitems * (size_t)(fmt / 8);
		insert_bytes((char *)data, nbytes);
		reveal_cursor();
		XFree(data);
		}
	paste_phase = 0;
	}

static void scroll_by(int lines_delta)
	{
	if (lines_delta < 0)
		{
		size_t d = (size_t)(-lines_delta);
		top_line = top_line > d ? top_line - d : 0;
		}
	else
		{
		top_line += (size_t)lines_delta;
		if (nlines == 0)
			top_line = 0;
		else if (top_line >= nlines)
			top_line = nlines - 1;
		}
	}

static void append_fd(int fd, const char *what)
	{
	char buf[8192];
	for (;;)
		{
		ssize_t n = read(fd, buf, sizeof buf);
		if (n == 0)
			break;
		if (n < 0)
			{
			if (errno == EINTR)
				continue;
			fprintf(stderr, APP_NAME ": %s: %s\n", what, strerror(errno));
			die("read failed", 2);
			}
		ensure(len + (size_t)n + 1);
		memcpy(text + len, buf, (size_t)n);
		len += (size_t)n;
		text[len] = 0;
		}
	}

static void load_inputs(int nfiles, char **files)
	{
	int i, saw_stdin = 0;
	text = xmalloc(1);
	text[0] = 0;
	cap = 1;
	len = 0;
	if (nfiles == 0)
		{
		if (!isatty(STDIN_FILENO))
			append_fd(STDIN_FILENO, "stdin");
		close(STDIN_FILENO);
		return;
		}
	for (i = 0; i < nfiles; i++)
		{
		int fd;
		if (strcmp(files[i], "-") == 0)
			{
			if (!saw_stdin)
				{
				if (!isatty(STDIN_FILENO))
					append_fd(STDIN_FILENO, "stdin");
				saw_stdin = 1;
				}
			continue;
			}
		fd = open(files[i], O_RDONLY);
		if (fd < 0)
			{
			fprintf(stderr, APP_NAME ": %s: %s\n", files[i], strerror(errno));
			die("cannot open file", 2);
			}
		append_fd(fd, files[i]);
		close(fd);
		}
	if (saw_stdin)
		close(STDIN_FILENO);
	}

static void open_ui(void)
	{
	XSetWindowAttributes attr;
	XSizeHints hints;
	XClassHint classh;
	XWMHints wmh;
	unsigned long mask;

	dpy = XOpenDisplay(NULL);
	if (!dpy)
		die("cannot open display (is DISPLAY set?)", 2);
	XSetIOErrorHandler(x_io_error);
	scr = DefaultScreen(dpy);
	visual = DefaultVisual(dpy, scr);
	cmap = DefaultColormap(dpy, scr);
	font = open_font(font_name);
	if (!font)
		die("cannot open an Xft font", 2);
	ascent = font->ascent;
	line_h = font->ascent + font->descent + 2;
		{
		XGlyphInfo ext;
		XftTextExtentsUtf8(dpy, font, (FcChar8 *)"M", 1, &ext);
		char_w = ext.xOff > 0 ? ext.xOff : 8;
		}
	if (alloc_color(&col_fg, "#1c1914") ||
	        alloc_color(&col_bg, "#efeae2") ||
	        alloc_color(&col_paper, "#fbf8f2") ||
	        alloc_color(&col_sel, "#d4e4f5") ||
	        alloc_color(&col_caret, "#b84324") ||
	        alloc_color(&col_muted, "#6e675c") ||
	        alloc_color(&col_okbg, "#245c45") ||
	        alloc_color(&col_okfg, "#f7f4ee") ||
	        alloc_color(&col_btn, "#e4ddd0") ||
	        alloc_color(&col_btnfg, "#1c1914") ||
	        alloc_color(&col_line, "#cfc6b8"))
		die("cannot allocate colours", 2);

	attr.background_pixel = col_bg.pixel;
	attr.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
	                  ButtonReleaseMask | PointerMotionMask | StructureNotifyMask |
	                  FocusChangeMask;
	attr.colormap = cmap;
	mask = CWBackPixel | CWEventMask | CWColormap;
	win = XCreateWindow(dpy, RootWindow(dpy, scr), 80, 80,
	                    (unsigned)win_w, (unsigned)win_h, 0,
	                    DefaultDepth(dpy, scr), InputOutput, visual, mask, &attr);
	gc = XCreateGC(dpy, win, 0, NULL);
	xa_wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &xa_wm_del, 1);
	xa_clip = XInternAtom(dpy, "CLIPBOARD", False);
	xa_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	xa_targets = XInternAtom(dpy, "TARGETS", False);
	xa_compound = XInternAtom(dpy, "COMPOUND_TEXT", False);
	xa_paste = XInternAtom(dpy, "XCAT_PASTE", False);

	hints.flags = PMinSize | PSize;
	hints.min_width = 420;
	hints.min_height = 220;
	hints.width = win_w;
	hints.height = win_h;
	XSetWMNormalHints(dpy, win, &hints);
	classh.res_name = "xcat";
	classh.res_class = "Xcat";
	XSetClassHint(dpy, win, &classh);
	wmh.flags = InputHint;
	wmh.input = True;
	XSetWMHints(dpy, win, &wmh);
	XStoreName(dpy, win, title);
	XSetIconName(dpy, win, title);
		{
		XTextProperty tp;
		if (Xutf8TextListToTextProperty(dpy, &title, 1, XUTF8StringStyle, &tp) == Success)
			{
			XSetWMName(dpy, win, &tp);
			XFree(tp.value);
			}
		}
	XStoreName(dpy, win, title);
	XSetIconName(dpy, win, title);

	im = XOpenIM(dpy, NULL, NULL, NULL);
	if (im)
		{
		ic = XCreateIC(im, XNInputStyle,
		               XIMPreeditNothing | XIMStatusNothing,
		               XNClientWindow, win, XNFocusWindow, win, NULL);
		}
	XMapRaised(dpy, win);
	XFlush(dpy);
	}

static void run(void)
	{
	int xfd = ConnectionNumber(dpy);
	int button1 = 0;
	exit_kind = -1;
	while (exit_kind < 0)
		{
		fd_set fds;
		struct timeval tv;
		int r;
		while (XPending(dpy))
			{
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (ev.type == KeyPress)
				{
				KeySym ks = XLookupKeysym(&ev.xkey, 0);
				if (ev.type == KeyPress && !fp.active)
					{
					if (ks == XK_Escape)
						{
						cancel();
						break;
						}
					}
				}
			if (XFilterEvent(&ev, None))
				continue;
			/* file picker is modal — route its events */
			if (fp.active)
				{
				if (ev.xany.window == fp.win)
					{
					switch (ev.type)
						{
						case Expose:
							if (ev.xexpose.count == 0)
								fp_draw();
							break;
						case KeyPress:
							fp_handle_key(&ev.xkey);
							break;
						case ButtonPress:
							fp_handle_button(&ev.xbutton);
							break;
						case ClientMessage:
							if ((Atom)ev.xclient.data.l[0] == xa_wm_del)
								fp_close();
							break;
						default:
							break;
						}
					continue;
					}
				/* swallow main-window events while picker is open */
				continue;
				}

			switch (ev.type)
				{
				case Expose:
					if (ev.xexpose.count == 0)
						draw_all();
					break;
				case ConfigureNotify:
					if (ev.xconfigure.width != win_w || ev.xconfigure.height != win_h)
						{
						win_w = ev.xconfigure.width;
						win_h = ev.xconfigure.height;
						draw_all();
						}
					break;
				case KeyPress:
					handle_key(&ev.xkey);
					if (exit_kind < 0)
						draw_all();
					break;
				case ButtonPress:
					if (ev.xbutton.button == Button1)
						{
						int mx = ev.xbutton.x, my = ev.xbutton.y;

						/* menu bar hit? */
						if (my < MENU_H)
							{
							int i;
							for (i = 0; i < NMENUS; i++)
								{
								int tx = menu_title_x(i);
								XGlyphInfo ext;
								XftTextExtentsUtf8(dpy, font,
								                   (FcChar8 *)menus[i].label,
								                   (int)strlen(menus[i].label), &ext);
								if (mx >= tx - 4 && mx < tx + ext.xOff + 4)
									{
									if (menu_open == i)
										menu_open = -1;
									else
										{
										menu_open = i;
										menu_hot = -1;
										}
									draw_all();
									break;
									}
								}
							if (menu_open < 0)
								draw_all();
							break;
							}

						/* dropdown item hit? */
						if (menu_open >= 0)
							{
							menu_dropdown_geom();
							if (mx >= menu_drop_x && mx < menu_drop_x + menu_drop_w &&
							        my >= menu_drop_y && my < menu_drop_y + menu_drop_h)
								{
								int idx = (my - menu_drop_y - 3) / line_h;
								if (idx >= 0 && idx < menus[menu_open].nitems)
									menu_execute(menu_open, idx);
								else
									{
									ctx_mode = 0;
									menu_open = -1;
									}
								draw_all();
								break;
								}
							menu_open = -1;
							menu_hot = -1;
							draw_all();
							break;
							}

						if (in_rect(ok_r, mx, my))
							{
							accept();
							break;
							}
						if (in_rect(cancel_r, mx, my))
							{
							cancel();
							break;
							}
						button1 = 1;
						if (find_active && my >= text_y && my < text_y + text_h)
							find_close();
						place_click(mx, my,
						            (ev.xbutton.state & ShiftMask) != 0);
						draw_all();
						}
					else if (ev.xbutton.button == Button2)
						{
						request_paste(XA_PRIMARY);

						}
					else if (ev.xbutton.button == Button3)
						{
						int mx = ev.xbutton.x, my = ev.xbutton.y;
						/* only in the text area */
						if (my >= text_y && my < text_y + text_h)
							{
							menu_open = 1;  /* Edit menu */
							menu_hot = -1;
							ctx_mode = 1;
							ctx_x = mx;
							ctx_y = my;
							/* clamp so the dropdown stays in the window */
							menu_dropdown_geom();
							if (ctx_x + menu_drop_w > win_w)
								ctx_x = win_w - menu_drop_w;
							if (ctx_y + menu_drop_h > win_h)
								ctx_y = my - menu_drop_h;
							draw_all();
							}
						break;
						}
					else if (ev.xbutton.button == Button4)
						{
						scroll_by((ev.xbutton.state & ShiftMask) ? 0 : -3);
						if (ev.xbutton.state & ShiftMask)
							xoff = xoff > 48 ? xoff - 48 : 0;
						draw_all();
						}
					else if (ev.xbutton.button == Button5)
						{
						scroll_by((ev.xbutton.state & ShiftMask) ? 0 : 3);
						if (ev.xbutton.state & ShiftMask)
							xoff += 48;
						draw_all();
						}
					break;
				case MotionNotify:
					if (button1)
						{
						place_click(ev.xmotion.x, ev.xmotion.y, 1);
						draw_all();
						}
					else
						{
						int o1 = in_rect(ok_r, ev.xmotion.x, ev.xmotion.y);
						int o2 = in_rect(cancel_r, ev.xmotion.x, ev.xmotion.y);
						if (o1 != hot_ok || o2 != hot_cancel)
							{
							hot_ok = o1;
							hot_cancel = o2;
							draw_all();
							}
						if (menu_open >= 0)
							{
							int idx = -1;
							menu_dropdown_geom();
							if (ev.xmotion.x >= menu_drop_x &&
							        ev.xmotion.x < menu_drop_x + menu_drop_w &&
							        ev.xmotion.y >= menu_drop_y &&
							        ev.xmotion.y < menu_drop_y + menu_drop_h)
								{
								idx = (ev.xmotion.y - menu_drop_y - 3) / line_h;
								if (idx >= menus[menu_open].nitems)
									idx = -1;
								}
							if (idx != menu_hot)
								{
								menu_hot = idx;
								draw_all();
								}
							}
						}
					break;
				case ButtonRelease:
					if (ev.xbutton.button == Button1)
						button1 = 0;
					break;
				case SelectionRequest:
					handle_selection_request(&ev.xselectionrequest);
					break;
				case SelectionNotify:
					handle_selection_notify(&ev.xselection);
					if (exit_kind < 0)
						draw_all();
					break;
				case SelectionClear:
					own_clip = 0;
					break;
				case FocusIn:
					if (ic)
						XSetICFocus(ic);
					caret_on = 1;
					draw_all();
					break;
				case FocusOut:
					if (ic)
						XUnsetICFocus(ic);
					break;
				case ClientMessage:
					if ((Atom)ev.xclient.data.l[0] == xa_wm_del)
						cancel();
					break;
				default:
					break;
				}
			}
		if (exit_kind >= 0)
			break;
		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		tv.tv_sec = 0;
		tv.tv_usec = 450000;
		r = select(xfd + 1, &fds, NULL, NULL, &tv);
		if (r < 0 && errno == EINTR)
			continue;
		if (r == 0)
			{
			caret_on = !caret_on;
			dirty_blink = 1;
			draw_all();
			}
		}
	}

int main(int argc, char **argv)
	{
	int i;
	int title_set = 0;
	char **files = NULL;
	int nfiles = 0;
	setlocale(LC_ALL, "");
	XSetLocaleModifiers("");
	for (i = 1; i < argc; i++)
		{
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
			{
			usage();
			return 0;
			}
		if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
			{
			title = argv[++i];
			title_set = 1;
			continue;
			}
		if (strcmp(argv[i], "-fn") == 0 && i + 1 < argc)
			{
			font_name = argv[++i];
			continue;
			}
		if (strcmp(argv[i], "-g") == 0 && i + 1 < argc)
			{
			if (parse_geom(argv[++i]) != 0)
				die("bad geometry, want WIDTHxHEIGHT", 2);
			continue;
			}
		if (strcmp(argv[i], "--") == 0)
			{
			i++;
			break;
			}
		if (argv[i][0] == '-' && argv[i][1] != '\0')
			{
			usage();
			return 2;
			}
		files = xrealloc(files, (size_t)(nfiles + 1) * sizeof *files);
		files[nfiles++] = argv[i];
		}
	for (; i < argc; i++)
		{
		files = xrealloc(files, (size_t)(nfiles + 1) * sizeof *files);
		files[nfiles++] = argv[i];
		}
	if (!title_set && nfiles == 1 && strcmp(files[0], "-") != 0)
		title = files[0];
	else if (!title_set && nfiles > 1)
		title = "xcat";
	/* remember first real filename for Save default */
	for (i = 0; i < nfiles; i++)
		if (strcmp(files[i], "-") != 0)
			{
			first_file = files[i];
			break;
			}
	load_inputs(nfiles, files);
	free(files);
	rebuild_lines();
	cursor = 0;
	anchor = 0;
	pref_col = 0;
	top_line = 0;
	xoff = 0;
	open_ui();
	reveal_cursor();
	run();
	XCloseDisplay(dpy);
	return exit_kind == 0 ? 0 : 1;
	}
