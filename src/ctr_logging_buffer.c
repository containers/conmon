#define _GNU_SOURCE
#include "ctr_logging_buffer.h"
#include "ctr_logging.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/timerfd.h>
#include <stdint.h>
#include <glib.h>
#include <signal.h>

/* Global buffer manager */
static async_log_buffer_t *g_log_buffer = NULL;
static sigset_t critical_signals;
static bool signal_mask_initialized = false;
static guint timer_source_id = 0;

/* Forward declarations */
static bool add_entry_to_buffer_locked(stdpipe_t pipe, char *buf, ssize_t size);
static bool should_flush_buffer_locked(void);
static void flush_buffer_contents_locked(void);
static int enter_critical_section(sigset_t *old_mask);
static void exit_critical_section(const sigset_t *old_mask);
static void setup_flush_timer(void);
static void cleanup_flush_timer(void);

/* Initialize the signal mask for critical sections */
static void init_signal_mask(void)
{
	if (signal_mask_initialized)
		return;

	sigemptyset(&critical_signals);
	/* Block all signals that could corrupt buffer state, but keep critical sections minimal */
	sigaddset(&critical_signals, SIGTERM);
	sigaddset(&critical_signals, SIGINT);
	sigaddset(&critical_signals, SIGCHLD);
	sigaddset(&critical_signals, SIGPIPE);
	sigaddset(&critical_signals, SIGALRM);
	sigaddset(&critical_signals, SIGUSR1);
	sigaddset(&critical_signals, SIGUSR2);
	signal_mask_initialized = true;
}

/* Enter critical section by blocking signals */
static int enter_critical_section(sigset_t *old_mask)
{
	init_signal_mask();
	if (sigprocmask(SIG_BLOCK, &critical_signals, old_mask) != 0) {
		nwarnf("Failed to block signals: %m");
		return -1;
	}
	return 0;
}

/* Exit critical section by restoring signal mask */
static void exit_critical_section(const sigset_t *old_mask)
{
	if (sigprocmask(SIG_SETMASK, old_mask, NULL) != 0) {
		nwarnf("Failed to restore signal mask: %m");
	}
}

bool init_async_logging(void)
{
	sigset_t old_mask;
	if (enter_critical_section(&old_mask) != 0) {
		return false;
	}

	/* Check if already initialized */
	if (g_log_buffer != NULL) {
		exit_critical_section(&old_mask);
		nwarn("Async logging already initialized");
		return true;
	}

	/* Allocate buffer */
	g_log_buffer = calloc(1, sizeof(async_log_buffer_t));
	if (!g_log_buffer) {
		exit_critical_section(&old_mask);
		nwarn("Failed to allocate log buffer");
		return false;
	}

	/* Initialize simple fields */
	g_log_buffer->count = 0;
	g_log_buffer->total_size = 0;
	g_log_buffer->entries_written = 0;
	g_log_buffer->entries_dropped = 0;
	g_log_buffer->initialized = false;
	g_log_buffer->shutdown_requested = false;
	g_log_buffer->timer_fd = -1;

	/* Initialize signal handling first */
	init_signal_mask();

	/* Mark as initialized */
	g_log_buffer->initialized = true;

	exit_critical_section(&old_mask);

	/* Setup timer outside critical section to avoid deadlock */
	setup_flush_timer();

	ninfo("Async logging initialized successfully");
	return true;
}

void shutdown_async_logging(void)
{
	sigset_t old_mask;
	if (enter_critical_section(&old_mask) != 0) {
		return;
	}

	if (!g_log_buffer) {
		exit_critical_section(&old_mask);
		return;
	}

	/* Mark shutdown to prevent new operations */
	g_log_buffer->shutdown_requested = true;

	/* Clean up timer first (outside critical section to avoid deadlock) */
	exit_critical_section(&old_mask);
	cleanup_flush_timer();

	/* Re-enter critical section for final operations */
	if (enter_critical_section(&old_mask) != 0) {
		return;
	}

	if (!g_log_buffer) {
		exit_critical_section(&old_mask);
		return;
	}

	/* Perform final flush */
	flush_buffer_contents_locked();

	/* Get final statistics before cleanup */
	size_t written = g_log_buffer->entries_written;
	size_t dropped = g_log_buffer->entries_dropped;

	/* Clean up resources */
	async_log_buffer_t *temp_buffer = g_log_buffer;
	g_log_buffer = NULL;

	exit_critical_section(&old_mask);

	ninfof("Async logging shutdown. Entries written: %zu, dropped: %zu", written, dropped);
	free(temp_buffer);
}

