#define _GNU_SOURCE
#include "ctr_logging.h"
#include "cli.h"
#include "config.h"
#include <ctype.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

// if the systemd development files were found, we can log to systemd
#ifdef USE_JOURNALD
#include <systemd/sd-journal.h>
#else
/* this function should never be used, as journald logging is disabled and
 * parsing code errors if USE_JOURNALD isn't flagged.
 * This is just to make the compiler happy and the other code prettier
 */
static inline int sd_journal_sendv(G_GNUC_UNUSED const struct iovec *iov, G_GNUC_UNUSED int n)
{
	return -1;
}

#endif

/* strlen("1997-03-25T13:20:42.999999999+01:00 stdout ") + 1 */
#define TSBUFLEN 44

/* Different types of container logging */
static gboolean use_journald_logging = FALSE;
static gboolean use_k8s_logging = FALSE;
static gboolean use_k8s_stream_logging = FALSE;
static gboolean use_logging_passthrough = FALSE;

/* Value the user must input for each log driver */
static const char *const K8S_FILE_STRING = "k8s-file";
static const char *const K8S_STREAM_FILE_STRING = "k8s-stream-file";
static const char *const JOURNALD_FILE_STRING = "journald";

/* Max log size for any log file types */
static int64_t log_size_max = -1;

/* Max total log size for any log file types */
static int64_t log_global_size_max = -1;

/* k8s log file parameters */
static int k8s_log_fd = -1;
static char *k8s_log_path = NULL;
static int64_t k8s_bytes_written;
static int64_t k8s_total_bytes_written;

/* journald log file parameters */
// short ID length
#define TRUNC_ID_LEN 12
// MESSAGE=
#define MESSAGE_EQ_LEN 8
// PRIORITY=x
#define PRIORITY_EQ_LEN 10
// CONTAINER_ID_FULL=
#define CID_FULL_EQ_LEN 18
// CONTAINER_ID=
#define CID_EQ_LEN 13
// CONTAINER_NAME=
#define NAME_EQ_LEN 15
// CONTAINER_PARTIAL_MESSAGE=true
#define PARTIAL_MESSAGE_EQ_LEN 30
// SYSLOG_IDENTIFIER=
#define SYSLOG_IDENTIFIER_EQ_LEN 18
static char short_cuuid[TRUNC_ID_LEN + 1];
static char *cuuid = NULL;
static char *name = NULL;
static size_t cuuid_len = 0;
static size_t name_len = 0;
static char *container_id_full = NULL;
static char *container_id = NULL;
static char *container_name = NULL;
static char *container_tag = NULL;
static gchar **container_labels = NULL;
static size_t container_tag_len;
static char *syslog_identifier = NULL;
static size_t syslog_identifier_len;

#define WRITEV_BUFFER_N_IOV 128

typedef struct {
	int iovcnt;
	struct iovec iov[WRITEV_BUFFER_N_IOV];
} writev_buffer_t;

static void parse_log_path(char *log_config);
static const char *stdpipe_name(stdpipe_t pipe);
static int write_journald(int pipe, char *buf, ssize_t num_read);
static int write_k8s_log(stdpipe_t pipe, const char *buf, ssize_t buflen);
static int write_k8s_stream_log(stdpipe_t pipe, const char *buf, ssize_t buflen);
static bool get_line_len(ptrdiff_t *line_len, const char *buf, ssize_t buflen);
static ssize_t writev_buffer_append_segment(int fd, writev_buffer_t *buf, const void *data, ssize_t len);
static ssize_t writev_buffer_append_segment_no_flush(writev_buffer_t *buf, const void *data, ssize_t len);
static ssize_t writev_buffer_flush(int fd, writev_buffer_t *buf);
static void set_k8s_timestamp(char *buf, ssize_t buflen, const char *pipename);
static void reopen_k8s_file(void);
static int parse_priority_prefix(const char *buf, ssize_t buflen, int *priority, const char **message_start);


gboolean logging_is_passthrough(void)
{
	return use_logging_passthrough;
}

gboolean logging_is_journald_enabled(void)
{
	return use_journald_logging;
}

static int count_chars_in_string(const char *str, char ch)
{
	int count = 0;

	while (str) {
		str = strchr(str, ch);
		if (str == NULL)
			break;
		count++;
		str++;
	}

	return count;
}

static int is_valid_label_name(const char *str)
{
	while (*str) {
		if (*str == '=') {
			return 1;
		}
		if (!isupper(*str) && !isdigit(*str) && *str != '_') {
			return 0;
		}
		str++;
	}
	return 1;
}

/*
 * configures container log specific information, such as the drivers the user
 * called with and the max log size for log file types. For the log file types
 * (currently just k8s log file), it will also open the log_fd for that specific
 * log file.
 */
