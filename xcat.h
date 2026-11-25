/* xcat.h — shared types, constants, and the App struct.
 * SPDX-License-Identifier: MIT
 */
#ifndef XCAT_H
#define XCAT_H

#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#define UNDO_MAX 48
#define TAB_COLS 4
#define APP_NAME "xcat"

#define MENU_H   24
#define MENU_PAD 8

#define FILE_BUFFER_SIZE 4096

typedef struct {
    int x, y, w, h;
} Rect;

typedef struct {
    char *text;
    size_t len;
    size_t cur;
} Snap;

typedef struct {
    const char *label;
    const char **items;
    int nitems;
} Menu;

typedef struct {
    /* X11 */
    Display *dpy;
    int scr;
    Window win;
    GC gc;
    Pixmap pm;
    int pm_w, pm_h;
    XftDraw *draw;
    XftFont *font;
    Visual *visual;
    Colormap cmap;
    XIM im;
    XIC ic;

    /* Colors */
    XftColor col_fg, col_bg, col_sel, col_caret, col_muted;
    XftColor col_okbg, col_okfg, col_btn, col_btnfg, col_paper, col_line;

    /* Geometry */
    int win_w, win_h;
    int char_w, line_h, ascent;
    int text_x, text_y, text_w, text_h, status_y;
    Rect ok_r, cancel_r;
    int hot_ok, hot_cancel;

    /* Buffer */
    char *text;
    size_t len, cap, cursor, anchor, top_line, pref_col;
    int selecting;
    int xoff;
    int caret_on;

    size_t *lines;
    size_t nlines, lines_cap;

    /* Undo */
    Snap undo_stack[UNDO_MAX];
    int undo_len, undo_pos;

    /* Clipboard */
    char *clip;
    size_t clip_len;
    int own_clip;

    /* Atoms */
    Atom xa_wm_del, xa_clip, xa_utf8, xa_targets, xa_compound, xa_paste;
    int paste_phase;
    Atom paste_sel;

    /* Menu */
    int menu_open, menu_hot, ctx_mode, ctx_x, ctx_y;
    int menu_drop_x, menu_drop_y, menu_drop_w, menu_drop_h;

    /* Find */
    int find_active;
    char find_buf[256];
    size_t find_len;

    /* Pipe */
    int pipe_active;
    char pipe_buf[512];
    size_t pipe_len;

    /* Misc */
    char *first_file;
    char *title;
    const char *font_name;
    int exit_kind;
} App;

/* --- utf8.c --- */
int utf8_len(const unsigned char *s, size_t avail);
size_t utf8_prev(const char *text, size_t pos);
int cp_width(const unsigned char *s, int n);

/* --- buffer.c --- */
void buf_ensure(App *a, size_t need);
void buf_rebuild_lines(App *a);
size_t buf_line_of(App *a, size_t pos);
size_t buf_line_end(App *a, size_t li);
size_t buf_col_at(App *a, size_t pos);
size_t buf_pos_at_col(App *a, size_t li, size_t want);
void buf_clear_sel(App *a);
void buf_sel_range(App *a, size_t *a_out, size_t *b_out);
void buf_push_undo(App *a);
void buf_undo(App *a);
void buf_delete_range(App *a, size_t a_pos, size_t b_pos);
void buf_insert_bytes(App *a, const char *s, size_t n);
void buf_backspace(App *a);
void buf_delete_fwd(App *a);
void buf_move_left(App *a, int extend);
void buf_move_right(App *a, int extend);
void buf_move_vert(App *a, int dir, int extend);
void buf_move_home(App *a, int extend, int doc);
void buf_move_end(App *a, int extend, int doc);
void buf_select_all(App *a);
void buf_kill_line(App *a);

/* --- display.c --- */
void disp_open_ui(App *a);
void disp_layout(App *a);
void disp_ensure_pixmap(App *a);
void disp_draw_all(App *a);
void disp_reveal_cursor(App *a);
int disp_prefix_px(App *a, size_t line_start, size_t pos);
size_t disp_pos_from_px(App *a, size_t li, int px);
void disp_fill_rect(App *a, int x, int y, int w, int h, unsigned long pixel);
void disp_stroke_rect(App *a, int x, int y, int w, int h, unsigned long pixel);
void disp_draw_button(App *a, Rect r, const char *label, int hot, int primary);

/* --- menu.c --- */
void menu_bar_draw(App *a);
void menu_dropdown_geom(App *a);
int menu_title_x(App *a, int i);
void menu_execute(App *a, int mi, int item);

/* --- find.c --- */
void find_bar_draw(App *a);
void find_next(App *a);
void find_open(App *a);
void find_close(App *a);

/* --- filepicker.c --- */
void fp_open(App *a, int save_mode, int replace);
void fp_close(App *a);
void fp_draw(App *a);
void fp_handle_key(App *a, XKeyEvent *ev);
void fp_handle_button(App *a, XButtonEvent *ev);
int fp_is_active(App *a);

/* --- clipboard.c --- */
void clip_copy_sel(App *a);
void clip_cut_sel(App *a);
void clip_request_paste(App *a, Atom sel);
void clip_handle_selection_request(App *a, XSelectionRequestEvent *req);
void clip_handle_selection_notify(App *a, XSelectionEvent *sev);
void buf_delete_sel(App *a); 

/* --- fileio.c --- */
void fileio_append_fd(App *a, int fd, const char *what);
void fileio_load_inputs(App *a, int nfiles, char **files);
void fileio_pipe_open(App *a);
void fileio_pipe_close(App *a);
void fileio_pipe_run(App *a);
void fileio_save_to(App *a, const char *path);

/* --- input.c --- */
void input_handle_key(App *a, XKeyEvent *ev);
void input_place_click(App *a, int x, int y, int extend);
void input_scroll_by(App *a, int lines_delta);
int input_in_rect(Rect r, int x, int y);

/* --- main.c (shared helpers) --- */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
void die(const char *msg, int code);
int tty_msg(const char *msg);
void accept(App *a);
void cancel(App *a);
void usage(void);
int x_io_error(Display *d);

#endif /* XCAT_H */   
