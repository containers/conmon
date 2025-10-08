#pragma once
#if !defined(CTR_LOGGING_H)
#define CTR_LOGGING_H

#include "utils.h"   /* stdpipe_t */
#include <stdbool.h> /* bool */

void reopen_log_files(void);
bool write_to_logs(stdpipe_t pipe, char *buf, size_t buflen);
void configure_log_drivers(gchar **log_drivers, int64_t log_size_max_, int64_t log_global_size_max_, char *cuuid_, char *name_, char *tag,
			   gchar **labels);
void sync_logs(void);
gboolean logging_is_passthrough(void);
gboolean logging_is_journald_enabled(void);
void close_logging_fds(void);

#endif /* !defined(CTR_LOGGING_H) */