void configure_log_drivers(gchar **log_drivers, int64_t log_size_max_, int64_t log_global_size_max_, char *cuuid_, char *name_, char *tag,
			   gchar **log_labels)
{
	log_size_max = log_size_max_;
	log_global_size_max = log_global_size_max_;
	if (log_drivers == NULL)
		nexit("Log driver not provided. Use --log-path");
	for (int driver = 0; log_drivers[driver]; ++driver) {
		parse_log_path(log_drivers[driver]);
	}
	if (use_k8s_logging) {
		/* Open the log path file. */
		k8s_log_fd = open(k8s_log_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0640);
		if (k8s_log_fd < 0)
			pexit("Failed to open log file");

		struct stat statbuf;
		if (fstat(k8s_log_fd, &statbuf) == 0) {
			k8s_bytes_written = statbuf.st_size;
		} else {
			nwarnf("Could not stat log file %s, assuming 0 size", k8s_log_path);
			k8s_bytes_written = 0;
		}
		k8s_total_bytes_written = k8s_bytes_written;

		if (!use_journald_logging) {
			if (tag) {
				nexit("k8s-file doesn't support --log-tag");
			}
			if (log_labels) {
				nexit("k8s-file doesn't support --log-label");
			}
		}
	}

	if (use_journald_logging) {
#ifndef USE_JOURNALD
		nexit("Include journald in compilation path to log to systemd journal");
#endif
		/* save the length so we don't have to compute every sd_journal_* call */
		if (cuuid_ == NULL)
			nexit("Container ID must be provided and of the correct length");
		cuuid_len = strlen(cuuid_);
		if (cuuid_len <= TRUNC_ID_LEN)
			nexit("Container ID must be longer than 12 characters");

		cuuid = cuuid_;
		strncpy(short_cuuid, cuuid, TRUNC_ID_LEN);
		short_cuuid[TRUNC_ID_LEN] = '\0';
		name = name_;

		/* Setup some sd_journal_sendv arguments that won't change */
		container_id_full = g_strdup_printf("CONTAINER_ID_FULL=%s", cuuid);
		container_id = g_strdup_printf("CONTAINER_ID=%s", short_cuuid);

		/* Priority order of syslog_identifier (in order of precedence) is tag, name, `conmon`. */
		syslog_identifier = g_strdup_printf("SYSLOG_IDENTIFIER=%s", short_cuuid);
		syslog_identifier_len = TRUNC_ID_LEN + SYSLOG_IDENTIFIER_EQ_LEN;
		if (name) {
			name_len = strlen(name);
			container_name = g_strdup_printf("CONTAINER_NAME=%s", name);

			g_free(syslog_identifier);
			syslog_identifier = g_strdup_printf("SYSLOG_IDENTIFIER=%s", name);
			syslog_identifier_len = name_len + SYSLOG_IDENTIFIER_EQ_LEN;
		}
		if (tag) {
			container_tag = g_strdup_printf("CONTAINER_TAG=%s", tag);
			container_tag_len = strlen(container_tag);

			g_free(syslog_identifier);
			syslog_identifier = g_strdup_printf("SYSLOG_IDENTIFIER=%s", tag);
			syslog_identifier_len = strlen(syslog_identifier);
		}
		if (log_labels) {
			container_labels = log_labels;

			/* Ensure that valid LABEL=VALUE pairs have been passed */
			for (char **ptr = log_labels; *ptr; ptr++) {
				if (**ptr == '=') {
					nexitf("Container labels must be in format LABEL=VALUE (no LABEL present in '%s')", *ptr);
				}
				if (count_chars_in_string(*ptr, '=') != 1) {
					nexitf("Container labels must be in format LABEL=VALUE (none or more than one '=' present in '%s')",
					       *ptr);
				}
				if (!is_valid_label_name(*ptr)) {
					nexitf("Container label names must contain only uppercase letters, numbers and underscore (in '%s')",
					       *ptr);
				}
			}
		}
	}
}

/*
 * parse_log_path branches on log driver type the user inputted.
 * log_config will either be a ':' delimited string containing:
 * <DRIVER_NAME>:<PATH_NAME> or <PATH_NAME>
 * in the case the log driver is 'k8s-stream-file', the <PATH_NAME> must be present.
 * in the case of no colon, the driver will be k8s-file,
 * in the case the log driver is 'journald', the <PATH_NAME> is ignored.
 * exits with error if <DRIVER_NAME> isn't 'journald', 'k8s-file', or 'k8s-stream-file'.
 */
static void parse_log_path(char *log_config)
{
	const char *delim = strchr(log_config, ':');
	char *driver = strtok(log_config, ":");
	char *path = strtok(NULL, ":");

	if (path == NULL && driver == NULL) {
		nexitf("log-path must not be empty");
	}

	// :none is not the same as none, nor is :journald the same as journald
	// we check the delim here though, because we DO want to match "none" as the none driver
	if (path == NULL && delim == log_config) {
		path = driver;
		driver = (char *)K8S_FILE_STRING;
	}

	if (!strcmp(driver, "off") || !strcmp(driver, "null") || !strcmp(driver, "none")) {
		// no-op, this means things like --log-driver journald --log-driver none will still log to journald.
		return;
	}

	if (!strcmp(driver, "passthrough")) {
		use_logging_passthrough = TRUE;
		return;
	}

	if (!strcmp(driver, JOURNALD_FILE_STRING)) {
		use_journald_logging = TRUE;
		return;
	}

	// Driver is k8s-file, k8s-stream-file, or empty
	if (!strcmp(driver, K8S_STREAM_FILE_STRING)) {
		if (path == NULL) {
			nexitf("k8s-stream-file requires a filename");
		}
		use_k8s_logging = TRUE;
		use_k8s_stream_logging = TRUE;
		k8s_log_path = path;
		return;
	}

	// Driver is k8s-file or empty
	if (!strcmp(driver, K8S_FILE_STRING)) {
		if (path == NULL) {
			nexitf("k8s-file requires a filename");
		}
		use_k8s_logging = TRUE;
		k8s_log_path = path;
		return;
	}

	// If no : was found, use the entire log-path as a filename to k8s-file.
	if (path == NULL && delim == NULL) {
		use_k8s_logging = TRUE;
		k8s_log_path = driver;
		return;
	}

	nexitf("No such log driver %s", driver);
}

/* write container output to all logs the user defined */
bool write_to_logs(stdpipe_t pipe, char *buf, ssize_t num_read)
{
	if (use_k8s_logging) {
		if (use_k8s_stream_logging) {
			if (write_k8s_stream_log(pipe, buf, num_read) < 0) {
				nwarn("write_k8s_stream_log failed");
				return G_SOURCE_CONTINUE;
			}
		} else {
			if (write_k8s_log(pipe, buf, num_read) < 0) {
				nwarn("write_k8s_log failed");
				return G_SOURCE_CONTINUE;
			}
		}
	}
	if (use_journald_logging && write_journald(pipe, buf, num_read) < 0) {
		nwarn("write_journald failed");
		return G_SOURCE_CONTINUE;
	}
	return true;
}


