#if !defined(CLOSE_FDS_H)
#define CLOSE_FDS_H

void close_other_fds();
void add_save_g_unix_fd(guint fd, GIOCondition condition, GUnixFDSourceFunc function, gpointer user_data);
void close_remove_g_unix_fd(guint fd);
void remove_g_unix_fds();

#endif // CLOSE_FD_H
