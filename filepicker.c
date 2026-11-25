/* filepicker.c — modal file open/insert/save dialog.
 * SPDX-License-Identifier: MIT
 */
#include "filepicker.h"
#include "buffer.h"
#include "fileio.h"
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>

#define FP_MAX_ENTRIES 512
#define FP_ENTRY_LEN   256

typedef struct {
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

/* --- helpers --- */

/* We need App for dpy — store a pointer */
static App *fp_app;

static void fp_fill2(int x, int y, int w, int h, unsigned long pixel)
{
	if (w <= 0 || h <= 0)
		return;
	XSetForeground(fp_app->dpy, fp.gc, pixel);
	XFillRectangle(fp_app->dpy, fp.pm, fp.gc, x, y, (unsigned)w, (unsigned)h);
}

static void fp_stroke(int x, int y, int w, int h, unsigned long pixel)
{
	XSetForeground(fp_app->dpy, fp.gc, pixel);
	XDrawRectangle(fp_app->dpy, fp.pm, fp.gc, x, y, (unsigned)(w - 1), (unsigned)(h - 1));
}

static void fp_ensure_pixmap(void)
{
	if (fp.pm && fp.pm_w == fp.w && fp.pm_h == fp.h)
		return;
	if (fp.draw)
		XftDrawDestroy(fp.draw);
	if (fp.pm)
		XFreePixmap(fp_app->dpy, fp.pm);
	fp.pm = XCreatePixmap(fp_app->dpy, fp.win, (unsigned)fp.w, (unsigned)fp.h,
	                      (unsigned)DefaultDepth(fp_app->dpy, fp_app->scr));
	fp.pm_w = fp.w;
	fp.pm_h = fp.h;
	fp.draw = XftDrawCreate(fp_app->dpy, fp.pm, fp_app->visual, fp_app->cmap);
}

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

	/* sort: directories first, then files, case-insensitive */
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

static void fp_do_save(App *a, const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0)
		{
		size_t off = 0;
		while (off < a->len)
			{
			ssize_t n = write(fd, a->text + off, a->len - off);
			if (n < 0)
				{
				if (errno == EINTR) continue;
				tty_msg(strerror(errno));
				break;
				}
			off += (size_t)n;
			}
		close(fd);
		if (off == a->len)
			tty_msg("saved");
		}
	else
		tty_msg(strerror(errno));
}

static void fp_do_insert(App *a, const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		{
		tty_msg(strerror(errno));
		return;
		}
	{
	size_t save_cursor = a->cursor;
	size_t tail_len = a->len - a->cursor;
	char *tail;

	buf_push_undo(a);
	if (fp.replace)
		{
		a->len = 0;
		a->text[0] = 0;
		a->cursor = 0;
		save_cursor = 0;
		tail_len = 0;
		}

	tail = xmalloc(tail_len + 1);
	memcpy(tail, a->text + save_cursor, tail_len);
	tail[tail_len] = 0;

	a->len = save_cursor;
	a->text[a->len] = 0;

	fileio_append_fd(a, fd, path);
	close(fd);

	if (tail_len > 0)
		{
		buf_ensure(a, a->len + tail_len + 1);
		memcpy(a->text + a->len, tail, tail_len);
		a->len += tail_len;
		a->text[a->len] = 0;
		}
	free(tail);

	buf_rebuild_lines(a);
	a->cursor = a->len - tail_len;
	buf_clear_sel(a);
	disp_reveal_cursor(a);
	}
}

/* --- public API --- */

int fp_is_active(App *a)
{
	(void)a;
	return fp.active;
}

