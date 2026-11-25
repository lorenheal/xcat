/* menu.c — menu bar, dropdown, and menu action dispatch.
 * SPDX-License-Identifier: MIT
 */
#include "menu.h"
#include "find.h"
#include "clipboard.h"
#include "filepicker.h"
#include "fileio.h"
#include <string.h>

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

const char *menu_label(int i)
{
	return menus[i].label;
}

int menu_nitems(int i)
{
	return menus[i].nitems;
}   

int menu_title_x(App *a, int i)
{
	int x = MENU_PAD;
	int j;
	for (j = 0; j < i; j++)
		{
		XGlyphInfo ext;
		XftTextExtentsUtf8(a->dpy, a->font, (FcChar8 *)menus[j].label,
		                   (int)strlen(menus[j].label), &ext);
		x += ext.xOff + 24;
		}
	return x;
}

void menu_dropdown_geom(App *a)
{
	int i, wmax = 0;
	XGlyphInfo ext;
	if (a->menu_open < 0)
		return;
	if (a->ctx_mode)
		{
		a->menu_drop_x = a->ctx_x;
		a->menu_drop_y = a->ctx_y;
		}
	else
		{
		a->menu_drop_x = menu_title_x(a, a->menu_open) - 2;
		a->menu_drop_y = MENU_H + 6;
		}
	for (i = 0; i < menus[a->menu_open].nitems; i++)
		{
		XftTextExtentsUtf8(a->dpy, a->font, (FcChar8 *)menus[a->menu_open].items[i],
		                   (int)strlen(menus[a->menu_open].items[i]), &ext);
		if (ext.xOff + 32 > wmax)
			wmax = ext.xOff + 32;
		}
	a->menu_drop_w = wmax;
	a->menu_drop_h = menus[a->menu_open].nitems * a->line_h + 6;
}

void menu_bar_draw(App *a)
{
	int i, x;
	disp_fill_rect(a, 0, 0, a->win_w, MENU_H, a->col_paper.pixel);
	disp_fill_rect(a, 0, MENU_H - 1, a->win_w, 1, a->col_line.pixel);

	for (i = 0; i < NMENUS; i++)
		{
		XGlyphInfo ext;
		int tx;
		x = menu_title_x(a, i);
		XftTextExtentsUtf8(a->dpy, a->font, (FcChar8 *)menus[i].label,
		                   (int)strlen(menus[i].label), &ext);
		if (i == a->menu_open && !a->ctx_mode)
			disp_fill_rect(a, x - 4, 0, ext.xOff + 8, MENU_H, a->col_sel.pixel);
		tx = x;
		XftDrawStringUtf8(a->draw, &a->col_fg, a->font, tx, MENU_H - (MENU_H - a->ascent) / 2,
		                  (FcChar8 *)menus[i].label, (int)strlen(menus[i].label));
		}

	if (a->menu_open >= 0)
		{
		menu_dropdown_geom(a);
		disp_fill_rect(a, a->menu_drop_x, a->menu_drop_y, a->menu_drop_w, a->menu_drop_h, a->col_paper.pixel);
		disp_stroke_rect(a, a->menu_drop_x, a->menu_drop_y, a->menu_drop_w, a->menu_drop_h, a->col_line.pixel);
		for (i = 0; i < menus[a->menu_open].nitems; i++)
			{
			int iy = a->menu_drop_y + 7 + i * a->line_h;
			if (i == a->menu_hot)
				disp_fill_rect(a, a->menu_drop_x + 1, iy, a->menu_drop_w - 2, a->line_h, a->col_sel.pixel);
			XftDrawStringUtf8(a->draw, &a->col_fg, a->font,
			                  a->menu_drop_x + 12, iy + a->ascent,
			                  (FcChar8 *)menus[a->menu_open].items[i],
			                  (int)strlen(menus[a->menu_open].items[i]));
			}
		}
}

/* --- menu actions --- */

static void file_open(App *a)
{
	fp_open(a, 0, 1);
}

static void file_insert(App *a)
{
	fp_open(a, 0, 0);
}

static void file_save(App *a)
{
	fp_open(a, 1, 0);
}

static void file_quit(App *a)
{
	cancel(a);
}

void menu_execute(App *a, int mi, int item)
{
	switch (mi)
		{
		case 0: /* File */
			switch (item)
				{
				case 0:
					file_open(a);
					break;
				case 1:
					file_insert(a);
					break;
				case 2:
					file_save(a);
					break;
				case 3:
					fileio_pipe_open(a);
					break;
				case 4:
					file_quit(a);
					break;
				}
			break;
		case 1: /* Edit */
			switch (item)
				{
				case 0:
					clip_cut_sel(a);
					disp_reveal_cursor(a);
					break;
				case 1:
					clip_copy_sel(a);
					break;
				case 2:
					clip_request_paste(a, a->xa_clip);
					break;
				}
			break;
		case 2: /* Search */
			switch (item)
				{
				case 0:
					find_open(a);
					break;
				}
			break;
		}
	a->ctx_mode = 0;
	a->menu_open = -1;
	a->menu_hot = -1;
}   