bool write_to_logs_buffered(stdpipe_t pipe, char *buf, ssize_t num_read)
{
	/* Validate input parameters first */
	if (!buf || num_read < 0) {
		return false;
	}

	sigset_t old_mask;
	if (enter_critical_section(&old_mask) != 0) {
		/* Failed to enter critical section, fallback to direct logging */
		return write_to_logs(pipe, buf, num_read);
	}

	/* Check buffer state atomically in critical section */
	if (!g_log_buffer || !g_log_buffer->initialized || g_log_buffer->shutdown_requested) {
		exit_critical_section(&old_mask);
		return write_to_logs(pipe, buf, num_read);
	}

	/* Try to add to buffer */
	bool success = add_entry_to_buffer_locked(pipe, buf, num_read);

	if (!success) {
		/* Buffer full - flush and try again */
		flush_buffer_contents_locked();
		success = add_entry_to_buffer_locked(pipe, buf, num_read);
	}

	if (!success) {
		/* Still can't fit - drop entry and fallback to direct write */
		g_log_buffer->entries_dropped++;
		exit_critical_section(&old_mask);
		nwarnf("Log buffer full, dropping entry and writing directly");
		return write_to_logs(pipe, buf, num_read);
	}

	/* Check if we should flush immediately */
	bool should_flush = should_flush_buffer_locked();
	if (should_flush) {
		flush_buffer_contents_locked();
	}

	exit_critical_section(&old_mask);
	return true;
}

void flush_log_buffer(void)
{
	size_t flush_count = 0;
	log_entry_t *temp_entries = NULL;

	/* Critical section: only copy buffer contents, don't do I/O */
	sigset_t old_mask;
	if (enter_critical_section(&old_mask) != 0) {
		return;
	}

	if (g_log_buffer && g_log_buffer->initialized && !g_log_buffer->shutdown_requested && g_log_buffer->count > 0) {
		flush_count = g_log_buffer->count;
		temp_entries = malloc(flush_count * sizeof(log_entry_t));
		if (temp_entries) {
			/* Copy entries and clear buffer quickly */
			memcpy(temp_entries, g_log_buffer->entries, flush_count * sizeof(log_entry_t));
			g_log_buffer->count = 0;
			g_log_buffer->total_size = 0;
		}
	}

	exit_critical_section(&old_mask);

	/* Now do I/O outside critical section */
	if (temp_entries && flush_count > 0) {
		for (size_t i = 0; i < flush_count; i++) {
			log_entry_t *entry = &temp_entries[i];
			if (entry->size > 0) {
				write_to_logs(entry->pipe, entry->data, entry->size);
				/* Update statistics in a brief critical section */
				if (enter_critical_section(&old_mask) == 0) {
					if (g_log_buffer) {
						g_log_buffer->entries_written++;
					}
					exit_critical_section(&old_mask);
				}
			}
		}
		free(temp_entries);
	}
}

/* Flush buffer contents when already in critical section */
static void flush_buffer_contents_locked(void)
{
	if (!g_log_buffer || g_log_buffer->count == 0) {
		return;
	}

	/* Copy buffer contents */
	size_t flush_count = g_log_buffer->count;
	log_entry_t *temp_entries = malloc(flush_count * sizeof(log_entry_t));
	if (!temp_entries) {
		nwarn("Failed to allocate temporary buffer for log flush");
		return;
	}

	memcpy(temp_entries, g_log_buffer->entries, flush_count * sizeof(log_entry_t));
	g_log_buffer->count = 0;
	g_log_buffer->total_size = 0;

	/* Exit critical section temporarily for I/O */
	sigset_t old_mask;
	if (sigprocmask(SIG_UNBLOCK, &critical_signals, &old_mask) == 0) {
		/* Do I/O outside critical section */
		for (size_t i = 0; i < flush_count; i++) {
			log_entry_t *entry = &temp_entries[i];
			if (entry->size > 0) {
				write_to_logs(entry->pipe, entry->data, entry->size);
			}
		}
		/* Re-enter critical section */
		sigprocmask(SIG_SETMASK, &old_mask, NULL);

		/* Update statistics now that we're back in critical section */
		if (g_log_buffer) {
			g_log_buffer->entries_written += flush_count;
		}
	}

	free(temp_entries);
}