void fp_open(App *a, int save_mode, int replace)
{
	XSetWindowAttributes attr;
	XWMHints wmh;
	XSizeHints hints;
	unsigned long mask;
	XWindowAttributes wa;

	fp_app = a;
	fp.w = 520;
	fp.h = 380;
	fp.save_mode = save_mode;
	fp.replace = replace;

	XGetWindowAttributes(a->dpy, a->win, &wa);
	if (fp.w > wa.width) fp.w = wa.width;
	if (fp.h > wa.height) fp.h = wa.height;
	{
	int x = wa.x + (wa.width - fp.w) / 2;
	int y = wa.y + (wa.height - fp.h) / 2;
	if (x < 0) x = 0;
	if (y < 0) y = 0;

	memset(&attr, 0, sizeof attr);
	attr.background_pixel = a->col_paper.pixel;
	attr.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
	                  StructureNotifyMask;
	mask = CWBackPixel | CWEventMask;
	fp.win = XCreateWindow(a->dpy, RootWindow(a->dpy, a->scr),
	                       (unsigned)x, (unsigned)y,
	                       (unsigned)fp.w, (unsigned)fp.h, 0,
	                       DefaultDepth(a->dpy, a->scr), InputOutput,
	                       a->visual, mask, &attr);
	fp.gc = XCreateGC(a->dpy, fp.win, 0, NULL);
	XSetTransientForHint(a->dpy, fp.win, a->win);
	hints.flags = PMinSize;
	hints.min_width = 320;
	hints.min_height = 200;
	XSetWMNormalHints(a->dpy, fp.win, &hints);
	wmh.flags = InputHint;
	wmh.input = True;
	XSetWMHints(a->dpy, fp.win, &wmh);
	XStoreName(a->dpy, fp.win, save_mode ? "Save File" :
	            (replace ? "Open File" : "Insert File"));
	XSetWMProtocols(a->dpy, fp.win, &a->xa_wm_del, 1);

	if (!getcwd(fp.path, sizeof fp.path))
		snprintf(fp.path, sizeof fp.path, "/");
	fp.name_len = 0;
	fp.name_buf[0] = 0;
	if (save_mode && a->first_file)
		{
		const char *base = a->first_file;
		char *slash = strrchr(a->first_file, '/');
		if (slash)
			{
			size_t dlen = (size_t)(slash - a->first_file);
			if (dlen > 0 && dlen < sizeof fp.path)
				{
				memcpy(fp.path, a->first_file, dlen);
				fp.path[dlen] = 0;
				}
			base = slash + 1;
			}
		snprintf(fp.name_buf, sizeof fp.name_buf, "%s", base);
		fp.name_len = strlen(fp.name_buf);
		}
	fp_load_dir();
	fp.active = 1;
	XMapRaised(a->dpy, fp.win);
	XFlush(a->dpy);
	}
}

void fp_close(App *a)
{
	(void)a;
	if (fp.draw)
		XftDrawDestroy(fp.draw);
	if (fp.pm)
		XFreePixmap(fp_app->dpy, fp.pm);
	if (fp.win)
		XDestroyWindow(fp_app->dpy, fp.win);
	fp.win = 0;
	fp.pm = 0;
	fp.draw = NULL;
	fp.active = 0;
}