/*
 * parse_priority_prefix checks if the buffer starts with a systemd priority prefix
 * in the format <N> where N is a digit 0-7. If found, it extracts the priority
 * and returns a pointer to the message content after the prefix.
 *
 * Returns:
 *  1 if priority prefix was found and parsed
 *  0 if no valid priority prefix was found
 * -1 on error (invalid parameters)
 */
static int parse_priority_prefix(const char *buf, ssize_t buflen, int *priority, const char **message_start)
{
	if (!buf || !priority || !message_start) {
		return -1;
	}

	/* Need at least 3 characters for <N> pattern */
	if (buflen < 3) {
		return 0;
	}

	/* Check for minimum pattern: <N> where N is 0-7 */
	if (buf[0] != '<') {
		return 0;
	}

	/* Check if second character is a valid priority digit (0-7) */
	if (buf[1] < '0' || buf[1] > '7') {
		return 0;
	}

	/* Check for closing bracket */
	if (buf[2] != '>') {
		return 0;
	}

	/* Extract the priority */
	*priority = buf[1] - '0';
	*message_start = buf + 3;

	return 1;
}

/* write to systemd journal. If the pipe is stdout, write with notice priority,
 * otherwise, write with error priority. Partial lines (that don't end in a newline) are buffered
 * between invocations. A 0 buflen argument forces a buffered partial line to be flushed.
 */
static int write_journald(int pipe, char *buf, ssize_t buflen)
{
	static char stdout_partial_buf[STDIO_BUF_SIZE];
	static size_t stdout_partial_buf_len = 0;
	static char stderr_partial_buf[STDIO_BUF_SIZE];
	static size_t stderr_partial_buf_len = 0;

	char *partial_buf;
	size_t *partial_buf_len;

	/* Default priority values: 6 (info) for stdout, 3 (err) for stderr
	 * These may be overridden by systemd priority prefixes in the message.
	 */
	int default_priority = (pipe == STDERR_PIPE) ? 3 : 6;
	char priority_str[PRIORITY_EQ_LEN + 2]; /* "PRIORITY=" + digit + null terminator */

	if (pipe == STDERR_PIPE) {
		partial_buf = stderr_partial_buf;
		partial_buf_len = &stderr_partial_buf_len;
	} else {
		partial_buf = stdout_partial_buf;
		partial_buf_len = &stdout_partial_buf_len;
	}

	ptrdiff_t line_len = 0;

	while (buflen > 0 || *partial_buf_len > 0) {
		writev_buffer_t bufv = {0};

		bool partial = buflen == 0 || get_line_len(&line_len, buf, buflen);

		/* If this is a partial line, and we have capacity to buffer it, buffer it and return.
		 * The capacity of the partial_buf is one less than its size so that we can always add
		 * a null terminating char later */
		if (buflen && partial && ((unsigned long)line_len < (STDIO_BUF_SIZE - *partial_buf_len))) {
			memcpy(partial_buf + *partial_buf_len, buf, line_len);
			*partial_buf_len += line_len;
			return 0;
		}

		/* Check for systemd priority prefix in the message */
		int parsed_priority = default_priority;
		const char *actual_message_start = NULL;
		ssize_t actual_message_len = line_len;

		/* Try to parse priority prefix from the complete message */
		if (*partial_buf_len == 0 && line_len > 0) {
			/* Only check for priority prefix at the start of a new line */
			int parse_result = parse_priority_prefix(buf, line_len, &parsed_priority, &actual_message_start);
			if (parse_result == 1) {
				/* Priority prefix found, adjust message content */
				actual_message_len = line_len - (actual_message_start - buf);
			} else {
				/* No priority prefix, use full message */
				actual_message_start = buf;
			}
		} else {
			/* Use full message when dealing with partial buffers */
			actual_message_start = buf;
		}

		ssize_t msg_len = actual_message_len + MESSAGE_EQ_LEN + *partial_buf_len;

		_cleanup_free_ char *message = g_malloc(msg_len);

		memcpy(message, "MESSAGE=", MESSAGE_EQ_LEN);
		memcpy(message + MESSAGE_EQ_LEN, partial_buf, *partial_buf_len);
		memcpy(message + MESSAGE_EQ_LEN + *partial_buf_len, actual_message_start, actual_message_len);

		/* Format the priority string */
		snprintf(priority_str, sizeof(priority_str), "PRIORITY=%d", parsed_priority);

		if (writev_buffer_append_segment_no_flush(&bufv, message, msg_len) < 0)
			return -1;

		if (writev_buffer_append_segment_no_flush(&bufv, container_id_full, cuuid_len + CID_FULL_EQ_LEN) < 0)
			return -1;

		if (writev_buffer_append_segment_no_flush(&bufv, priority_str, strlen(priority_str)) < 0)
			return -1;

		if (writev_buffer_append_segment_no_flush(&bufv, container_id, TRUNC_ID_LEN + CID_EQ_LEN) < 0)
			return -1;

		if (container_tag && writev_buffer_append_segment_no_flush(&bufv, container_tag, container_tag_len) < 0)
			return -1;

		/* only print the name if we have a name to print */
		if (name && writev_buffer_append_segment_no_flush(&bufv, container_name, name_len + NAME_EQ_LEN) < 0)
			return -1;

		if (writev_buffer_append_segment_no_flush(&bufv, syslog_identifier, syslog_identifier_len) < 0)
			return -1;

		/* per docker journald logging format, CONTAINER_PARTIAL_MESSAGE is set to true if it's partial, but otherwise not set. */
		if (partial && !opt_no_container_partial_message
		    && writev_buffer_append_segment_no_flush(&bufv, "CONTAINER_PARTIAL_MESSAGE=true", PARTIAL_MESSAGE_EQ_LEN) < 0)
			return -1;
		if (container_labels) {
			for (gchar **label = container_labels; *label; ++label) {
				if (writev_buffer_append_segment_no_flush(&bufv, *label, strlen(*label)) < 0)
					return -1;
			}
		}

		int err = sd_journal_sendv(bufv.iov, bufv.iovcnt);
		if (err < 0) {
			nwarnf("sd_journal_sendv: %s", strerror(-err));
			return err;
		}

		buf += line_len;
		buflen -= line_len;
		*partial_buf_len = 0;
	}
	return 0;
}

