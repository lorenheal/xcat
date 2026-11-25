/* main.c — entry point, event loop, and shared helpers.
 * SPDX-License-Identifier: MIT
 */
#include "xcat.h"
#include "buffer.h"
#include "display.h"
#include "find.h"
#include "filepicker.h"
#include "clipboard.h"
#include "fileio.h"
#include "input.h"
#include "menu.h"
#include <locale.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

const char *menu_label(int);
int menu_nitems(int i);

/* --- shared helpers (declared in xcat.h) --- */

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		die("out of memory", 2);
	return p;
}

void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);
	if (!q)
		die("out of memory", 2);
	return q;
}

int tty_msg(const char *msg)
{
	FILE *t = fopen("/dev/tty", "w");
	if (!t)
		return 0;
	fprintf(t, APP_NAME ": %s\n", msg);
	fflush(t);
	fclose(t);
	return 1;
}

void die(const char *msg, int code)
{
	if (!tty_msg(msg))
		fprintf(stderr, APP_NAME ": %s\n", msg);
	exit(code);
}

int x_io_error(Display *d)
{
	(void)d;
	if (!tty_msg("lost connection to the X server"))
		fprintf(stderr, APP_NAME ": lost connection to the X server\n");
	_exit(2);
}

void usage(void)
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

void accept(App *a)
{
	size_t off = 0;
	while (off < a->len)
		{
		ssize_t n = write(STDOUT_FILENO, a->text + off, a->len - off);
		if (n < 0)
			{
			if (errno == EINTR)
				continue;
			die("write to stdout failed", 2);
			}
		off += (size_t)n;
		}
	a->exit_kind = 0;
}

void cancel(App *a)
{
	if (!tty_msg("cancelled"))
		fprintf(stderr, APP_NAME ": cancelled\n");
	a->exit_kind = 1;
}


/* --- event loop --- */