void fp_draw(App *a)
{
	int pad = 8;
	int lh = a->line_h;
	int list_y = pad + lh + 4;
	int name_h = fp.save_mode ? (lh + 4) : 0;
	int list_h = fp.h - list_y - pad - lh - 4 - name_h;
	int vis = list_h / lh;
	int i;
	char status[256];
	int list_x = pad;
	int list_w = fp.w - pad * 2;

	fp_ensure_pixmap();
	fp_fill2(0, 0, fp.w, fp.h, a->col_paper.pixel);
	fp_stroke(0, 0, fp.w, fp.h, a->col_line.pixel);

	/* path bar */
	{
	char pbar[4200];
	snprintf(pbar, sizeof pbar, "%s", fp.path);
	fp_fill2(pad, pad, list_w, lh, a->col_bg.pixel);
	fp_stroke(pad, pad, list_w, lh, a->col_line.pixel);
	XftDrawStringUtf8(fp.draw, &a->col_fg, a->font, pad + 6, pad + a->ascent,
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
			fp_fill2(list_x, ey, list_w, lh, a->col_sel.pixel);

		XftDrawStringUtf8(fp.draw, &a->col_fg, a->font, list_x + 6, ey + a->ascent,
		                  (FcChar8 *)disp, (int)strlen(disp));
		}

	/* filename entry for Save */
	if (fp.save_mode)
		{
		int ny = list_y + list_h + 2;
		char label[600];
		snprintf(label, sizeof label, "Name: %s", fp.name_buf);
		fp_fill2(pad, ny, list_w, lh, a->col_bg.pixel);
		fp_stroke(pad, ny, list_w, lh, a->col_line.pixel);
		XftDrawStringUtf8(fp.draw, &a->col_fg, a->font, pad + 6, ny + a->ascent,
		                  (FcChar8 *)label, (int)strlen(label));
		}

	/* status bar */
	snprintf(status, sizeof status,
	         fp.save_mode ?
	         "Enter: save    Esc: cancel" :
	         "%d entries    Enter: open    Backspace: up    Esc: cancel",
	         fp.nentries);
	XftDrawStringUtf8(fp.draw, &a->col_muted, a->font, pad, fp.h - pad - 2,
	                  (FcChar8 *)status, (int)strlen(status));

	XCopyArea(a->dpy, fp.pm, fp.win, fp.gc, 0, 0,
	          (unsigned)fp.w, (unsigned)fp.h, 0, 0);
}

void fp_handle_key(App *a, XKeyEvent *ev)
{
	KeySym ks = 0;
	char buf[64];
	int n = XLookupString(ev, buf, (int)sizeof buf - 1, &ks, NULL);
	(void)n;

	if (ks == XK_Escape)
		{
		fp_close(a);
		disp_draw_all(a);
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
			fp_draw(a);
			return;
			}
		}
	if (fp.save_mode && ks == XK_BackSpace && fp.name_len > 0)
		{
		fp.name_buf[--fp.name_len] = 0;
		fp_draw(a);
		return;
		}

	if (ks == XK_Return || ks == XK_KP_Enter)
		{
		if (fp.save_mode && fp.name_len > 0)
			{
			char dest[5000];
			snprintf(dest, sizeof dest, "%s/%s", fp.path, fp.name_buf);
			fp_do_save(a, dest);
			fp_close(a);
			disp_draw_all(a);
			return;
			}
		if (fp.sel >= 0 && fp.sel < fp.nentries)
			{
			char full[FILE_BUFFER_SIZE];
			if (strcmp(fp.entries[fp.sel], "<..>") == 0)
				{
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
				char dest[FILE_BUFFER_SIZE];
				const char *use = fp.name_len ? fp.name_buf : fp.entries[fp.sel];
				snprintf(dest, sizeof dest, "%s/%s", fp.path, use);
				fp_do_save(a, dest);
				fp_close(a);
				disp_draw_all(a);
				return;
				}
			else
				{
				snprintf(full, sizeof full, "%s/%s", fp.path, fp.entries[fp.sel]);
				fp_do_insert(a, full);
				fp_close(a);
				disp_draw_all(a);
				return;
				}
			}
		fp_draw(a);
		return;
		}

	if (ks == XK_BackSpace)
		{
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
		fp_draw(a);
		return;
		}
	if (ks == XK_Up || ks == XK_KP_Up)
		{
		if (fp.sel > 0)
			fp.sel--;
		if (fp.sel < fp.top)
			fp.top = fp.sel;
		fp_draw(a);
		return;
		}
	if (ks == XK_Down || ks == XK_KP_Down)
		{
		if (fp.sel + 1 < fp.nentries)
			fp.sel++;
		{
		int vis = (fp.h - 8 - a->line_h - 4 - 8 - a->line_h - 4) / a->line_h;
		if (vis < 1) vis = 1;
		if (fp.sel >= fp.top + vis)
			fp.top = fp.sel - vis + 1;
		}
		fp_draw(a);
		return;
		}
	if (ks == XK_Page_Up)
		{
		int vis = (fp.h - 8 - a->line_h - 4 - 8 - a->line_h - 4) / a->line_h;
		if (vis < 1) vis = 1;
		fp.sel -= vis;
		if (fp.sel < 0) fp.sel = 0;
		fp.top = fp.sel < fp.top ? fp.sel : fp.top - vis;
		if (fp.top < 0) fp.top = 0;
		fp_draw(a);
		return;
		}
	if (ks == XK_Page_Down)
		{
		int vis = (fp.h - 8 - a->line_h - 4 - 8 - a->line_h - 4) / a->line_h;
		if (vis < 1) vis = 1;
		fp.sel += vis;
		if (fp.sel >= fp.nentries) fp.sel = fp.nentries - 1;
		if (fp.sel >= fp.top + vis)
			fp.top = fp.sel - vis + 1;
		fp_draw(a);
		return;
		}
}

void fp_handle_button(App *a, XButtonEvent *ev)
{
	int pad = 8;
	int lh = a->line_h;
	int list_y = pad + lh + 4;
	int name_h = fp.save_mode ? (lh + 4) : 0;
	int list_h = fp.h - list_y - pad - lh - 4 - name_h;
	int vis = list_h / lh;

	if (ev->button != Button1)
		return;

	if (ev->y >= list_y && ev->y < list_y + vis * lh)
		{
		int idx = fp.top + (ev->y - list_y) / lh;
		if (idx >= 0 && idx < fp.nentries)
			{
			fp.sel = idx;
			fp_draw(a);
			return;
			}
		}
}   

Window fp_get_window(void)
{
	return fp.win;
}
