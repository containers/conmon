#if !defined(SELF_PIPE_H)
#define SELF_PIPE_H

/*
 * Self-pipe helper module for safely waking up the GLib main loop
 * from signal handlers.
 *
 * This avoids calling raise() from a signal handler while the main thread
 * is in ppoll(), which can trigger glibc's __syscall_cancel mechanism
 * and cause SIGABRT (issue #657).
 *
 * Usage:
 *   1. Call self_pipe_init(fd, callback, user_data) during startup to create
 *      the pipe and register a GLib IO source on the read end.
 *   2. Call self_pipe_wake() from any signal handler to wake the main loop.
 *   3. The callback receives (fd, condition, user_data) and must drain all
 *      bytes from the pipe before returning.
 */

#include <glib.h>

/* Initialize the self-pipe: create pipe2 with O_CLOEXEC|O_NONBLOCK,
 * register GLib IO source on read end. Returns 0 on success, -1 on failure. */
int self_pipe_init(gboolean (*callback)(gint fd, GIOCondition condition, gpointer user_data), gpointer user_data);

/* Write a single byte to the self-pipe to wake up the main loop.
 * Safe to call from a signal handler (async-signal-safe).
 * errno is preserved around the write(). */
void self_pipe_wake(void);

/* Clean up the self-pipe: remove the GLib source and close both ends.
 * Call this before exiting to avoid resource leaks. */
void self_pipe_fini(void);

#endif // SELF_PIPE_H
