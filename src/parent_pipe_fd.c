#include "parent_pipe_fd.h"
#include "utils.h"
#include "cli.h"

#include <glib.h>
#include <stdlib.h>

int sync_pipe_fd = -1;

/*
 * Tracks whether we have already written to the sync pipe.
 * Used by the atexit handler to avoid sending a fallback message
 * when the normal code path already reported success or error.
 */
static gboolean sync_pipe_written = FALSE;

static char *escape_json_string(const char *str);

int get_pipe_fd_from_env(const char *envname)
{
	char *endptr = NULL;

	char *pipe_str = getenv(envname);
	if (pipe_str == NULL)
		return -1;

	errno = 0;
	int pipe_fd = strtol(pipe_str, &endptr, 10);
	if (errno != 0 || *endptr != '\0')
		pexitf("unable to parse %s", envname);
	if (fcntl(pipe_fd, F_SETFD, FD_CLOEXEC) == -1)
		pexitf("unable to make %s CLOEXEC", envname);

	return pipe_fd;
}

// Write a message to the sync pipe, or close the file descriptor if it's a broken pipe.
void write_or_close_sync_fd(int *fd, int res, const char *message)
{
	const char *res_key;
	if (opt_api_version >= 1)
		res_key = "data";
	else if (opt_exec)
		res_key = "exit_code";
	else
		res_key = "pid";

	ssize_t len;

	if (*fd == -1)
		return;

	_cleanup_free_ char *json = NULL;
	if (message && strlen(message) > 0) {
		_cleanup_free_ char *escaped_message = escape_json_string(message);
		if (escaped_message == NULL) {
			/* Fallback to JSON without message if escaping fails */
			json = g_strdup_printf("{\"%s\": %d}\n", res_key, res);
		} else {
			json = g_strdup_printf("{\"%s\": %d, \"message\": \"%s\"}\n", res_key, res, escaped_message);
		}
	} else {
		json = g_strdup_printf("{\"%s\": %d}\n", res_key, res);
	}

	/* Ensure we have valid JSON before attempting to write */
	if (json == NULL) {
		/* Fallback to minimal valid JSON */
		json = g_strdup_printf("{\"%s\": %d}\n", res_key, res);
	}

	len = strlen(json);
	if (write_all(*fd, json, len) != len) {
		if (errno == EPIPE) {
			close(*fd);
			*fd = -1;
			return;
		}
		pexit("Unable to send container stderr message to parent");
	}

	/* Track that we successfully wrote to the sync pipe so the
	 * atexit handler knows not to send a fallback error message. */
	if (fd == &sync_pipe_fd)
		sync_pipe_written = TRUE;
}

static char *escape_json_string(const char *str)
{
	if (str == NULL) {
		return NULL;
	}

	size_t str_len = strlen(str);
	if (str_len == 0) {
		return g_strdup("");
	}

	const char *p = str;
	GString *escaped = g_string_sized_new(str_len * 2); /* Pre-allocate extra space for escaping */

	if (escaped == NULL) {
		return NULL;
	}

	while (*p != 0) {
		unsigned char c = (unsigned char)*p++;

		/* Handle standard JSON escape sequences */
		if (c == '\\' || c == '"') {
			g_string_append_c(escaped, '\\');
			g_string_append_c(escaped, c);
		} else if (c == '/') {
			g_string_append_printf(escaped, "\\/");
		} else if (c == '\n') {
			g_string_append_printf(escaped, "\\n");
		} else if (c == '\r') {
			g_string_append_printf(escaped, "\\r");
		} else if (c == '\t') {
			g_string_append_printf(escaped, "\\t");
		} else if (c == '\b') {
			g_string_append_printf(escaped, "\\b");
		} else if (c == '\f') {
			g_string_append_printf(escaped, "\\f");
		} else if (c < 0x20 || c == 0x7f) {
			/* Escape control characters */
			g_string_append_printf(escaped, "\\u00%02x", c);
		} else if (c >= 0x80) {
			/* For non-ASCII characters, pass through as-is for UTF-8 compatibility */
			g_string_append_c(escaped, c);
		} else {
			/* Regular ASCII characters */
			g_string_append_c(escaped, c);
		}
	}

	return g_string_free(escaped, FALSE);
}

/*
 * Send a fallback error message on the sync pipe if no message has been
 * written yet. This ensures the caller (podman/cri-o) always gets a
 * JSON response instead of reading EOF on an empty pipe.
 *
 * This function is safe to call multiple times - it only writes once.
 * It is called explicitly from do_exit_command() before it closes fds,
 * and also registered as an atexit handler for exits that bypass
 * do_exit_command.
 *
 * IMPORTANT: This may run from an atexit handler, so it must never call
 * exit() (which write_or_close_sync_fd can do via pexit on write
 * failure). Use raw write() with best-effort semantics instead.
 */
void report_sync_pipe_if_needed(void)
{
	if (sync_pipe_written || sync_pipe_fd < 0)
		return;

	static const char fallback_json[] = "{\"pid\": -1, \"message\": \"conmon exited before reporting back to the caller\"}\n";

	/* Best-effort write - ignore errors. We must not call pexit/exit
	 * here because this may be running inside an atexit handler, and
	 * calling exit() from exit() is undefined behavior. */
	gssize ret __attribute__((unused)) = write(sync_pipe_fd, fallback_json, sizeof(fallback_json) - 1);
	sync_pipe_written = TRUE;
}

static void sync_pipe_atexit_handler(void)
{
	report_sync_pipe_if_needed();
}

void setup_sync_pipe_atexit(void)
{
	atexit(sync_pipe_atexit_handler);
}
