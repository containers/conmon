#define _GNU_SOURCE

#include "conn_sock.h"
#include "globals.h"
#include "utils.h"
#include "config.h"
#include "cli.h" // opt_stdin

#include <libgen.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sys/un.h>
#include <sys/stat.h>

static gboolean attach_cb(int fd, G_GNUC_UNUSED GIOCondition condition, gpointer user_data);
static gboolean remote_sock_cb(int fd, GIOCondition condition, gpointer user_data);
static void init_remote_sock(struct remote_sock_s *sock, struct remote_sock_s *src);
static gboolean read_remote_sock(struct remote_sock_s *sock);
static gboolean terminate_remote_sock(struct remote_sock_s *sock);
static void remote_sock_shutdown(struct remote_sock_s *sock, int how);
static void schedule_local_sock_write(struct local_sock_s *local_sock);
static void sock_try_write_to_local_sock(struct remote_sock_s *sock);
static gboolean local_sock_write_cb(G_GNUC_UNUSED int fd, G_GNUC_UNUSED GIOCondition condition, G_GNUC_UNUSED gpointer user_data);
static char *bind_unix_socket(char *socket_relative_name, int sock_type, mode_t perms, struct remote_sock_s *remote_sock,
			      gboolean use_full_attach_path);
static char *socket_parent_dir(gboolean use_full_attach_path, size_t desired_len);
static char *setup_socket(int *fd, const char *path);
/*
  Since our socket handling is abstract now, handling is based on sock_type, so we can pass around a structure
  that contains everything we need to handle I/O.  Callbacks used to handle IO, for example, and whether this
  can be read from or written to or both, and the buffers used for the communication.
*/

/*
  This defines the Container STDIN, attaches it to the correct FD and sets the flags for handling I/O.
  setup_attach_socket() is responsible for setting the correct remote FD and pushing it onto the queue.
*/
static struct local_sock_s local_mainfd_stdin = {&mainfd_stdin, true, NULL, "container stdin", NULL};
struct remote_sock_s remote_attach_sock = {
	SOCK_TYPE_CONSOLE,   /* sock_type */
	-1,		     /* fd */
	&local_mainfd_stdin, /* dest */
	true,		     /* listening */
	false,		     /* data_ready */
	true,		     /* readable */
	true,		     /* writable */
	0,		     /* remaining */
	0,		     /* off */
	{0}		     /* buf */
};
/*
  This defines the Container SDNotify socket, attaches it to the correct FD and sets the flags for handling I/O.
  setup_notify_socket() is responsible for initializing the unix sockets and pushing it onto the queue.

  If the local_notify_host_fd stays -1 (i.e. we have not requested SD-NOTIFY) then setup was never run and
  this has no effect.
*/
static int local_notify_host_fd = -1;
static struct sockaddr_un local_notify_host_addr = {0};
static struct local_sock_s local_notify_host = {&local_notify_host_fd, false, NULL, "host notify socket", &local_notify_host_addr};
struct remote_sock_s remote_notify_sock = {
	SOCK_TYPE_NOTIFY,   /* sock_type */
	-1,		    /* fd */
	&local_notify_host, /* dest */
	false,		    /* listening */
	false,		    /* data_ready */
	true,		    /* readable */
	false,		    /* writable */
	0,		    /* remaining */
	0,		    /* off */
	{0}		    /* buf */
};

/* External */

char *setup_console_socket(void)
{
	return setup_socket(&console_socket_fd, NULL);
}

char *setup_seccomp_socket(const char *socket)
{
	return setup_socket(&seccomp_socket_fd, socket);
}