/*
 * The CRI requires us to write logs with a (timestamp, stream, line) format
 * for every newline-separated line. write_k8s_log writes said format for every
 * line in buf, and will partially write the final line of the log if buf is
 * not terminated by a newline.
 */
static int write_k8s_log(stdpipe_t pipe, const char *buf, ssize_t buflen)
{
	writev_buffer_t bufv = {0};
	int64_t bytes_to_be_written = 0;

	/*
	 * Use the same timestamp for every line of the log in this buffer.
	 * There is no practical difference in the output since write(2) is
	 * fast.
	 */
	char tsbuf[TSBUFLEN];
	set_k8s_timestamp(tsbuf, sizeof tsbuf, stdpipe_name(pipe));

	ptrdiff_t line_len = 0;
	while (buflen > 0) {
		bool partial = get_line_len(&line_len, buf, buflen);

		/* This is line_len bytes + TSBUFLEN - 1 + 2 (- 1 is for ignoring \0). */
		bytes_to_be_written = line_len + TSBUFLEN + 1;

		/* If partial, then we add a \n */
		if (partial) {
			bytes_to_be_written += 1;
		}

		/* If the caller specified a global max, enforce it before writing */
		if (log_global_size_max > 0 && k8s_total_bytes_written >= log_global_size_max)
			break;

		/*
		 * We re-open the log file if writing out the bytes will exceed the max
		 * log size. We also reset the state so that the new file is started with
		 * a timestamp.
		 */
		if ((log_size_max > 0) && (k8s_bytes_written + bytes_to_be_written) > log_size_max) {
			if (writev_buffer_flush(k8s_log_fd, &bufv) < 0) {
				nwarn("failed to flush buffer to log");
			}
			reopen_k8s_file();
		}

		/* Output the timestamp */
		if (writev_buffer_append_segment(k8s_log_fd, &bufv, tsbuf, TSBUFLEN - 1) < 0) {
			nwarn("failed to write (timestamp, stream) to log");
			goto next;
		}

		/* Output log tag for partial or newline */
		if (partial) {
			if (writev_buffer_append_segment(k8s_log_fd, &bufv, "P ", 2) < 0) {
				nwarn("failed to write partial log tag");
				goto next;
			}
		} else {
			if (writev_buffer_append_segment(k8s_log_fd, &bufv, "F ", 2) < 0) {
				nwarn("failed to write end log tag");
				goto next;
			}
		}

		/* Output the actual contents. */
		if (writev_buffer_append_segment(k8s_log_fd, &bufv, buf, line_len) < 0) {
			nwarn("failed to write buffer to log");
			goto next;
		}

		/* Output a newline for partial */
		if (partial) {
			if (writev_buffer_append_segment(k8s_log_fd, &bufv, "\n", 1) < 0) {
				nwarn("failed to write newline to log");
				goto next;
			}
		}

		k8s_bytes_written += bytes_to_be_written;
		k8s_total_bytes_written += bytes_to_be_written;
	next:
		/* Update the head of the buffer remaining to output. */
		buf += line_len;
		buflen -= line_len;
	}

	if (writev_buffer_flush(k8s_log_fd, &bufv) < 0) {
		nwarn("failed to flush buffer to log");
	}

	return 0;
}

/* Find the end of the line, or alternatively the end of the buffer.
 * Returns false in the former case (it's a whole line) or true in the latter (it's a partial)
 */
static bool get_line_len(ptrdiff_t *line_len, const char *buf, ssize_t buflen)
{
	bool partial = FALSE;
	const char *line_end = memchr(buf, '\n', buflen);
	if (line_end == NULL) {
		line_end = &buf[buflen - 1];
		partial = TRUE;
	}
	*line_len = line_end - buf + 1;
	return partial;
}


static ssize_t writev_buffer_flush(int fd, writev_buffer_t *buf)
{
	ssize_t count = 0;
	int iovcnt = buf->iovcnt;
	struct iovec *iov = buf->iov;

	/*
	 * By definition, flushing the buffers will either be entirely successful, or will fail at some point
	 * along the way.  There is no facility to attempt to retry a writev() system call outside of an EINTR
	 * errno.  Therefore, no matter the outcome, always reset the writev_buffer_t data structure.
	 */
	buf->iovcnt = 0;

	while (iovcnt > 0) {
		ssize_t res;
		do {
			res = writev(fd, iov, iovcnt);
		} while (res == -1 && errno == EINTR);

		if (res <= 0) {
			/*
			 * Any unflushed data is lost (this would be a good place to add a counter for how many times
			 * this occurs and another count for how much data is lost).
			 *
			 * Note that if writev() returns a 0, this logic considers it an error.
			 */
			return -1;
		}

		count += res;

		while (res > 0) {
			size_t iov_len = iov->iov_len;
			size_t from_this = MIN((size_t)res, iov_len);
			res -= from_this;
			iov_len -= from_this;

			if (iov_len == 0) {
				iov++;
				iovcnt--;
				/* continue, res still > 0 */
			} else {
				iov->iov_len = iov_len;
				iov->iov_base += from_this;
				/* break, res is 0 */
			}
		}
	}

	return count;
}


