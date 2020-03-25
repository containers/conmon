#include "parent_pipe_fd.h"
#include "utils.h"
#include "cli.h"

#include <glib.h>

int sync_pipe_fd = -1;

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

void write_sync_fd(int fd, int res, const char *message)
{
	const char *res_key;
	if (opt_api_version >= 1)
		res_key = "data";
	else if (opt_exec)
		res_key = "exit_code";
	else
		res_key = "pid";

	ssize_t len;

	if (fd == -1)
		return;

	_cleanup_free_ char *json = NULL;
	if (message) {
		_cleanup_free_ char *escaped_message = escape_json_string(message);
		json = g_strdup_printf("{\"%s\": %d, \"message\": \"%s\"}\n", res_key, res, escaped_message);
	} else {
		json = g_strdup_printf("{\"%s\": %d}\n", res_key, res);
	}

	len = strlen(json);
	if (write_all(fd, json, len) != len) {
		pexit("Unable to send container stderr message to parent");
	}
}

static char *escape_json_string(const char *str)
{
	const char *p = str;
	GString *escaped = g_string_sized_new(strlen(str));

	while (*p != 0) {
		char c = *p++;
		if (c == '\\' || c == '"') {
			g_string_append_c(escaped, '\\');
			g_string_append_c(escaped, c);
		} else if (c == '\n') {
			g_string_append_printf(escaped, "\\n");
		} else if (c == '\t') {
			g_string_append_printf(escaped, "\\t");
		} else if ((c > 0 && c < 0x1f) || c == 0x7f) {
			g_string_append_printf(escaped, "\\u00%02x", (guint)c);
		} else {
			g_string_append_c(escaped, c);
		}
	}

	return g_string_free(escaped, FALSE);
}
