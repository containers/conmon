#if !defined(GLOBALS_H)
#define GLOBALS_H

#include <glib.h> /* gboolean and GMainLoop */

/* Global state */
// TODO FIXME not static
extern int runtime_status;
extern int container_status;

extern int masterfd_stdin;
extern int masterfd_stdout;
extern int masterfd_stderr;

extern GPtrArray *conn_socks;

extern int attach_socket_fd;
extern int console_socket_fd;
extern int terminal_ctrl_fd;
extern int inotify_fd;
extern int winsz_fd_w;
extern int winsz_fd_r;

extern gboolean timed_out;

extern GMainLoop *main_loop;


#endif // GLOBALS_H