ssize_t writev_buffer_append_segment(int fd, writev_buffer_t *buf, const void *data, ssize_t len)
{
	if (data == NULL)
		return 1;

	if (buf->iovcnt == WRITEV_BUFFER_N_IOV && writev_buffer_flush(fd, buf) < 0)
		return -1;

	if (len > 0) {
		buf->iov[buf->iovcnt].iov_base = (void *)data;
		buf->iov[buf->iovcnt].iov_len = (size_t)len;
		buf->iovcnt++;
	}

	return 1;
}

ssize_t writev_buffer_append_segment_no_flush(writev_buffer_t *buf, const void *data, ssize_t len)
{
	if (data == NULL)
		return 1;

	if (buf->iovcnt == WRITEV_BUFFER_N_IOV)
		return -1;

	if (len > 0) {
		buf->iov[buf->iovcnt].iov_base = (void *)data;
		buf->iov[buf->iovcnt].iov_len = (size_t)len;
		buf->iovcnt++;
	}

	return 1;
}


static const char *stdpipe_name(stdpipe_t pipe)
{
	switch (pipe) {
	case STDIN_PIPE:
		return "stdin";
	case STDOUT_PIPE:
		return "stdout";
	case STDERR_PIPE:
		return "stderr";
	default:
		return "NONE";
	}
}

/* Generate timestamp string to buf. */
static void set_k8s_timestamp(char *buf, ssize_t buflen, const char *pipename)
{
	static int tzset_called = 0;

	/* Initialize timestamp variables with sensible defaults. */
	struct timespec ts = {0};
	struct tm current_tm = {0};
	char off_sign = '+';
	int off = 0;

	/* Attempt to get the current time. */
	if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
		if (errno != EINVAL) {
			ts.tv_nsec = 0; /* If other errors, fallback to nanoseconds = 0. */
		}
	}

	/* Ensure tzset is called only once. */
	if (!tzset_called) {
		tzset();
		tzset_called = 1;
	}

	/* Get the local time or fallback to defaults. */
	if (localtime_r(&ts.tv_sec, &current_tm) == NULL) {
		current_tm.tm_year = 70; /* 1970 (default epoch year) */
		current_tm.tm_mon = 0;	 /* January */
		current_tm.tm_mday = 1;	 /* 1st day of the month */
		current_tm.tm_hour = 0;	 /* midnight */
		current_tm.tm_min = 0;
		current_tm.tm_sec = 0;
		current_tm.tm_gmtoff = 0; /* UTC offset */
	}

	/* Calculate timezone offset. */
	off = (int)current_tm.tm_gmtoff;
	if (off < 0) {
		off_sign = '-';
		off = -off;
	}

	/* Format the timestamp into the buffer. */
	int len = snprintf(buf, buflen, "%d-%02d-%02dT%02d:%02d:%02d.%09ld%c%02d:%02d %s ", current_tm.tm_year + 1900,
			   current_tm.tm_mon + 1, current_tm.tm_mday, current_tm.tm_hour, current_tm.tm_min, current_tm.tm_sec, ts.tv_nsec,
			   off_sign, off / 3600, (off % 3600) / 60, pipename);

	/* Ensure null termination if snprintf output exceeds buffer length. */
	if (len >= buflen && buflen > 0) {
		buf[buflen - 1] = '\0';
	}
}

/* Force closing any open FD. */
void close_logging_fds(void)
{
	if (k8s_log_fd >= 0)
		close(k8s_log_fd);
	k8s_log_fd = -1;
}

/* reopen all log files */
void reopen_log_files(void)
{
	reopen_k8s_file();
}

/* Atomic symlink validation using file descriptors to prevent race conditions */
static gboolean path_contains_symlinks_atomic(const char *canonical_path)
{
	_cleanup_free_ char *path_copy = NULL;
	_cleanup_free_ char *component = NULL;
	char *current_pos;
	char *slash_pos;
	int current_fd = AT_FDCWD;
	int next_fd = -1;
	struct stat path_stat;
	gboolean result = FALSE;

	if (!canonical_path)
		return TRUE;

	path_copy = g_strdup(canonical_path);
	if (!path_copy)
		return TRUE; /* Treat allocation failure as unsafe */

	/* Start from root if absolute path */
	if (path_copy[0] == '/') {
		current_fd = open("/", O_PATH | O_CLOEXEC);
		if (current_fd < 0)
			return TRUE;
		current_pos = path_copy + 1;
	} else {
		current_pos = path_copy;
	}

	/* Check each directory component atomically */
	while (current_pos && *current_pos) {
		slash_pos = strchr(current_pos, '/');
		if (slash_pos)
			*slash_pos = '\0';

		/* Use fstatat to check component without following symlinks */
		if (fstatat(current_fd, current_pos, &path_stat, AT_SYMLINK_NOFOLLOW) == 0) {
			if (S_ISLNK(path_stat.st_mode)) {
				result = TRUE;
				goto cleanup;
			}

			/* If it's a directory, open it for next iteration */
			if (S_ISDIR(path_stat.st_mode) && slash_pos) {
				next_fd = openat(current_fd, current_pos, O_PATH | O_CLOEXEC);
				if (next_fd < 0) {
					result = TRUE; /* Treat access failure as unsafe */
					goto cleanup;
				}
				if (current_fd != AT_FDCWD)
					close(current_fd);
				current_fd = next_fd;
				next_fd = -1;
			}
		} else {
			/* Component doesn't exist or access denied */
			if (errno != ENOENT) {
				result = TRUE; /* Treat access failure as unsafe */
				goto cleanup;
			}
		}

		if (slash_pos) {
			*slash_pos = '/';
			current_pos = slash_pos + 1;
		} else {
			break;
		}
	}

cleanup:
	if (current_fd != AT_FDCWD)
		close(current_fd);
	if (next_fd >= 0)
		close(next_fd);
	return result;
}


