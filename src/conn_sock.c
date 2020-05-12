#define _GNU_SOURCE

#include "conn_sock.h"
#include "globals.h"
#include "utils.h"
#include "config.h"
#include "cli.h" // opt_stdin

#include <stdbool.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sys/un.h>
#include <sys/stat.h>

static gboolean attach_cb(int fd, G_GNUC_UNUSED GIOCondition condition, G_GNUC_UNUSED gpointer user_data);
static gboolean conn_sock_cb(int fd, GIOCondition condition, gpointer user_data);
static gboolean read_conn_sock(struct conn_sock_s *sock);
static gboolean terminate_conn_sock(struct conn_sock_s *sock);
void conn_sock_shutdown(struct conn_sock_s *sock, int how);
static void sock_try_write_to_masterfd_stdin(struct conn_sock_s *sock);
static gboolean masterfd_write_cb(G_GNUC_UNUSED int fd, G_GNUC_UNUSED GIOCondition condition, G_GNUC_UNUSED gpointer user_data);

char *setup_console_socket(void)
{
	struct sockaddr_un addr = {0};
	_cleanup_free_ const char *tmpdir = g_get_tmp_dir();
	_cleanup_free_ char *csname = g_build_filename(tmpdir, "conmon-term.XXXXXX", NULL);
	/*
	 * Generate a temporary name. Is this unsafe? Probably, but we can
	 * replace it with a rename(2) setup if necessary.
	 */

	int unusedfd = g_mkstemp(csname);
	if (unusedfd < 0)
		pexit("Failed to generate random path for console-socket");
	close(unusedfd);

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, csname, sizeof(addr.sun_path) - 1);

	ninfof("addr{sun_family=AF_UNIX, sun_path=%s}", addr.sun_path);

	/* Bind to the console socket path. */
	console_socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (console_socket_fd < 0)
		pexit("Failed to create console-socket");
	if (fchmod(console_socket_fd, 0700))
		pexit("Failed to change console-socket permissions");
	/* XXX: This should be handled with a rename(2). */
	if (unlink(csname) < 0)
		pexit("Failed to unlink temporary random path");
	if (bind(console_socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		pexit("Failed to bind to console-socket");
	if (listen(console_socket_fd, 128) < 0)
		pexit("Failed to listen on console-socket");

	return g_strdup(csname);
}

char *setup_attach_socket(void)
{
	struct sockaddr_un attach_addr = {0};
	attach_addr.sun_family = AF_UNIX;

	/*
	 * Create a symlink so we don't exceed unix domain socket
	 * path length limit.
	 */
	char *attach_symlink_dir_path = g_build_filename(opt_socket_path, opt_cuuid, NULL);
	if (unlink(attach_symlink_dir_path) == -1 && errno != ENOENT)
		pexit("Failed to remove existing symlink for attach socket directory");

	/*
	 * This is to address a corner case where the symlink path length can end up being
	 * the same as the socket.  When it happens, the symlink prevents the socket from being
	 * be created.  This could still be a problem with other containers, but it is safe
	 * to assume the CUUIDs don't change length in the same directory.  As a workaround,
	 *  in such case, make the symlink one char shorter.
	 */
	if (strlen(attach_symlink_dir_path) == (sizeof(attach_addr.sun_path) - 1))
		attach_symlink_dir_path[sizeof(attach_addr.sun_path) - 2] = '\0';

	if (symlink(opt_bundle_path, attach_symlink_dir_path) == -1)
		pexit("Failed to create symlink for attach socket");

	_cleanup_free_ char *attach_sock_path = g_build_filename(opt_socket_path, opt_cuuid, "attach", NULL);
	ninfof("attach sock path: %s", attach_sock_path);

	strncpy(attach_addr.sun_path, attach_sock_path, sizeof(attach_addr.sun_path) - 1);
	ninfof("addr{sun_family=AF_UNIX, sun_path=%s}", attach_addr.sun_path);

	/*
	 * We make the socket non-blocking to avoid a race where client aborts connection
	 * before the server gets a chance to call accept. In that scenario, the server
	 * accept blocks till a new client connection comes in.
	 */
	attach_socket_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (attach_socket_fd == -1)
		pexit("Failed to create attach socket");

	if (fchmod(attach_socket_fd, 0700))
		pexit("Failed to change attach socket permissions");

	if (unlink(attach_addr.sun_path) == -1 && errno != ENOENT)
		pexitf("Failed to remove existing attach socket: %s", attach_addr.sun_path);

	if (bind(attach_socket_fd, (struct sockaddr *)&attach_addr, sizeof(struct sockaddr_un)) == -1)
		pexitf("Failed to bind attach socket: %s", attach_sock_path);

	if (listen(attach_socket_fd, 10) == -1)
		pexitf("Failed to listen on attach socket: %s", attach_sock_path);

	g_unix_fd_add(attach_socket_fd, G_IO_IN, attach_cb, NULL);

	return attach_symlink_dir_path;
}

static gboolean attach_cb(int fd, G_GNUC_UNUSED GIOCondition condition, G_GNUC_UNUSED gpointer user_data)
{
	int conn_fd = accept(fd, NULL, NULL);
	if (conn_fd == -1) {
		if (errno != EWOULDBLOCK)
			nwarn("Failed to accept client connection on attach socket");
	} else {
		struct conn_sock_s *conn_sock;
		if (conn_socks == NULL) {
			conn_socks = g_ptr_array_new_with_free_func(free);
		}
		conn_sock = malloc(sizeof(*conn_sock));
		if (conn_sock == NULL) {
			pexit("Failed to allocate memory");
		}
		conn_sock->fd = conn_fd;
		conn_sock->readable = true;
		conn_sock->writable = true;
		conn_sock->off = 0;
		conn_sock->remaining = 0;
		conn_sock->data_ready = false;
		g_unix_fd_add(conn_sock->fd, G_IO_IN | G_IO_HUP | G_IO_ERR, conn_sock_cb, conn_sock);
		g_ptr_array_add(conn_socks, conn_sock);
		ninfof("Accepted connection %d", conn_sock->fd);
	}

	return G_SOURCE_CONTINUE;
}

static gboolean conn_sock_cb(G_GNUC_UNUSED int fd, GIOCondition condition, gpointer user_data)
{
	struct conn_sock_s *sock = (struct conn_sock_s *)user_data;

	if (condition & G_IO_IN)
		return read_conn_sock(sock);

	return terminate_conn_sock(sock);
}

static gboolean read_conn_sock(struct conn_sock_s *sock)
{
	ssize_t num_read;

	/* There is still data in the buffer.  */
	if (sock->remaining) {
		sock->data_ready = true;
		return G_SOURCE_REMOVE;
	}

	num_read = read(sock->fd, sock->buf, CONN_SOCK_BUF_SIZE);
	if (num_read < 0)
		return G_SOURCE_CONTINUE;

	if (num_read == 0)
		return terminate_conn_sock(sock);

	/* num_read > 0 */
	sock->remaining = num_read;
	sock->off = 0;

	sock_try_write_to_masterfd_stdin(sock);

	/* Not everything was written to stdin, let's wait for the fd to be ready.  */
	if (sock->remaining)
		schedule_master_stdin_write();
	return G_SOURCE_CONTINUE;
}

static gboolean terminate_conn_sock(struct conn_sock_s *sock)
{
	conn_sock_shutdown(sock, SHUT_RD);
	if (masterfd_stdin >= 0 && opt_stdin) {
		if (!opt_leave_stdin_open) {
			close(masterfd_stdin);
			masterfd_stdin = -1;
		} else {
			ninfo("Not closing input");
		}
	}
	return G_SOURCE_REMOVE;
}

void conn_sock_shutdown(struct conn_sock_s *sock, int how)
{
	if (sock->fd == -1)
		return;
	shutdown(sock->fd, how);
	switch (how) {
	case SHUT_RD:
		sock->readable = false;
		break;
	case SHUT_WR:
		sock->writable = false;
		break;
	case SHUT_RDWR:
		sock->readable = false;
		sock->writable = false;
		break;
	}
	if (!sock->writable && !sock->readable) {
		close(sock->fd);
		sock->fd = -1;
		g_ptr_array_remove(conn_socks, sock);
	}
}

static void write_to_masterfd_stdin(gpointer data, gpointer user_data)
{
	struct conn_sock_s *sock = (struct conn_sock_s *)data;
	bool *has_data = user_data;

	sock_try_write_to_masterfd_stdin(sock);

	if (sock->remaining)
		*has_data = true;
	else if (sock->data_ready) {
		sock->data_ready = false;
		g_unix_fd_add(sock->fd, G_IO_IN | G_IO_HUP | G_IO_ERR, conn_sock_cb, sock);
	}
}

static void sock_try_write_to_masterfd_stdin(struct conn_sock_s *sock)
{
	if (!sock->remaining || masterfd_stdin < 0)
		return;

	ssize_t w = write(masterfd_stdin, sock->buf + sock->off, sock->remaining);
	if (w < 0) {
		nwarn("Failed to write to container stdin");
	} else {
		sock->off += w;
		sock->remaining -= w;
	}
}

static gboolean masterfd_write_cb(G_GNUC_UNUSED int fd, G_GNUC_UNUSED GIOCondition condition, G_GNUC_UNUSED gpointer user_data)
{
	bool has_data = FALSE;

	if (masterfd_stdin < 0)
		return G_SOURCE_REMOVE;

	g_ptr_array_foreach(conn_socks, write_to_masterfd_stdin, &has_data);
	if (has_data)
		return G_SOURCE_CONTINUE;
	return G_SOURCE_REMOVE;
}

void schedule_master_stdin_write()
{
	g_unix_fd_add(masterfd_stdin, G_IO_OUT, masterfd_write_cb, NULL);
}
