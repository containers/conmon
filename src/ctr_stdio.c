#include "ctr_stdio.h"
#include "globals.h"
#include "config.h"
#include "conn_sock.h"
#include "utils.h"
#include "ctr_logging.h"
#include "cli.h"

#include <stdbool.h>
#include <sys/socket.h>

static gboolean tty_hup_timeout_scheduled = false;
static bool read_stdio(int fd, stdpipe_t pipe, gboolean *eof);
static void drain_log_buffers(stdpipe_t pipe);
static gboolean tty_hup_timeout_cb(G_GNUC_UNUSED gpointer user_data);


gboolean stdio_cb(int fd, GIOCondition condition, gpointer user_data)
{
	stdpipe_t pipe = GPOINTER_TO_INT(user_data);
	gboolean read_eof = FALSE;
	gboolean has_input = (condition & G_IO_IN) != 0;
	gboolean has_hup = (condition & G_IO_HUP) != 0;

	/* When we get here, condition can be G_IO_IN and/or G_IO_HUP.
	   IN means there is some data to read.
	   HUP means the other side closed the fd. In the case of a pine
	   this in final, and we will never get more data. However, in the
	   terminal case this just means that nobody has the terminal
	   open at this point, and this can be change whenever someone
	   opens the tty */

	/* Read any data before handling hup */
	if (has_input) {
		read_stdio(fd, pipe, &read_eof);
	}

	if (has_hup && opt_terminal && pipe == STDOUT_PIPE) {
		/* We got a HUP from the terminal main this means there
		   are no open workers ptys atm, and we will get a lot
		   of wakeups until we have one, switch to polling
		   mode. */

		/* If we read some data this cycle, wait one more, maybe there
		   is more in the buffer before we handle the hup */
		if (has_input && !read_eof) {
			return G_SOURCE_CONTINUE;
		}

		if (!tty_hup_timeout_scheduled) {
			g_timeout_add(100, tty_hup_timeout_cb, NULL);
		}
		tty_hup_timeout_scheduled = true;
		return G_SOURCE_REMOVE;
	}

	/* End of input */
	if (read_eof || (has_hup && !has_input)) {
		/* There exists a case that the process has already exited
		 * and we know about it (because we checked our child processes)
		 * but we needed to run the main_loop to catch all the rest of the output
		 * (specifically, when we are exec, but not terminal)
		 * In this case, after both the stderr and stdout pipes have closed
		 * we should quit the loop. Otherwise, conmon will hang forever
		 * waiting for container_exit_cb that will never be called.
		 */
		if (pipe == STDOUT_PIPE) {
			mainfd_stdout = -1;
			if (container_status >= 0 && mainfd_stderr < 0) {
				g_main_loop_quit(main_loop);
			}
		}
		if (pipe == STDERR_PIPE) {
			mainfd_stderr = -1;
			if (container_status >= 0 && mainfd_stdout < 0) {
				g_main_loop_quit(main_loop);
			}
		}

		close(fd);
		return G_SOURCE_REMOVE;
	}

	return G_SOURCE_CONTINUE;
}

void drain_stdio()
{
	if (mainfd_stdout != -1) {
		g_unix_set_fd_nonblocking(mainfd_stdout, TRUE, NULL);
		while (read_stdio(mainfd_stdout, STDOUT_PIPE, NULL))
			;
	}
	drain_log_buffers(STDOUT_PIPE);
	if (mainfd_stderr != -1) {
		g_unix_set_fd_nonblocking(mainfd_stderr, TRUE, NULL);
		while (read_stdio(mainfd_stderr, STDERR_PIPE, NULL))
			;
	}
	drain_log_buffers(STDERR_PIPE);
}

/* the journald log writer is buffering partial lines so that whole log lines are emitted
 * to the journal as a unit. this flushes those buffers */
static void drain_log_buffers(stdpipe_t pipe)
{
	/* We pass a single byte buffer because write_to_logs expects that there is one
	   byte of capacity beyond the buflen that we specify */
	char buf;
	write_to_logs(pipe, &buf, 0);
}

static bool read_stdio(int fd, stdpipe_t pipe, gboolean *eof)
{
	/* We use two extra bytes. One at the start, which we don't read into, instead
	   we use that for marking the pipe when we write to the attached socket.
	   One at the end to guarantee a null-terminated buffer for journald logging*/

	char real_buf[STDIO_BUF_SIZE + 2];
	char *buf = real_buf + 1;
	ssize_t num_read = 0;

	if (eof)
		*eof = false;

	num_read = read(fd, buf, STDIO_BUF_SIZE);
	if (num_read == 0) {
		if (eof)
			*eof = true;
		return false;
	} else if (num_read < 0) {
		nwarnf("stdio_input read failed %s", strerror(errno));
		return false;
	} else {
		// Always null terminate the buffer, just in case.
		buf[num_read] = '\0';

		bool written = write_to_logs(pipe, buf, num_read);
		if (!written)
			return written;

		real_buf[0] = pipe;
		write_back_to_remote_consoles(real_buf, num_read + 1);
		return true;
	}
}


static gboolean tty_hup_timeout_cb(G_GNUC_UNUSED gpointer user_data)
{
	tty_hup_timeout_scheduled = false;
	g_unix_fd_add(mainfd_stdout, G_IO_IN, stdio_cb, GINT_TO_POINTER(STDOUT_PIPE));
	return G_SOURCE_REMOVE;
}