/* check if path is within allowlisted directories */
static gboolean is_path_in_allowlist(const char *canonical_path)
{
	char **dir_iter;

	if (!opt_log_allowlist_dirs)
		return TRUE; /* No allowlist configured, allow all paths */

	/* Check if canonical path starts with any allowed directory */
	for (dir_iter = opt_log_allowlist_dirs; *dir_iter; dir_iter++) {
		_cleanup_free_ char *allowed_canonical = NULL;

		/* Skip empty entries */
		if (strlen(*dir_iter) == 0)
			continue;

		/* Get canonical path of allowed directory */
		allowed_canonical = realpath(*dir_iter, NULL);
		if (!allowed_canonical) {
			nwarnf("Invalid allowlist directory");
			continue;
		}

		/* Check if log path is within this allowed directory */
		if (g_str_has_prefix(canonical_path, allowed_canonical)) {
			/* Ensure it's exactly the directory or a subdirectory */
			size_t allowed_len = strlen(allowed_canonical);
			if (canonical_path[allowed_len] == '\0' || canonical_path[allowed_len] == '/') {
				return TRUE;
			}
		}
	}

	return FALSE;
}

/* Secure file descriptor validation to prevent TOCTOU attacks */
static gboolean validate_fd_path_security(int fd, const char *expected_path)
{
	_cleanup_free_ char *fd_path = NULL;
	_cleanup_free_ char *expected_canonical = NULL;
	struct stat fd_stat, path_stat;
	char proc_fd_path[64];
	ssize_t link_len;

	/* Get the actual path of the file descriptor */
	snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/self/fd/%d", fd);
	fd_path = g_malloc0(PATH_MAX);
	link_len = readlink(proc_fd_path, fd_path, PATH_MAX - 1);
	if (link_len < 0) {
		nwarnf("Failed to read fd path: %m");
		return FALSE;
	}
	fd_path[link_len] = '\0';

	/* Get canonical path of expected file */
	expected_canonical = realpath(expected_path, NULL);
	if (!expected_canonical) {
		nwarnf("Failed to canonicalize expected path %s: %m", expected_path);
		return FALSE;
	}

	/* Compare paths */
	if (strcmp(fd_path, expected_canonical) != 0) {
		nwarnf("File descriptor path mismatch: expected %s, got %s", expected_canonical, fd_path);
		return FALSE;
	}

	/* Validate file stats match */
	if (fstat(fd, &fd_stat) != 0) {
		nwarnf("Failed to stat file descriptor: %m");
		return FALSE;
	}

	if (stat(expected_canonical, &path_stat) != 0) {
		nwarnf("Failed to stat expected path: %m");
		return FALSE;
	}

	/* Compare device and inode to ensure they're the same file */
	if (fd_stat.st_dev != path_stat.st_dev || fd_stat.st_ino != path_stat.st_ino) {
		nwarnf("File descriptor and path point to different files");
		return FALSE;
	}

	return TRUE;
}

/* Secure path validation using file descriptors to prevent TOCTOU */
static int secure_validate_log_path(const char *path)
{
	_cleanup_free_ char *canonical_path = NULL;
	_cleanup_free_ char *parent_dir = NULL;
	_cleanup_free_ char *basename = NULL;
	int parent_fd = -1;
	int result = -1;
	struct stat path_stat;

	if (!path || strlen(path) == 0)
		return -1;

	/* Prevent excessively long paths */
	if (strlen(path) >= PATH_MAX)
		return -1;

	/* Check for directory traversal patterns */
	if (strstr(path, "../") || strstr(path, "/.."))
		return -1;

	/* Get parent directory for openat() operations */
	parent_dir = g_path_get_dirname(path);
	basename = g_path_get_basename(path);
	if (!parent_dir || !basename)
		return -1;

	/* Open parent directory with O_PATH for safe operations */
	parent_fd = open(parent_dir, O_PATH | O_CLOEXEC);
	if (parent_fd < 0)
		return -1;

	/* Use fstatat to check if file exists and get its properties */
	if (fstatat(parent_fd, basename, &path_stat, AT_SYMLINK_NOFOLLOW) == 0) {
		/* File exists - check if it's a symlink */
		if (S_ISLNK(path_stat.st_mode)) {
			nwarnf("Log path is a symbolic link");
			goto cleanup;
		}
		/* Return the parent fd for atomic operations */
		result = parent_fd;
		parent_fd = -1; /* Transfer ownership */
	} else if (errno == ENOENT) {
		/* File doesn't exist - validate parent directory security comprehensively */
		struct stat parent_stat;

		/* Check parent directory permissions and ownership */
		if (fstat(parent_fd, &parent_stat) != 0) {
			nwarnf("Failed to stat parent directory: %m");
			goto cleanup;
		}

		/* Validate parent directory permissions are not world-writable */
		if (parent_stat.st_mode & S_IWOTH) {
			nwarnf("Parent directory is world-writable, security risk: %s", parent_dir);
			goto cleanup;
		}

		/* Additional security check: ensure parent directory is not owned by suspicious UIDs */
		if (parent_stat.st_uid == 0 || parent_stat.st_uid == getuid() || parent_stat.st_uid == geteuid()) {
			/* Acceptable ownership */
		} else {
			nwarnf("Parent directory owned by unexpected UID %d: %s", parent_stat.st_uid, parent_dir);
			goto cleanup;
		}

		canonical_path = realpath(parent_dir, NULL);
		if (!canonical_path)
			goto cleanup;

		/* Check allowlist if configured */
		if (opt_log_allowlist_dirs && !is_path_in_allowlist(canonical_path)) {
			nwarnf("Parent directory not in allowlist");
			goto cleanup;
		}

		/* Check for symlinks in parent path using atomic validation */
		if (path_contains_symlinks_atomic(canonical_path)) {
			nwarnf("Parent path contains symlinks");
			goto cleanup;
		}


		result = parent_fd;
		parent_fd = -1; /* Transfer ownership */
	}

cleanup:
	if (parent_fd >= 0)
		close(parent_fd);
	return result;
}


