#pragma once
#if !defined(CTR_LOGGING_H)
#define CTR_LOGGING_H

#include "utils.h"   /* stdpipe_t */
#include <stdbool.h> /* bool */

typedef struct {
	ssize_t iovcnt_max;
	ssize_t iovcnt;
	struct iovec *iov;
	char *buf;
	ssize_t buf_len;
} writev_buffer_t;

extern writev_buffer_t writev_buffer;

void writev_buffer_init(int pipe_size);
void reopen_log_files(void);
bool write_to_logs(stdpipe_t pipe, ssize_t num_read);
void configure_log_drivers(gchar **log_drivers, int64_t log_size_max_, char *cuuid_, char *name_, char *tag);
void sync_logs(void);

#endif /* !defined(CTR_LOGGING_H) */
