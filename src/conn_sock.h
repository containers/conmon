#if !defined(CONN_SOCK_H)
#define CONN_SOCK_H

#include <glib.h>   /* gboolean */
#include "config.h" /* CONN_SOCK_BUF_SIZE */

/* Used for attach */
struct conn_sock_s {
	int fd;
	gboolean data_ready;
	gboolean readable;
	gboolean writable;
	size_t remaining;
	size_t off;
	char buf[CONN_SOCK_BUF_SIZE];
};

char *setup_console_socket(void);
char *setup_attach_socket(void);
void conn_sock_shutdown(struct conn_sock_s *sock, int how);
void schedule_master_stdin_write();

#endif // CONN_SOCK_H