/* shift backup log files for rotation */
static gboolean shift_backup_files(void)
{
	gboolean had_errors = FALSE;

	/* Enhanced bounds checking to prevent integer overflow and underflow */
	if (opt_log_max_files <= 0 || opt_log_max_files > INT_MAX) {
		nwarnf("Invalid log_max_files value: %d", opt_log_max_files);
		return FALSE;
	}

	/* Validate log path using secure validation */
	int validation_fd = secure_validate_log_path(k8s_log_path);
	if (validation_fd < 0) {
		nwarnf("Invalid log path for rotation");
		return FALSE;
	}
	close(validation_fd);

	/* Shift existing backups from highest to lowest: .N-1 -> .N, .N-2 -> .N-1, etc. */
	int loop_start = (opt_log_max_files > 1) ? opt_log_max_files : 2;

	for (int i = loop_start; i >= 2; i--) {
		_cleanup_free_ char *from = g_strdup_printf("%s.%d", k8s_log_path, i - 1);
		_cleanup_free_ char *to = g_strdup_printf("%s.%d", k8s_log_path, i);

		/* Verify string allocation succeeded */
		if (!from || !to) {
			nwarnf("Memory allocation failed during backup file shifting");
			return FALSE;
		}

		/* Direct atomic rename - overwrites destination if it exists */
		if (rename(from, to) != 0 && errno != ENOENT) {
			nwarnf("Failed to shift backup file %s to %s: %m", from, to);
			had_errors = TRUE;
		}
	}

	/* Report success but warn if there were non-critical errors */
	if (had_errors) {
		nwarnf("Backup file shifting completed with some errors");
	}

	return TRUE;
}


/* Helper function to perform the actual file rotation */
static gboolean perform_file_rotation(const char *temp_path, const char *backup_path)
{
	/* Rename current log to .1 */
	if (rename(k8s_log_path, backup_path) < 0) {
		nwarnf("Failed to rotate log file: %m");
		return FALSE;
	}

	/* Move new file into place atomically */
	if (rename(temp_path, k8s_log_path) < 0) {
		nwarnf("Failed to move new log file into place: %m");
		/* Try to restore the original file */
		if (rename(backup_path, k8s_log_path) < 0) {
			nwarnf("CRITICAL: Failed to restore original log file: %m");
			nwarnf("Original log data may be in backup file");
		}
		return FALSE;
	}

	return TRUE;
}

/* Helper function to clean up temporary files and file descriptors */
static void cleanup_temp_file(int fd, const char *temp_path)
{
	if (fd >= 0) {
		if (close(fd) < 0) {
			nwarnf("Failed to close temporary file descriptor %d: %m", fd);
		}
	}
	if (temp_path && strlen(temp_path) > 0) {
		if (unlink(temp_path) < 0 && errno != ENOENT) {
			nwarnf("Failed to remove temporary file: %m");
		}
	}
}

/* Validate rotation preconditions and acquire file lock */
static int validate_and_lock_rotation(int *parent_fd)
{
	int old_fd = k8s_log_fd;
	struct flock lock_info = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0};

	*parent_fd = secure_validate_log_path(k8s_log_path);
	if (*parent_fd < 0) {
		nwarnf("Cannot rotate: invalid log path");
		return -1;
	}

	if (old_fd < 0) {
		nwarnf("Cannot rotate: invalid file descriptor");
		close(*parent_fd);
		*parent_fd = -1;
		return -1;
	}

	if (fcntl(old_fd, F_SETLK, &lock_info) == -1) {
		nwarnf("Log file locked by another process, skipping rotation");
		close(*parent_fd);
		*parent_fd = -1;
		return -1;
	}

	if (!validate_fd_path_security(old_fd, k8s_log_path)) {
		nwarnf("File descriptor security validation failed");
		return -1;
	}

	return old_fd;
}

