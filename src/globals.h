#if !defined(GLOBALS_H)
#define GLOBALS_H

#include <glib.h> /* gboolean and GMainLoop */

/* Global state */
// TODO FIXME not static
extern int runtime_status;
extern int container_status;

extern int mainfd_stdin;
extern int mainfd_stdout;
extern int mainfd_stderr;

extern int attach_socket_fd;
extern int console_socket_fd;
extern int seccomp_socket_fd;
extern int terminal_ctrl_fd;
extern int inotify_fd;
extern int winsz_fd_w;
extern int winsz_fd_r;
extern int attach_pipe_fd;
extern int dev_null_r;
extern int dev_null_w;

extern gboolean timed_out;

extern GMainLoop *main_loop;


#endif // GLOBALS_H