#ifdef __linux__
static void bind_relative_to_dir(int dir_fd, int sock_fd, const char *path)
{
	struct sockaddr_un addr;

	/*
	 * To be able to access the location of the attach socket, without first creating the attach socket
	 * but also be able to handle arbitrary length paths, we open the parent dir (base_path), and then use
	 * the corresponding entry in `/proc/self/fd` to act as the path to base_path, then we use the socket_relative_name
	 * to actually refer to the file where the socket will be created below.
	 */
	addr.sun_family = AF_UNIX;
	if (dir_fd == -1) {
		strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	} else {
		snprintf(addr.sun_path, sizeof(addr.sun_path) - 1, "/proc/self/fd/%d/%s", dir_fd, path);
	}
	ndebugf("addr{sun_family=AF_UNIX, sun_path=%s}", addr.sun_path);

	if (fchmod(sock_fd, 0700))
		pexit("Failed to change console-socket permissions");
	if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		pexit("Failed to bind to console-socket");
}
#endif

#ifdef __FreeBSD__

// FreeBSD earlier than 13.1-RELEASE doesn't have O_PATH
#ifndef O_PATH
#define O_PATH 0
#endif

static void bind_relative_to_dir(int dir_fd, int sock_fd, const char *path)
{
	struct sockaddr_un addr;

	if (dir_fd == -1) {
		dir_fd = AT_FDCWD;
	}

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	ndebugf("addr{sun_family=AF_UNIX, sun_path=%s}", addr.sun_path);
	if (bindat(dir_fd, sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
		pexit("Failed to bind to console-socket");
	if (fchmodat(dir_fd, addr.sun_path, 0700, AT_SYMLINK_NOFOLLOW))
		pexit("Failed to change console-socket permissions");
}
#endif

static char *setup_socket(int *fd, const char *path)
{
	char *csname = NULL;
	char *bname = NULL;
	_cleanup_close_ int sfd = -1;

	if (path != NULL) {
		_cleanup_free_ char *dname_buf = NULL;
		_cleanup_free_ char *bname_buf = NULL;
		char *dname = NULL, *bname = NULL;

		csname = strdup(path);
		dname_buf = strdup(path);
		bname_buf = strdup(path);
		if (csname == NULL || dname_buf == NULL || bname_buf == NULL) {
			pexit("Failed to allocate memory");
			return NULL;
		}
		dname = dirname(dname_buf);
		if (dname == NULL)
			pexitf("Cannot get dirname for %s", csname);

		sfd = open(dname, O_CREAT | O_PATH, 0600);
		if (sfd < 0)
			pexit("Failed to create file for console-socket");

		bname = basename(bname_buf);
		if (bname == NULL)
			pexitf("Cannot get basename for %s", csname);
	} else {
		_cleanup_free_ const char *tmpdir = g_get_tmp_dir();

		csname = g_build_filename(tmpdir, "conmon-term.XXXXXX", NULL);
		/*
		 * Generate a temporary name. Is this unsafe? Probably, but we can
		 * replace it with a rename(2) setup if necessary.
		 */
		int unusedfd = g_mkstemp(csname);
		if (unusedfd < 0)
			pexit("Failed to generate random path for console-socket");
		close(unusedfd);
		/* XXX: This should be handled with a rename(2). */
		if (unlink(csname) < 0)
			pexit("Failed to unlink temporary random path");

		bname = csname;
	}

	/* Bind to the console socket path. */
	*fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (*fd < 0)
		pexit("Failed to create socket");
	bind_relative_to_dir(sfd, *fd, bname);
	if (listen(*fd, 128) < 0)
		pexit("Failed to listen on console-socket");

	return csname;
}

char *setup_attach_socket(void)
{
	char *symlink_dir_path =
		bind_unix_socket("attach", SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0700, &remote_attach_sock, opt_full_attach_path);

	if (listen(remote_attach_sock.fd, 10) == -1)
		pexitf("Failed to listen on attach socket: %s/%s", symlink_dir_path, "attach");

	g_unix_fd_add(remote_attach_sock.fd, G_IO_IN, attach_cb, &remote_attach_sock);

	return symlink_dir_path;
}

void setup_notify_socket(char *socket_path)
{
	/* Connect to Host socket */
	if (local_notify_host_fd < 0) {
		local_notify_host_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		if (local_notify_host_fd == -1) {
			pexit("Failed to create notify socket");
		}
		local_notify_host_addr.sun_family = AF_UNIX;
		strncpy(local_notify_host_addr.sun_path, socket_path, sizeof(local_notify_host_addr.sun_path) - 1);
	}

	/* No _cleanup_free_ here so we don't get a warning about unused variables
	 * when compiling with clang */
	char *symlink_dir_path =
		bind_unix_socket("notify/notify.sock", SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0777, &remote_notify_sock, TRUE);
	g_unix_fd_add(remote_notify_sock.fd, G_IO_IN | G_IO_HUP | G_IO_ERR, remote_sock_cb, &remote_notify_sock);

	g_free(symlink_dir_path);
}

static size_t max_socket_path_len()
{
	struct sockaddr_un addr;
	return sizeof(addr.sun_path);
}

/* REMEMBER to g_free() the return value! */
static char *bind_unix_socket(char *socket_relative_name, int sock_type, mode_t perms, struct remote_sock_s *remote_sock,
			      gboolean use_full_attach_path)
{
	int socket_fd = -1;

	/* get the parent_dir of the socket. We'll use this to get the location of the socket. */
	char *parent_dir = socket_parent_dir(use_full_attach_path, max_socket_path_len());

	/*
	 * To be able to access the location of the attach socket, without first creating the attach socket
	 * but also be able to handle arbitrary length paths, we open the parent dir (base_path), and then use
	 * the corresponding entry in `/proc/self/fd` to act as the path to base_path, then we use the socket_relative_name
	 * to actually refer to the file where the socket will be created below.
	 */
	_cleanup_close_ int parent_dir_fd = open(parent_dir, O_PATH);
	if (parent_dir_fd < 0)
		pexitf("failed to open socket path parent dir %s", parent_dir);

	/*
	 * We use the fullpath for operations that aren't as limited in length as socket_addr.sun_path
	 * Cleanup of this variable is up to the caller
	 */
	char *sock_fullpath = g_build_filename(parent_dir, socket_relative_name, NULL);

	/*
	 * We make the socket non-blocking to avoid a race where client aborts connection
	 * before the server gets a chance to call accept. In that scenario, the server
	 * accept blocks till a new client connection comes in.
	 */
	socket_fd = socket(AF_UNIX, sock_type, 0);
	if (socket_fd == -1)
		pexitf("Failed to create socket %s", sock_fullpath);

	if (unlink(sock_fullpath) == -1 && errno != ENOENT)
		pexitf("Failed to remove existing socket: %s", sock_fullpath);

	bind_relative_to_dir(parent_dir_fd, socket_fd, socket_relative_name);

	if (chmod(sock_fullpath, perms))
		pexitf("Failed to change socket permissions %s", sock_fullpath);

	remote_sock->fd = socket_fd;

	return sock_fullpath;
}

/*
 * socket_parent_dir decides whether to truncate the socket path, to match
 * the caller's expectation.
 * use_full_attach_path is whether conmon was told to not truncate the path.
 * base_path is the path of the socket
 * desired_len is the length of socket_addr.sun_path (should be strlen(char[108]) on linux).
 */
char *socket_parent_dir(gboolean use_full_attach_path, size_t desired_len)
{
	/* if we're to use the full path, ignore the socket path and only use the bundle_path */
	if (use_full_attach_path)
		return opt_bundle_path;

	char *base_path = g_build_filename(opt_socket_path, opt_cuuid, NULL);

	/*
	 * This is to address a corner case where the symlink path length can end up being
	 * the same as the socket.  When it happens, the symlink prevents the socket from being
	 * be created.  This could still be a problem with other containers, but it is safe
	 * to assume the CUUIDs don't change length in the same directory.  As a workaround,
	 *  in such case, make the symlink one char shorter.
	 *
	 * If we're using using_full_attach_path, this is unnecessary.
	 */
	if (strlen(base_path) == (desired_len - 1))
		base_path[desired_len - 2] = '\0';

	/*
	 * Create a symlink so we don't exceed unix domain socket
	 * path length limit.  We use the base path passed in from our parent.
	 */
	if (unlink(base_path) == -1 && errno != ENOENT)
		pexitf("Failed to remove existing symlink for socket directory %s", base_path);

	if (symlink(opt_bundle_path, base_path) == -1)
		pexit("Failed to create symlink for notify socket");

	return base_path;
}


void schedule_main_stdin_write()
{
	schedule_local_sock_write(&local_mainfd_stdin);
}

void write_back_to_remote_consoles(char *buf, int len)
{
	if (local_mainfd_stdin.readers == NULL)
		return;

	for (int i = local_mainfd_stdin.readers->len; i > 0; i--) {
		struct remote_sock_s *remote_sock = g_ptr_array_index(local_mainfd_stdin.readers, i - 1);

		if (remote_sock->writable && write_all(remote_sock->fd, buf, len) < 0) {
			nwarn("Failed to write to remote console socket");
			remote_sock_shutdown(remote_sock, SHUT_WR);
		}
	}
}

/* Internal */
static gboolean attach_cb(int fd, G_GNUC_UNUSED GIOCondition condition, gpointer user_data)
{
	struct remote_sock_s *srcsock = (struct remote_sock_s *)user_data;
	int new_fd = accept(fd, NULL, NULL);
	if (new_fd == -1) {
		if (errno != EWOULDBLOCK)
			nwarn("Failed to accept client connection on attach socket");
	} else {
		struct remote_sock_s *remote_sock;
		if (srcsock->dest->readers == NULL) {
			srcsock->dest->readers = g_ptr_array_new_with_free_func(free);
		}
		remote_sock = malloc(sizeof(*remote_sock));
		if (remote_sock == NULL) {
			pexit("Failed to allocate memory");
		}
		init_remote_sock(remote_sock, srcsock);
		remote_sock->fd = new_fd;
		g_unix_fd_add(remote_sock->fd, G_IO_IN | G_IO_HUP | G_IO_ERR, remote_sock_cb, remote_sock);
		g_ptr_array_add(remote_sock->dest->readers, remote_sock);
		ndebugf("Accepted%s connection %d", SOCK_IS_CONSOLE(srcsock->sock_type) ? " console" : "", remote_sock->fd);
	}

	return G_SOURCE_CONTINUE;
}

static gboolean remote_sock_cb(G_GNUC_UNUSED int fd, GIOCondition condition, gpointer user_data)
{
	struct remote_sock_s *sock = (struct remote_sock_s *)user_data;

	if (condition & G_IO_IN)
		return read_remote_sock(sock);

	return terminate_remote_sock(sock);
}

static gboolean read_remote_sock(struct remote_sock_s *sock)
{
	ssize_t num_read;

	/* There is still data in the buffer.  */
	if (sock->remaining) {
		sock->data_ready = true;
		return G_SOURCE_REMOVE;
	}

	if (SOCK_IS_STREAM(sock->sock_type)) {
		num_read = read(sock->fd, sock->buf, CONN_SOCK_BUF_SIZE);
	} else {
		num_read = recvfrom(sock->fd, sock->buf, CONN_SOCK_BUF_SIZE - 1, 0, NULL, NULL);
	}

	if (num_read < 0)
		return G_SOURCE_CONTINUE;

	if (num_read == 0)
		return terminate_remote_sock(sock);

	/* num_read > 0 */
	sock->remaining = num_read;
	sock->off = 0;

	if (SOCK_IS_NOTIFY(sock->sock_type)) {
		/* Do what OCI runtime does - only pass READY=1 */
		sock->buf[num_read] = '\0';
		if (strstr(sock->buf, "READY=1")) {
			strncpy(sock->buf, "READY=1", 8);
			sock->remaining = 7;
		} else if (strstr(sock->buf, "WATCHDOG=1")) {
			strncpy(sock->buf, "WATCHDOG=1", 11);
			sock->remaining = 10;
		} else {
			sock->remaining = 0;
		}
	}

	if (sock->remaining)
		sock_try_write_to_local_sock(sock);

	/* Not everything was written, let's wait for the fd to be ready.  */
	if (sock->remaining)
		schedule_local_sock_write(sock->dest);
	return G_SOURCE_CONTINUE;
}

static gboolean terminate_remote_sock(struct remote_sock_s *sock)
{
	remote_sock_shutdown(sock, SHUT_RD);
	if (SOCK_IS_CONSOLE(sock->sock_type)) {
		// If we're terminating our STDIN holder, we need to close the FD too, based on the cmdline option
		if (*(sock->dest->fd) >= 0 && opt_stdin) {
			if (!opt_leave_stdin_open) {
				close(*(sock->dest->fd));
				*(sock->dest->fd) = -1;
			} else {
				ninfo("Not closing input");
			}
		}
	}
	return G_SOURCE_REMOVE;
}

static void remote_sock_shutdown(struct remote_sock_s *sock, int how)
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
		ndebugf("Closing %d", sock->fd);
		close(sock->fd);
		sock->fd = -1;
		if (sock->dest->readers != NULL) {
			g_ptr_array_remove(sock->dest->readers, sock);
		}
	}
}

