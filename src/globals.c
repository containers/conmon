#include "globals.h"

int runtime_status = -1;
int container_status = -1;

int mainfd_stdin = -1;
int mainfd_stdout = -1;
int mainfd_stderr = -1;

GPtrArray *conn_socks = NULL;

int attach_socket_fd = -1;
int console_socket_fd = -1;
int terminal_ctrl_fd = -1;
int inotify_fd = -1;
int winsz_fd_w = -1;
int winsz_fd_r = -1;

gboolean timed_out = FALSE;

GMainLoop *main_loop = NULL;
