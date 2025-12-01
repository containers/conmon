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
static _Atomic(async_log_buffer_t *) g_log_buffer = NULL;
static sigset_t critical_signals;
static bool signal_mask_initialized = false;
static _Atomic(guint) timer_source_id = 0;

/* Forward declarations */
static bool add_entry_to_buffer_locked(stdpipe_t pipe, char *buf, ssize_t size);
static bool should_flush_buffer_locked(void);
static size_t prepare_buffer_flush_locked(log_entry_t *dest_buffer);
static void perform_flush_io(log_entry_t *entries, size_t count);
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
	/* Safety check: signal mask must be initialized before use */
	if (!signal_mask_initialized) {
		nwarnf("Signal mask not initialized - this is a programming error");
		return -1;
	}

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
	/* Initialize signal mask once at startup */
	init_signal_mask();

	sigset_t old_mask;
	if (enter_critical_section(&old_mask) != 0) {
		return false;
	}

	/* Check if already initialized */
	async_log_buffer_t *current = atomic_load_explicit(&g_log_buffer, memory_order_acquire);
	if (current != NULL) {
		exit_critical_section(&old_mask);
		nwarn("Async logging already initialized");
		return true;
	}

	/* Allocate buffer */
	async_log_buffer_t *new_buffer = g_malloc0(sizeof(async_log_buffer_t));
	if (!new_buffer) {
		exit_critical_section(&old_mask);
		nwarn("Failed to allocate log buffer");
		return false;
	}

	/* Initialize simple fields */
	new_buffer->count = 0;
	new_buffer->total_size = 0;
	new_buffer->entries_written = 0;
	new_buffer->entries_dropped = 0;
	atomic_init(&new_buffer->initialized, false);
	atomic_init(&new_buffer->shutdown_requested, false);

	/* Mark as initialized */
	atomic_store_explicit(&new_buffer->initialized, true, memory_order_release);

	/* Publish the buffer with release semantics */
	atomic_store_explicit(&g_log_buffer, new_buffer, memory_order_release);

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

	async_log_buffer_t *buffer = atomic_load_explicit(&g_log_buffer, memory_order_acquire);
	if (!buffer) {
		exit_critical_section(&old_mask);
		return;
	}

	/* Mark shutdown to prevent new operations */
	atomic_store_explicit(&buffer->shutdown_requested, true, memory_order_release);

	/* Clean up timer first (outside critical section to avoid deadlock) */
	exit_critical_section(&old_mask);
	cleanup_flush_timer();

	/* Re-enter critical section for final operations */
	if (enter_critical_section(&old_mask) != 0) {
		return;
	}

	buffer = atomic_load_explicit(&g_log_buffer, memory_order_acquire);
	if (!buffer) {
		exit_critical_section(&old_mask);
		return;
	}

	/* Perform final flush */
	flush_buffer_contents_locked();

	/* Get final statistics before cleanup */
	size_t written = buffer->entries_written;
	size_t dropped = buffer->entries_dropped;

	/* Clear the global pointer with release semantics */
	atomic_store_explicit(&g_log_buffer, NULL, memory_order_release);

	exit_critical_section(&old_mask);

	ninfof("Async logging shutdown. Entries written: %zu, dropped: %zu", written, dropped);
	free(buffer);
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

	/* Handle empty writes explicitly - these are valid drain operations */
	if (num_read == 0) {
		exit_critical_section(&old_mask);
		return write_to_logs(pipe, buf, num_read);
	}

	/* Check buffer state atomically in critical section */
	async_log_buffer_t *buffer = atomic_load_explicit(&g_log_buffer, memory_order_acquire);
	bool initialized = buffer ? atomic_load_explicit(&buffer->initialized, memory_order_acquire) : false;
	bool shutdown = buffer ? atomic_load_explicit(&buffer->shutdown_requested, memory_order_acquire) : false;

	if (!buffer || !initialized || shutdown) {
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
		buffer->entries_dropped++;
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

	async_log_buffer_t *buffer = atomic_load_explicit(&g_log_buffer, memory_order_acquire);
	bool initialized = buffer ? atomic_load_explicit(&buffer->initialized, memory_order_acquire) : false;
	bool shutdown = buffer ? atomic_load_explicit(&buffer->shutdown_requested, memory_order_acquire) : false;

	if (buffer && initialized && !shutdown && buffer->count > 0) {
		temp_entries = buffer->flush_buffer; /* Use pre-allocated buffer */
		flush_count = prepare_buffer_flush_locked(temp_entries);
	}

	exit_critical_section(&old_mask);

	/* Now do I/O outside critical section using the helper */
	if (flush_count > 0) {
		perform_flush_io(temp_entries, flush_count);
	}
}

