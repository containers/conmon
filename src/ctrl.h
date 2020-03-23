#if !defined(CTRL_H)
#define CTRL_H

#include <glib.h> /* gpointer */

gboolean terminal_accept_cb(int fd, G_GNUC_UNUSED GIOCondition condition, G_GNUC_UNUSED gpointer user_data);
gboolean ctrl_winsz_cb(int fd, G_GNUC_UNUSED GIOCondition condition, G_GNUC_UNUSED gpointer user_data);
gboolean ctrl_cb(int fd, G_GNUC_UNUSED GIOCondition condition, G_GNUC_UNUSED gpointer user_data);
void setup_console_fifo();
int setup_terminal_control_fifo();

#endif // CTRL_H
