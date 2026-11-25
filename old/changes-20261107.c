#include <dirent.h>
#include <sys/stat.h>   Add these includes at the top (after <string.h>):
Add the file-picker state after the find_close function:/* ─── file picker ─── */

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
} FilePicker;

static FilePicker fp;

static int fp_cmp(const void *a, const void *b)
{
	const char *ea = *(const char (*)[FP_ENTRY_LEN])a;
	const char *eb = *(const char (*)[FP_ENTRY_LEN])b;
	return strcasecmp(ea, eb);
}

static void fp_load_dir(void)
{
	DIR *d;
	struct dirent *de;
	int n = 0;
	int i;
	size_t plen;

	fp.nentries = 0;
	fp.sel = 0;
	fp.top = 0;

	d = opendir(fp.path);
	if (!d)
		return;

	/* first entry: ".." if not root */
	plen = strlen(fp.path);
	if (plen > 1) {
		strcpy(fp.entries[0], "..");
		fp.entry_is_dir[0] = 1;
		fp.nentries++;
	}

	while ((de = readdir(d)) && fp.nentries < FP_MAX_ENTRIES) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		{
			char full[4600];
			struct stat st;
			snprintf(full, sizeof full, "%s/%s", fp.path, de->d_name);
			if (stat(full, &st) != 0)
				continue;
			if (n < FP_MAX_ENTRIES) {
				snprintf(fp.entries[n], FP_ENTRY_LEN, "%s", de->d_name);
				fp.entry_is_dir[n] = S_ISDIR(st.st_mode);
				n++;
			}
		}
	}
	closedir(d);

	/* sort: directories first, then files, both case-insensitive */
	/* simple insertion sort — fine for ≤512 entries */
	for (i = 1; i < fp.nentries; i++) {
		char tmp[FP_ENTRY_LEN];
		int tmpd;
		int j = i;
		while (j > 0) {
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

static void fp_draw(void)
{
	int pad = 8;
	int lh = line_h;
	int list_y = pad + lh + 4;
	int list_h = fp.h - list_y - pad - lh - 4;
	int vis = list_h / lh;
	int i;
	char status[256];
	int list_x = pad;
	int list_w = fp.w - pad * 2;

	fp_ensure_pixmap();
	fill_rect(0, 0, fp.w, fp.h, col_bg.pixel);
	fill_rect(0, 0, fp.w, fp.h, col_paper.pixel);
	stroke_rect(0, 0, fp.w, fp.h, col_line.pixel);

	/* path bar */
	{
		char pbar[4200];
		snprintf(pbar, sizeof pbar, "%s", fp.path);
		fill_rect(pad, pad, list_w, lh, col_bg.pixel);
		stroke_rect(pad, pad, list_w, lh, col_line.pixel);
		XftDrawStringUtf8(fp.draw, &col_fg, font, pad + 6, pad + ascent,
				  (FcChar8 *)pbar, (int)strlen(pbar));
	}

	/* entry list */
	for (i = 0; i < vis && i + fp.top < fp.nentries; i++) {
		int idx = fp.top + i;
		int ey = list_y + i * lh;
		const char *name = fp.entries[idx];
		char disp[FP_ENTRY_LEN + 2];
		if (fp.entry_is_dir[idx])
			snprintf(disp, sizeof disp, "%s/", name);
		else
			snprintf(disp, sizeof disp, "%s", name);

		if (idx == fp.sel)
			fill_rect(list_x, ey, list_w, lh, col_sel.pixel);

		XftDrawStringUtf8(fp.draw, &col_fg, font, list_x + 6, ey + ascent,
				  (FcChar8 *)disp, (int)strlen(disp));
	}

	/* status bar */
	snprintf(status, sizeof status,
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
	int rw, rh;
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
		wmh.flags = InputHint | TransientFor;
		wmh.input = True;
		wmh.transient_for = win;
		XSetWMHints(dpy, fp.win, &wmh);
		XStoreName(dpy, fp.win, "Insert File");
		XSetWMProtocols(dpy, fp.win, &xa_wm_del, 1);

		/* start in home directory */
		{
			const char *home = getenv("HOME");
			if (home)
				snprintf(fp.path, sizeof fp.path, "%s", home);
			else
				snprintf(fp.path, sizeof fp.path, "/");
		}if (!getcwd(fp.path, sizeof fp.path))
    snprintf(fp.path, sizeof fp.path, "/");   
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

	if (ks == XK_Escape) {
		fp_close();
		return;
	}
	if (ks == XK_Return || ks == XK_KP_Enter) {
		/* select current entry */
		if (fp.sel >= 0 && fp.sel < fp.nentries) {
			char full[4600];
			if (strcmp(fp.entries[fp.sel], "..") == 0) {
				/* go up */
				size_t plen = strlen(fp.path);
				if (plen > 1) {
					while (plen > 1 && fp.path[plen - 1] == '/')
						fp.path[--plen] = 0;
					while (plen > 1 && fp.path[plen - 1] != '/')
						plen--;
					fp.path[plen] = 0;
				}
				fp_load_dir();
			} else if (fp.entry_is_dir[fp.sel]) {
				snprintf(full, sizeof full, "%s/%s", fp.path, fp.entries[fp.sel]);
				snprintf(fp.path, sizeof fp.path, "%s", full);
				fp_load_dir();
			} else {
				/* it's a file — insert it */
				snprintf(full, sizeof full, "%s/%s", fp.path, fp.entries[fp.sel]);
				{
					int fd = open(full, O_RDONLY);
					if (fd >= 0) {
						append_fd(fd, full);
						close(fd);
						rebuild_lines();
						cursor = len;
						clear_sel();
						reveal_cursor();
						push_undo();
					} else {
						tty_msg(strerror(errno));
					}
				}
				fp_close();
				return;
			}
		}
		fp_draw();
		return;
	}
	if (ks == XK_BackSpace) {
		/* go up one level */
		size_t plen = strlen(fp.path);
		if (plen > 1) {
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
	if (ks == XK_Up || ks == XK_KP_Up) {
		if (fp.sel > 0)
			fp.sel--;
		if (fp.sel < fp.top)
			fp.top = fp.sel;
		fp_draw();
		return;
	}
	if (ks == XK_Down || ks == XK_KP_Down) {
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
	if (ks == XK_Page_Up) {
		int vis = (fp.h - 8 - line_h - 4 - 8 - line_h - 4) / line_h;
		if (vis < 1) vis = 1;
		fp.sel -= vis;
		if (fp.sel < 0) fp.sel = 0;
		fp.top = fp.sel < fp.top ? fp.sel : fp.top - vis;
		if (fp.top < 0) fp.top = 0;
		fp_draw();
		return;
	}
	if (ks == XK_Page_Down) {
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
	int list_h = fp.h - list_y - pad - lh - 4;
	int vis = list_h / lh;

	if (ev->button != Button1)
		return;

	/* click in list area */
	if (ev->y >= list_y && ev->y < list_y + vis * lh) {
		int idx = fp.top + (ev->y - list_y) / lh;
		if (idx >= 0 && idx < fp.nentries) {
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
Replace the stub file_insert():
static void file_insert(void)
{
	fp_open();
}   
In the run() event loop, add picker handling. Inside the while (XPending(dpy)) block, right after the XFilterEvent check and before the switch, insert:

			/* file picker is modal — route its events */
			if (fp.active) {
				if (ev.xany.window == fp.win) {
					switch (ev.type) {
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
/*
* One correction — in fp_handle_key, the file-insert path should push undo before the insert (to match the rest of the code). Change the file branch to:
*/
			} else {
				snprintf(full, sizeof full, "%s/%s", fp.path, fp.entries[fp.sel]);
				{
					int fd = open(full, O_RDONLY);
					if (fd >= 0) {
						push_undo();
						append_fd(fd, full);
						close(fd);
						rebuild_lines();
						cursor = len;
						clear_sel();
						reveal_cursor();
					} else {
						tty_msg(strerror(errno));
					}
				}
				fp_close();
				draw_all();
				return;
			}   
And add draw_all(); after fp_close() in the Escape branch too, so the main window refreshes:

	if (ks == XK_Escape) {
		fp_close();
		draw_all();
		return;
	}   