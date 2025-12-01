#pragma once
#if !defined(CTR_LOGGING_BUFFER_H)
#define CTR_LOGGING_BUFFER_H

#include "utils.h"
#include "config.h"
#include <stdbool.h>
#include <limits.h>
#include <stdatomic.h>

/* Signal-safe async I/O buffering for log writes to prevent fsync() blocking */

#define BUFFER_SIZE (STDIO_BUF_SIZE * 4)	      /* 32KB buffer */
#define MAX_LOG_ENTRIES 512			      /* Max entries per buffer */
#define FLUSH_INTERVAL_MS 50			      /* Flush every 50ms for better CRI-O compatibility */
#define FLUSH_THRESHOLD_ENTRIES (MAX_LOG_ENTRIES / 2) /* Flush at 256 entries (50% full) */
#define FLUSH_THRESHOLD_BYTES (BUFFER_SIZE / 2)	      /* Flush at 16KB (50% full) */
#define ENTRY_TRUNCATION_SIZE (STDIO_BUF_SIZE - 1)    /* Max size before truncation: 8191 bytes */

/* Structure for a single log entry */
typedef struct {
	stdpipe_t pipe;
	char data[STDIO_BUF_SIZE];
	ssize_t size;
} log_entry_t;

/* Simple async log buffer - protected by signal masking */
typedef struct {
	log_entry_t entries[MAX_LOG_ENTRIES];
	log_entry_t flush_buffer[MAX_LOG_ENTRIES]; /* Pre-allocated buffer for async-signal-safe flushing */
	size_t count;
	size_t total_size;
	atomic_bool initialized;
	atomic_bool shutdown_requested;

	/* Statistics */
	size_t entries_written;
	size_t entries_dropped;
} async_log_buffer_t;

/* Global buffer instance (declared in .c file) */

/* Public API */
bool init_async_logging(void);
void shutdown_async_logging(void);
bool write_to_logs_buffered(stdpipe_t pipe, char *buf, ssize_t num_read);
void flush_log_buffer(void);

/* GLib event loop integration */
#include <glib.h>
gboolean log_timer_cb(gpointer user_data);
void setup_log_timer_in_main_loop(void);

#endif /* !defined(CTR_LOGGING_BUFFER_H) */