/* Setup rotation file paths and create new log file */
static int setup_rotation_files(int parent_fd, char **temp_path, char **backup_path)
{
	_cleanup_free_ char *basename = g_path_get_basename(k8s_log_path);
	_cleanup_free_ char *temp_basename = NULL;

	if (!basename || !(*temp_path = g_strdup_printf("%s.new", k8s_log_path)) || !(*backup_path = g_strdup_printf("%s.1", k8s_log_path))
	    || !(temp_basename = g_strdup_printf("%s.new", basename))) {
		nwarnf("Memory allocation failed for rotation paths");
		return -1;
	}

	int new_fd = openat(parent_fd, temp_basename, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
	if (new_fd < 0) {
		nwarnf("Failed to create new log file: %m");
	}
	return new_fd;
}

/* Simplified thread-safe rotation with file locking */
static void rotate_k8s_file(void)
{
	int parent_fd = -1;
	_cleanup_free_ char *temp_path = NULL;
	_cleanup_free_ char *backup_path = NULL;

	int old_fd = validate_and_lock_rotation(&parent_fd);
	if (old_fd < 0)
		return;

	int new_fd = setup_rotation_files(parent_fd, &temp_path, &backup_path);
	if (new_fd < 0)
		goto cleanup;

	if (!shift_backup_files() || !perform_file_rotation(temp_path, backup_path)) {
		cleanup_temp_file(new_fd, temp_path);
		goto cleanup;
	}

	/* Atomic state update */
	struct flock unlock = {.l_type = F_UNLCK};
	fcntl(old_fd, F_SETLK, &unlock);
	close(old_fd);

	k8s_log_fd = new_fd;
	k8s_bytes_written = 0;
	close(parent_fd);
	return;

cleanup:
	if (parent_fd >= 0)
		close(parent_fd);
	unlock.l_type = F_UNLCK;
	fcntl(old_fd, F_SETLK, &unlock);
}

/* reopen the k8s log file fd.  */
static void reopen_k8s_file(void)
{
	if (!use_k8s_logging)
		return;

	if (opt_log_rotate) {
		/* Use log rotation instead of truncation */
		rotate_k8s_file();
	} else {
		/* Original truncation behavior for backward compatibility */
		_cleanup_free_ char *k8s_log_path_tmp = g_strdup_printf("%s.tmp", k8s_log_path);

		/* Close the current k8s_log_fd */
		close(k8s_log_fd);

		/* Open with O_TRUNC: reset bytes written */
		k8s_bytes_written = 0;

		/* Open the log path file again */
		k8s_log_fd = open(k8s_log_path_tmp, O_WRONLY | O_TRUNC | O_CREAT | O_CLOEXEC, 0640);
		if (k8s_log_fd < 0)
			pexitf("Failed to open log file %s", k8s_log_path);

		/* Replace the previous file */
		if (rename(k8s_log_path_tmp, k8s_log_path) < 0) {
			pexit("Failed to rename log file");
		}
	}
}


void sync_logs(void)
{
	/* Sync the logs to disk */
	if (k8s_log_fd > 0)
		if (fsync(k8s_log_fd) < 0)
			nwarnf("Failed to sync log file before exit: %m");
}


/* strlen("1997-03-25T13:20:42.999999999+01:00 stdout 9999999999 999999999 ") + 1 */
#define TSSTREAMBUFLEN 128

/*
 * PROPOSED: CRI Stream Format, variable length file format
 */
static int set_k8s_stream_timestamp(char *buf, ssize_t bufsiz, ssize_t *tsbuflen, const char *pipename, uint64_t offset, ssize_t buflen,
				    ssize_t *btbw)
{
	char off_sign = '+';
	int off, len, err = -1;

	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
		/* If CLOCK_REALTIME is not supported, we set nano seconds to 0 */
		if (errno == EINVAL) {
			ts.tv_nsec = 0;
		} else {
			return err;
		}
	}

	struct tm current_tm;
	if (localtime_r(&ts.tv_sec, &current_tm) == NULL)
		return err;

	off = (int)current_tm.tm_gmtoff;
	if (current_tm.tm_gmtoff < 0) {
		off_sign = '-';
		off = -off;
	}

	len = snprintf(buf, bufsiz, "%d-%02d-%02dT%02d:%02d:%02d.%09ld%c%02d:%02d %s %lud %ld ", current_tm.tm_year + 1900,
		       current_tm.tm_mon + 1, current_tm.tm_mday, current_tm.tm_hour, current_tm.tm_min, current_tm.tm_sec, ts.tv_nsec,
		       off_sign, off / 3600, off % 3600, pipename, offset, buflen);

	if (len < bufsiz)
		err = 0;

	*tsbuflen = len;
	*btbw = len + buflen;
	return err;
}


/*
 * PROPOSED: CRI Stream Format, variable length file format
 *
 * %d-%02d-%02dT%02d:%02d:%02d.%09ld%c%02d:%02d %(stream)s %(offset)lud %(buflen)ld %(buf)s
 *
 * The CRI stream fromat requires us to write each buffer read with a
 * timestamp, stream, length (human readable ascii), and the buffer contents
 * read (with a space character separating the buffer length string from the
 * buffer.
 */
static int write_k8s_stream_log(stdpipe_t pipe, const char *buf, ssize_t buflen)
{
	writev_buffer_t bufv = {0};
	char tsbuf[TSSTREAMBUFLEN];
	static ssize_t bytes_written = 0;
	static uint64_t offset = 0;
	ssize_t bytes_to_be_written = 0;
	ssize_t tsbuflen = 0;

	/*
	 * Use the same timestamp for every line of the log in this buffer.
	 * There is no practical difference in the output since write(2) is
	 * fast.
	 */
	if (set_k8s_stream_timestamp(tsbuf, sizeof tsbuf, &tsbuflen, stdpipe_name(pipe), buflen, offset, &bytes_to_be_written))
		/* TODO: We should handle failures much more cleanly than this. */
		return -1;

	/*
	 * We re-open the log file if writing out the bytes will exceed the max
	 * log size. We also reset the state so that the new file is started with
	 * a timestamp.
	 */
	if ((opt_log_size_max > 0) && (bytes_written + bytes_to_be_written) > opt_log_size_max) {
		bytes_written = 0;

		reopen_k8s_file();
	}

	/* Output the timestamp, stream, and length */
	if (writev_buffer_append_segment(k8s_log_fd, &bufv, tsbuf, tsbuflen) < 0) {
		nwarn("failed to write (timestamp, stream) to log");
		goto stream_next;
	}

	/* Output the actual contents. */
	if (writev_buffer_append_segment(k8s_log_fd, &bufv, buf, buflen) < 0) {
		nwarn("failed to write buffer to log");
	}

stream_next:
	bytes_written += bytes_to_be_written;
	offset += (uint64_t)bytes_to_be_written;

	if (writev_buffer_flush(k8s_log_fd, &bufv) < 0) {
		nwarn("failed to flush buffer to log");
	}

	return 0;
}