static void write_to_local_sock(gpointer data, gpointer user_data)
{
	struct remote_sock_s *sock = (struct remote_sock_s *)data;
	bool *has_data = user_data;

	sock_try_write_to_local_sock(sock);

	if (sock->remaining)
		*has_data = true;
	else if (sock->data_ready) {
		sock->data_ready = false;
		g_unix_fd_add(sock->fd, G_IO_IN | G_IO_HUP | G_IO_ERR, remote_sock_cb, sock);
	}
}

static void sock_try_write_to_local_sock(struct remote_sock_s *sock)
{
	struct local_sock_s *local_sock = sock->dest;
	ssize_t w = 0;

	if (!sock->remaining || *(local_sock->fd) < 0)
		return;

	if (local_sock->is_stream) {
		w = write(*(local_sock->fd), sock->buf + sock->off, sock->remaining);
	} else {
		w = sendto(*(local_sock->fd), sock->buf + sock->off, sock->remaining, MSG_DONTWAIT | MSG_NOSIGNAL,
			   (struct sockaddr *)local_sock->addr, sizeof(*(local_sock->addr)));
	}
	if (w < 0) {
		nwarnf("Failed to write %s", local_sock->label);
	} else {
		sock->off += w;
		sock->remaining -= w;
	}
}

static gboolean local_sock_write_cb(G_GNUC_UNUSED int fd, G_GNUC_UNUSED GIOCondition condition, gpointer user_data)
{
	struct local_sock_s *local_sock = (struct local_sock_s *)user_data;
	bool has_data = FALSE;

	if (*(local_sock->fd) < 0)
		return G_SOURCE_REMOVE;

	g_ptr_array_foreach(local_sock->readers, write_to_local_sock, &has_data);
	if (has_data)
		return G_SOURCE_CONTINUE;
	return G_SOURCE_REMOVE;
}

