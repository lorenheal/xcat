/* fileio.c — file I/O: append, load inputs, pipe, and save.
 * SPDX-License-Identifier: MIT
 */
#include "fileio.h"
#include "buffer.h"
#include "display.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

void fileio_append_fd(App *a, int fd, const char *what)
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
		buf_ensure(a, a->len + (size_t)n + 1);
		memcpy(a->text + a->len, buf, (size_t)n);
		a->len += (size_t)n;
		a->text[a->len] = 0;
		}
}

void fileio_load_inputs(App *a, int nfiles, char **files)
{
	int i, saw_stdin = 0;
	a->text = xmalloc(1);
	a->text[0] = 0;
	a->cap = 1;
	a->len = 0;
	if (nfiles == 0)
		{
		if (!isatty(STDIN_FILENO))
			fileio_append_fd(a, STDIN_FILENO, "stdin");
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
					fileio_append_fd(a, STDIN_FILENO, "stdin");
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
		fileio_append_fd(a, fd, files[i]);
		close(fd);
		}
	if (saw_stdin)
		close(STDIN_FILENO);
}

void fileio_pipe_open(App *a)
{
	a->pipe_active = 1;
	a->pipe_len = 0;
	a->pipe_buf[0] = 0;
	disp_layout(a);
	disp_draw_all(a);
}

void fileio_pipe_close(App *a)
{
	a->pipe_active = 0;
	a->pipe_buf[0] = 0;
	a->pipe_len = 0;
	disp_layout(a);
}

void fileio_pipe_run(App *a)
{
	char tmp[] = "/tmp/xcatXXXXXX";
	int tfd;
	FILE *p;
	char cmd[600];
	char *out;
	size_t out_len, out_cap;
	char buf[8192];
	ssize_t n;

	if (a->pipe_len == 0)
		{
		fileio_pipe_close(a);
		return;
		}
	tfd = mkstemp(tmp);
	if (tfd < 0)
		{
		tty_msg("cannot create temp file");
		fileio_pipe_close(a);
		return;
		}
	{
	size_t off = 0;
	while (off < a->len)
		{
		ssize_t w = write(tfd, a->text + off, a->len - off);
		if (w < 0)
			{
			if (errno == EINTR) continue;
			close(tfd);
			unlink(tmp);
			tty_msg(strerror(errno));
			fileio_pipe_close(a);
			return;
			}
		off += (size_t)w;
		}
	}
	close(tfd);

	snprintf(cmd, sizeof cmd, "%s < %s", a->pipe_buf, tmp);
	p = popen(cmd, "r");
	if (!p)
		{
		unlink(tmp);
		tty_msg("cannot run command");
		fileio_pipe_close(a);
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

	buf_push_undo(a);
	buf_ensure(a, out_len + 1);
	if (out)
		memcpy(a->text, out, out_len);
	a->text[out_len] = 0;
	a->len = out_len;
	a->cursor = 0;
	buf_clear_sel(a);
	buf_rebuild_lines(a);
	free(out);
	fileio_pipe_close(a);
	disp_reveal_cursor(a);
}

void fileio_save_to(App *a, const char *path)
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