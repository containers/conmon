#define _GNU_SOURCE

#include "self_pipe.h"
#include "globals.h" // self_pipe_w

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <unistd.h>

/* GLib source tag for the self-pipe read-end watcher. */
static int self_pipe_tag = -1;
/* Read end of the self-pipe (for cleanup). */
static int self_pipe_r_fd = -1;

/*
 * Initialize the self-pipe mechanism.
 * Creates a non-blocking, close-on-exec pipe and registers a GLib IO source
 * on the read end. The write end is stored in the global self_pipe_w.
 */
int self_pipe_init(gboolean (*callback)(gint fd, GIOCondition condition, gpointer user_data), gpointer user_data)
{
	int pipefd[2];

	if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0) {
		return -1;
	}

	self_pipe_w = pipefd[1];
	self_pipe_r_fd = pipefd[0];
	self_pipe_tag = g_unix_fd_add(self_pipe_r_fd, G_IO_IN, callback, user_data);
	return 0;
}

/*
 * Clean up the self-pipe: remove the GLib source and close both ends.
 */
void self_pipe_fini(void)
{
	if (self_pipe_tag >= 0) {
		g_source_remove(self_pipe_tag);
		self_pipe_tag = -1;
	}
	if (self_pipe_r_fd >= 0) {
		close(self_pipe_r_fd);
		self_pipe_r_fd = -1;
	}
	if (self_pipe_w >= 0) {
		close(self_pipe_w);
		self_pipe_w = -1;
	}
}

/*
 * Wake up the GLib main loop by writing a byte to the self-pipe.
 * This function is safe to call from a signal handler (async-signal-safe).
 * errno is preserved around the write() since signal handlers must not
 * clobber it (POSIX section 2.4.3).
 * The write() return value is intentionally ignored: if the pipe buffer is
 * full, a previous wake is already pending and will drain soon enough.
 */
void self_pipe_wake(void)
{
	if (self_pipe_w >= 0) {
		int saved_errno = errno;
		char c = 'x';
		gssize ret __attribute__((unused)) = write(self_pipe_w, &c, 1);
		errno = saved_errno;
	}
}