static void schedule_local_sock_write(struct local_sock_s *local_sock)
{
	if (*(local_sock->fd) < 0)
		return;

	g_unix_fd_add(*(local_sock->fd), G_IO_OUT, local_sock_write_cb, local_sock);
}

static void init_remote_sock(struct remote_sock_s *sock, struct remote_sock_s *src)
{
	sock->off = 0;
	sock->remaining = 0;
	sock->data_ready = false;
	sock->listening = false;
	if (src) {
		sock->readable = src->readable;
		sock->writable = src->writable;
		sock->dest = src->dest;
		g_unix_set_fd_nonblocking(*sock->dest->fd, TRUE, NULL);
		sock->sock_type = src->sock_type;
	}
}

static void close_sock(gpointer data, G_GNUC_UNUSED gpointer user_data)
{
	if (data == NULL)
		return;
	struct remote_sock_s *sock = (struct remote_sock_s *)data;

	close(sock->fd);
	sock->fd = -1;
}

void close_all_readers()
{
	if (local_mainfd_stdin.readers == NULL)
		return;
	g_ptr_array_foreach(local_mainfd_stdin.readers, close_sock, NULL);

	if (remote_attach_sock.fd >= 0)
		close(remote_attach_sock.fd);
	remote_attach_sock.fd = -1;
}