int get_log_timer_fd(void)
{
	sigset_t old_mask;
	if (enter_critical_section(&old_mask) != 0) {
		return -1;
	}

	int timer_fd = -1;
	if (g_log_buffer && g_log_buffer->initialized && !g_log_buffer->shutdown_requested) {
		timer_fd = g_log_buffer->timer_fd;
	}

	exit_critical_section(&old_mask);
	return timer_fd;
}

static bool add_entry_to_buffer_locked(stdpipe_t pipe, char *buf, ssize_t size)
{
	/* Buffer state is guaranteed stable in critical section */
	if (g_log_buffer->count >= MAX_LOG_ENTRIES) {
		return false;
	}

	/* Validate size constraints more strictly */
	if (size <= 0) {
		return true; /* Empty writes are valid, just ignore */
	}

	ssize_t actual_size = size;
	if (size >= STDIO_BUF_SIZE) {
		nwarn("Log entry too large, truncating to fit buffer");
		actual_size = STDIO_BUF_SIZE - 1;
	}

	/* Check total buffer size with proper overflow protection */
	if (actual_size > 0 && g_log_buffer->total_size > SIZE_MAX - (size_t)actual_size) {
		return false; /* Would overflow */
	}
	if (g_log_buffer->total_size + (size_t)actual_size > BUFFER_SIZE) {
		return false; /* Would exceed buffer size */
	}

	/* Add entry to buffer */
	log_entry_t *entry = &g_log_buffer->entries[g_log_buffer->count];
	entry->pipe = pipe;
	entry->size = actual_size;

	/* Copy data safely with strict bounds checking */
	if (actual_size > 0) {
		memcpy(entry->data, buf, (size_t)actual_size);
		/* Always null-terminate for safety */
		if (actual_size < STDIO_BUF_SIZE) {
			entry->data[actual_size] = '\0';
		} else {
			entry->data[STDIO_BUF_SIZE - 1] = '\0';
		}
	}

	/* Update counters */
	g_log_buffer->total_size += (size_t)actual_size;
	g_log_buffer->count++;

	return true;
}

static bool should_flush_buffer_locked(void)
{
	if (!g_log_buffer) {
		return false;
	}

	/* Flush if buffer is getting full */
	return (g_log_buffer->count >= (MAX_LOG_ENTRIES / 2)) || (g_log_buffer->total_size >= (BUFFER_SIZE / 2));
}

gboolean log_timer_cb(gpointer user_data)
{
	(void)user_data; /* unused */

	/* Check if buffer is still valid and not shutting down */
	sigset_t old_mask;
	if (enter_critical_section(&old_mask) != 0) {
		return G_SOURCE_REMOVE; /* Can't acquire lock, stop timer */
	}

	bool should_continue = (g_log_buffer && g_log_buffer->initialized && !g_log_buffer->shutdown_requested);

	if (!should_continue) {
		exit_critical_section(&old_mask);
		return G_SOURCE_REMOVE; /* Stop the timer */
	}

	/* Flush the log buffer while in critical section */
	flush_buffer_contents_locked();

	exit_critical_section(&old_mask);
	return G_SOURCE_CONTINUE;
}

/* Timer management functions */
static void setup_flush_timer(void)
{
	/* Use GLib timeout instead of timerfd for simplicity */
	if (timer_source_id == 0) {
		timer_source_id = g_timeout_add(FLUSH_INTERVAL_MS, log_timer_cb, NULL);
		if (timer_source_id == 0) {
			nwarn("Failed to add log timer to main loop, async logging will not flush periodically");
		} else {
			ninfo("Log timer successfully integrated with main loop");
		}
	}
}

static void cleanup_flush_timer(void)
{
	if (timer_source_id != 0) {
		g_source_remove(timer_source_id);
		timer_source_id = 0;
	}
}

void setup_log_timer_in_main_loop(void)
{
	/* Timer is now setup automatically during initialization */
	ninfo("Log timer is handled automatically during async logging initialization");
}