static void run(App *a)
{
	int xfd = ConnectionNumber(a->dpy);
	int button1 = 0;
	a->exit_kind = -1;
	while (a->exit_kind < 0)
		{
		fd_set fds;
		struct timeval tv;
		int r;
		while (XPending(a->dpy))
			{
			XEvent ev;
			XNextEvent(a->dpy, &ev);
			if (XFilterEvent(&ev, None))
				continue;

			/* file picker is modal — route its events */
			if (fp_is_active(a))
				{
				if (ev.xany.window == fp_get_window())
					{
					switch (ev.type)
						{
						case Expose:
							if (ev.xexpose.count == 0)
								fp_draw(a);
							break;
						case KeyPress:
							fp_handle_key(a, &ev.xkey);
							break;
						case ButtonPress:
							fp_handle_button(a, &ev.xbutton);
							break;
						case ClientMessage:
							if ((Atom)ev.xclient.data.l[0] == a->xa_wm_del)
								fp_close(a);
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
						disp_draw_all(a);
					break;
				case ConfigureNotify:
					if (ev.xconfigure.width != a->win_w || ev.xconfigure.height != a->win_h)
						{
						a->win_w = ev.xconfigure.width;
						a->win_h = ev.xconfigure.height;
						disp_draw_all(a);
						}
					break;
				case KeyPress:
					input_handle_key(a, &ev.xkey);
					if (a->exit_kind < 0)
						disp_draw_all(a);
					break;
				case ButtonPress:
					if (ev.xbutton.button == Button1)
						{
						int mx = ev.xbutton.x, my = ev.xbutton.y;

						/* menu bar hit? */
						if (my < MENU_H)
							{
							int i;
							for (i = 0; i < 3; i++)
								{
								int tx = menu_title_x(a, i);
								XGlyphInfo ext;
								XftTextExtentsUtf8(a->dpy, a->font,
								                   (FcChar8 *)menu_label(i),
								                   (int)strlen(menu_label(i)), &ext);
								if (mx >= tx - 4 && mx < tx + ext.xOff + 4)
									{
									if (a->menu_open == i)
										a->menu_open = -1;
									else
										{
										a->menu_open = i;
										a->menu_hot = -1;
										a->ctx_mode = 0;
										}
									disp_draw_all(a);
									break;
									}
								}
							if (a->menu_open < 0)
								disp_draw_all(a);
							break;
							}

						/* dropdown item hit? */
						if (a->menu_open >= 0)
							{
							menu_dropdown_geom(a);
							if (mx >= a->menu_drop_x && mx < a->menu_drop_x + a->menu_drop_w &&
							    my >= a->menu_drop_y && my < a->menu_drop_y + a->menu_drop_h)
								{
								int idx = (my - a->menu_drop_y - 3) / a->line_h;
								if (idx >= 0 && idx < menu_nitems(a->menu_open))
									menu_execute(a, a->menu_open, idx);
								else
									{
									a->ctx_mode = 0;
									a->menu_open = -1;
									}
								disp_draw_all(a);
								break;
								}
							a->menu_open = -1;
							a->menu_hot = -1;
							disp_draw_all(a);
							break;
							}

						if (input_in_rect(a->ok_r, mx, my))
							{
							accept(a);
							break;
							}
						if (input_in_rect(a->cancel_r, mx, my))
							{
							cancel(a);
							break;
							}
						button1 = 1;
						if (a->find_active && my >= a->text_y && my < a->text_y + a->text_h)
							find_close(a);
						input_place_click(a, mx, my,
						                  (ev.xbutton.state & ShiftMask) != 0);
						disp_draw_all(a);
						}
					else if (ev.xbutton.button == Button2)
						{
						clip_request_paste(a, XA_PRIMARY);
						}
					else if (ev.xbutton.button == Button3)
						{
						int mx = ev.xbutton.x, my = ev.xbutton.y;
						if (my >= a->text_y && my < a->text_y + a->text_h)
							{
							a->menu_open = 1;  /* Edit menu */
							a->menu_hot = -1;
							a->ctx_mode = 1;
							a->ctx_x = mx;
							a->ctx_y = my;
							menu_dropdown_geom(a);
							if (a->ctx_x + a->menu_drop_w > a->win_w)
								a->ctx_x = a->win_w - a->menu_drop_w;
							if (a->ctx_y + a->menu_drop_h > a->win_h)
								a->ctx_y = my - a->menu_drop_h;
							disp_draw_all(a);
							}
						break;
						}
					else if (ev.xbutton.button == Button4)
						{
						input_scroll_by(a, (ev.xbutton.state & ShiftMask) ? 0 : -3);
						if (ev.xbutton.state & ShiftMask)
							a->xoff = a->xoff > 48 ? a->xoff - 48 : 0;
						disp_draw_all(a);
						}
					else if (ev.xbutton.button == Button5)
						{
						input_scroll_by(a, (ev.xbutton.state & ShiftMask) ? 0 : 3);
						if (ev.xbutton.state & ShiftMask)
							a->xoff += 48;
						disp_draw_all(a);
						}
					break;
				case MotionNotify:
					if (button1)
						{
						input_place_click(a, ev.xmotion.x, ev.xmotion.y, 1);
						disp_draw_all(a);
						}
					else
						{
						int o1 = input_in_rect(a->ok_r, ev.xmotion.x, ev.xmotion.y);
						int o2 = input_in_rect(a->cancel_r, ev.xmotion.x, ev.xmotion.y);
						if (o1 != a->hot_ok || o2 != a->hot_cancel)
							{
							a->hot_ok = o1;
							a->hot_cancel = o2;
							disp_draw_all(a);
							}
						if (a->menu_open >= 0)
							{
							int idx = -1;
							menu_dropdown_geom(a);
							if (ev.xmotion.x >= a->menu_drop_x &&
							    ev.xmotion.x < a->menu_drop_x + a->menu_drop_w &&
							    ev.xmotion.y >= a->menu_drop_y &&
							    ev.xmotion.y < a->menu_drop_y + a->menu_drop_h)
								{
								idx = (ev.xmotion.y - a->menu_drop_y - 3) / a->line_h;
								if (idx >= menu_nitems(a->menu_open))
									idx = -1;
								}
							if (idx != a->menu_hot)
								{
								a->menu_hot = idx;
								disp_draw_all(a);
								}
							}
						}
					break;
				case ButtonRelease:
					if (ev.xbutton.button == Button1)
						button1 = 0;
					break;
				case SelectionRequest:
					clip_handle_selection_request(a, &ev.xselectionrequest);
					break;
				case SelectionNotify:
					clip_handle_selection_notify(a, &ev.xselection);
					if (a->exit_kind < 0)
						disp_draw_all(a);
					break;
				case SelectionClear:
					a->own_clip = 0;
					break;
				case FocusIn:
					if (a->ic)
						XSetICFocus(a->ic);
					a->caret_on = 1;
					disp_draw_all(a);
					break;
				case FocusOut:
					if (a->ic)
						XUnsetICFocus(a->ic);
					break;
				case ClientMessage:
					if ((Atom)ev.xclient.data.l[0] == a->xa_wm_del)
						cancel(a);
					break;
				default:
					break;
				}
			}
		if (a->exit_kind >= 0)
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
			a->caret_on = !a->caret_on;
			disp_draw_all(a);
			}
		}
}

/* --- main --- */

int main(int argc, char **argv)
{
	int i;
	int title_set = 0;
	char **files = NULL;
	int nfiles = 0;
	App a;

	memset(&a, 0, sizeof a);
	a.menu_open = -1;
	a.menu_hot = -1;
	a.win_w = 760;
	a.win_h = 520;
	a.char_w = 8;
	a.line_h = 16;
	a.ascent = 12;
	a.caret_on = 1;
	a.title = "xcat";
	a.font_name = "monospace:size=13";
	a.exit_kind = -1;

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
			a.title = argv[++i];
			title_set = 1;
			continue;
			}
		if (strcmp(argv[i], "-fn") == 0 && i + 1 < argc)
			{
			a.font_name = argv[++i];
			continue;
			}
		if (strcmp(argv[i], "-g") == 0 && i + 1 < argc)
			{
			int w, h;
			if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w >= 200 && h >= 160)
				{
				a.win_w = w;
				a.win_h = h;
				}
			else
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
		a.title = files[0];
	else if (!title_set && nfiles > 1)
		a.title = "xcat";
	for (i = 0; i < nfiles; i++)
		if (strcmp(files[i], "-") != 0)
			{
			a.first_file = files[i];
			break;
			}

	fileio_load_inputs(&a, nfiles, files);
	free(files);
	buf_rebuild_lines(&a);
	a.cursor = 0;
	a.anchor = 0;
	a.pref_col = 0;
	a.top_line = 0;
	a.xoff = 0;

	disp_open_ui(&a);
	disp_reveal_cursor(&a);
	run(&a);
	XCloseDisplay(a.dpy);
	return a.exit_kind == 0 ? 0 : 1;
}   