/* Prepare buffer for flush - must be called within critical section */
static size_t prepare_buffer_flush_locked(log_entry_t *dest_buffer)
{
	async_log_buffer_t *buffer = atomic_load_explicit(&g_log_buffer, memory_order_acquire);
	if (!buffer || buffer->count == 0) {
		return 0;
	}

	/* Copy buffer contents to destination */
	size_t flush_count = buffer->count;
	memcpy(dest_buffer, buffer->entries, flush_count * sizeof(log_entry_t));

	/* Clear buffer immediately */
	buffer->count = 0;
	buffer->total_size = 0;

	return flush_count;
}

/* Perform I/O for flushed entries - should be called OUTSIDE critical section */
static void perform_flush_io(log_entry_t *entries, size_t count)
{
	if (!entries || count == 0) {
		return;
	}

	size_t successful_writes = 0;
	for (size_t i = 0; i < count; i++) {
		log_entry_t *entry = &entries[i];
		if (entry->size > 0) {
			if (write_to_logs(entry->pipe, entry->data, entry->size)) {
				successful_writes++;
			}
		}
	}

	/* Update statistics in a single critical section */
	if (successful_writes > 0) {
		sigset_t old_mask;
		if (enter_critical_section(&old_mask) == 0) {
			async_log_buffer_t *buffer = atomic_load_explicit(&g_log_buffer, memory_order_acquire);
			if (buffer) {
				buffer->entries_written += successful_writes;
			}
			exit_critical_section(&old_mask);
		}
	}
}

/* Flush buffer contents when already in critical section
 * This function temporarily exits the critical section to perform I/O,
 * then re-enters to maintain the caller's expectation of holding the lock.
 */
static void flush_buffer_contents_locked(void)
{
	async_log_buffer_t *buffer = atomic_load_explicit(&g_log_buffer, memory_order_acquire);
	if (!buffer || buffer->count == 0) {
		return;
	}

	/* Prepare flush while in critical section */
	log_entry_t *temp_entries = buffer->flush_buffer;
	size_t flush_count = prepare_buffer_flush_locked(temp_entries);

	if (flush_count == 0) {
		return;
	}

	/* Temporarily exit critical section for I/O */
	sigset_t old_mask;
	if (sigprocmask(SIG_UNBLOCK, &critical_signals, &old_mask) == 0) {
		/* Perform I/O outside critical section - perform_flush_io handles its own
		 * critical section for statistics updates */
		size_t successful_writes = 0;
		for (size_t i = 0; i < flush_count; i++) {
			log_entry_t *entry = &temp_entries[i];
			if (entry->size > 0) {
				if (write_to_logs(entry->pipe, entry->data, entry->size)) {
					successful_writes++;
				}
			}
		}

		/* Re-enter critical section before returning to maintain caller's expectation */
		sigprocmask(SIG_SETMASK, &old_mask, NULL);

		/* Update statistics now that we're back in critical section */
		buffer = atomic_load_explicit(&g_log_buffer, memory_order_acquire);
		if (buffer) {
			buffer->entries_written += successful_writes;
		}
	}
}

