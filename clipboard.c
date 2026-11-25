/* clipboard.c — copy, cut, paste, and X11 selection protocol.
 * SPDX-License-Identifier: MIT
 */
#include "clipboard.h"
#include "buffer.h"
#include "display.h"
#include <string.h>

void clip_copy_sel(App *a)
{
	size_t sa, sb;
	buf_sel_range(a, &sa, &sb);
	if (sa == sb)
		return;
	free(a->clip);
	a->clip_len = sb - sa;
	a->clip = xmalloc(a->clip_len + 1);
	memcpy(a->clip, a->text + sa, a->clip_len);
	a->clip[a->clip_len] = 0;
	XSetSelectionOwner(a->dpy, a->xa_clip, a->win, CurrentTime);
	XSetSelectionOwner(a->dpy, XA_PRIMARY, a->win, CurrentTime);
	a->own_clip = 1;
}

void clip_cut_sel(App *a)
{
	size_t sa, sb;
	buf_sel_range(a, &sa, &sb);
	if (sa == sb)
		return;
	clip_copy_sel(a);
	buf_delete_range(a, sa, sb);
}

void clip_request_paste(App *a, Atom sel)
{
	a->paste_sel = sel;
	a->paste_phase = 1;
	XConvertSelection(a->dpy, sel, a->xa_utf8, a->xa_paste, a->win, CurrentTime);
}

void clip_handle_selection_request(App *a, XSelectionRequestEvent *req)
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
	if (a->clip && (req->target == a->xa_utf8 || req->target == XA_STRING))
		{
		XChangeProperty(a->dpy, req->requestor, prop, req->target, 8,
		                PropModeReplace, (unsigned char *)a->clip, (int)a->clip_len);
		sev.property = prop;
		}
	else if (req->target == a->xa_targets)
		{
		Atom tg[4];
		tg[0] = a->xa_targets;
		tg[1] = a->xa_utf8;
		tg[2] = XA_STRING;
		tg[3] = a->xa_compound;
		XChangeProperty(a->dpy, req->requestor, prop, XA_ATOM, 32,
		                PropModeReplace, (unsigned char *)tg, 4);
		sev.property = prop;
		}
	XSendEvent(a->dpy, req->requestor, False, 0, (XEvent *)&sev);
}

void clip_handle_selection_notify(App *a, XSelectionEvent *sev)
{
	Atom type;
	int fmt;
	unsigned long nitems, left;
	unsigned char *data = NULL;
	if (!a->paste_phase)
		return;
	if (sev->property == None)
		{
		if (a->paste_phase == 1)
			{
			a->paste_phase = 2;
			XConvertSelection(a->dpy, a->paste_sel, XA_STRING, a->xa_paste, a->win, CurrentTime);
			return;
			}
		a->paste_phase = 0;
		return;
		}
	if (XGetWindowProperty(a->dpy, a->win, a->xa_paste, 0, 1 << 20, True,
	                       AnyPropertyType, &type, &fmt, &nitems, &left, &data) == Success &&
	    data)
		{
		size_t nbytes = fmt == 8 ? nitems : nitems * (size_t)(fmt / 8);
		buf_insert_bytes(a, (char *)data, nbytes);
		disp_reveal_cursor(a);
		XFree(data);
		}
	a->paste_phase = 0;
}   