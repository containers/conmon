#if !defined(CTR_STDIO_H)
#define CTR_STDIO_H

#include <glib.h>   /* GIOCondition and gpointer */
#include <stdint.h> /* int64_t */

gboolean stdio_cb(int fd, GIOCondition condition, gpointer user_data);
void drain_stdio();

#endif // CTR_STDIO_H
