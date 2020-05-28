#if !defined(CONN_SOCK_H)
#define CONN_SOCK_H

#include <glib.h>   /* gboolean */
#include "config.h" /* CONN_SOCK_BUF_SIZE */

#define SOCK_TYPE_CONSOLE 1
#define SOCK_TYPE_NOTIFY 2
#define SOCK_IS_CONSOLE(sock_type) ((sock_type) == SOCK_TYPE_CONSOLE)
#define SOCK_IS_NOTIFY(sock_type) ((sock_type) == SOCK_TYPE_NOTIFY)
#define SOCK_IS_STREAM(sock_type) ((sock_type) == SOCK_TYPE_CONSOLE)
#define SOCK_IS_DGRAM(sock_type) ((sock_type) != SOCK_TYPE_CONSOLE)

/* Used for attach */
/* The nomenclature here is decided but may not be entirely intuitive.
   in_sock and out_sock doesn't seem right, because ctr_stdio
   breaks encapsulation and writes directly to the console
   sockets.
   In most cases in conn_sock.c, this struct is "INPUT" and
   the next one is "OUTPUT".  Really it's this struct is "one"
   and the next is "many", but I don't want the same fd in
   two different sockets.

   "remote" indicates "Remote User" i.e. attached console, or
       "container's /dev/log" or "container's /run/notify"
   "local"  incidates "A socket we own, locally" i.e. mainfd_stdin
      or "host /dev/log" or "host /run/systemd/notify"
  */
struct remote_sock_s {
	int sock_type;
	int fd;
	struct local_sock_s *dest;
	gboolean listening;
	gboolean data_ready;
	gboolean readable;
	gboolean writable;
	size_t remaining;
	size_t off;
	char buf[CONN_SOCK_BUF_SIZE];
};

struct local_sock_s {
	int *fd;
	gboolean is_stream;
	GPtrArray *readers;
	char *label;
	struct sockaddr_un *addr;
};

char *setup_console_socket(void);
char *setup_seccomp_socket(const char *socket);
char *setup_attach_socket(void);
void setup_notify_socket(char *);
void schedule_main_stdin_write();
void write_back_to_remote_consoles(char *buf, int len);
void close_all_readers();

#endif // CONN_SOCK_H