static bool add_entry_to_buffer_locked(stdpipe_t pipe, char *buf, ssize_t size)
{
	async_log_buffer_t *buffer = atomic_load_explicit(&g_log_buffer, memory_order_acquire);
	if (!buffer) {
		return false;
	}

	/* Buffer state is guaranteed stable in critical section */
	if (buffer->count >= MAX_LOG_ENTRIES) {
		return false;
	}

	/* Validate size constraints - empty writes should not reach here */
	if (size < 0) {
		nwarnf("Invalid negative size %zd passed to add_entry_to_buffer_locked", size);
		return false;
	}

	if (size == 0) {
		nwarn("Empty write reached add_entry_to_buffer_locked - this should not happen");
		return false;
	}

	/* Check simple buffer size first for performance */
	if ((size_t)size > BUFFER_SIZE) {
		nwarn("Log entry too large, cannot fit in buffer");
		return false;
	}

	/* Check if adding this would exceed buffer capacity */
	if (buffer->total_size + (size_t)size > BUFFER_SIZE) {
		return false; /* Would exceed buffer size */
	}

	/* Check for overflow (size is already positive here) */
	if (buffer->total_size > SIZE_MAX - (size_t)size) {
		return false; /* Would overflow */
	}

	ssize_t actual_size = size;
	if (size >= STDIO_BUF_SIZE) {
		nwarn("Log entry too large, truncating to fit entry buffer");
		actual_size = ENTRY_TRUNCATION_SIZE;
	}

	/* Add entry to buffer */
	log_entry_t *entry = &buffer->entries[buffer->count];
	entry->pipe = pipe;
	entry->size = actual_size;

	/* Copy data safely with strict bounds checking */
	memcpy(entry->data, buf, (size_t)actual_size);

	/* Always null-terminate for safety - fix off-by-one */
	if (actual_size < STDIO_BUF_SIZE - 1) {
		entry->data[actual_size] = '\0';
	} else {
		entry->data[STDIO_BUF_SIZE - 1] = '\0';
	}

	/* Update counters */
	buffer->total_size += (size_t)actual_size;
	buffer->count++;

	return true;
}

static bool should_flush_buffer_locked(void)
{
	async_log_buffer_t *buffer = atomic_load_explicit(&g_log_buffer, memory_order_acquire);
	if (!buffer) {
		return false;
	}

	/* Flush if buffer is getting full (50% threshold) */
	return (buffer->count >= FLUSH_THRESHOLD_ENTRIES) || (buffer->total_size >= FLUSH_THRESHOLD_BYTES);
}

gboolean log_timer_cb(gpointer user_data)
{
	(void)user_data; /* unused */

	/* Check if buffer is still valid and not shutting down */
	sigset_t old_mask;
	if (enter_critical_section(&old_mask) != 0) {
		return G_SOURCE_REMOVE; /* Can't acquire lock, stop timer */
	}

	async_log_buffer_t *buffer = atomic_load_explicit(&g_log_buffer, memory_order_acquire);
	bool initialized = buffer ? atomic_load_explicit(&buffer->initialized, memory_order_acquire) : false;
	bool shutdown = buffer ? atomic_load_explicit(&buffer->shutdown_requested, memory_order_acquire) : false;

	bool should_continue = (buffer && initialized && !shutdown);

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
	guint current_id = atomic_load_explicit(&timer_source_id, memory_order_acquire);
	if (current_id == 0) {
		guint new_id = g_timeout_add(FLUSH_INTERVAL_MS, log_timer_cb, NULL);
		if (new_id == 0) {
			nwarn("Failed to add log timer to main loop, async logging will not flush periodically");
		} else {
			atomic_store_explicit(&timer_source_id, new_id, memory_order_release);
			ninfo("Log timer successfully integrated with main loop");
		}
	}
}

static void cleanup_flush_timer(void)
{
	guint current_id = atomic_load_explicit(&timer_source_id, memory_order_acquire);
	if (current_id != 0) {
		g_source_remove(current_id);
		atomic_store_explicit(&timer_source_id, 0, memory_order_release);
	}
}

void setup_log_timer_in_main_loop(void)
{
	/* Timer is now setup automatically during initialization */
	ninfo("Log timer is handled automatically during async logging initialization");
}