#define _GNU_SOURCE
#include "ctr_logging.h"
#include "cli.h"
#include "config.h"
#include <string.h>

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
static gboolean use_logging_passthrough = FALSE;

/* Value the user must input for each log driver */
static const char *const K8S_FILE_STRING = "k8s-file";
static const char *const JOURNALD_FILE_STRING = "journald";

/* Max log size for any log file types */
static int64_t log_size_max = -1;

/* Max total log size for any log file types */
static int64_t log_global_size_max = -1;

/* k8s log file parameters */
static int k8s_log_fd = -1;
static char *k8s_log_path = NULL;

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
static size_t container_tag_len;
static char *syslog_identifier = NULL;
static size_t syslog_identifier_len;

typedef struct {
	int iovcnt;
	struct iovec iov[WRITEV_BUFFER_N_IOV];
} writev_buffer_t;

static void parse_log_path(char *log_config);
static const char *stdpipe_name(stdpipe_t pipe);
static int write_journald(int pipe, char *buf, ssize_t num_read);
static int write_k8s_log(stdpipe_t pipe, const char *buf, ssize_t buflen);
static bool get_line_len(ptrdiff_t *line_len, const char *buf, ssize_t buflen);
static ssize_t writev_buffer_append_segment(int fd, writev_buffer_t *buf, const void *data, ssize_t len);
static ssize_t writev_buffer_flush(int fd, writev_buffer_t *buf);
static int set_k8s_timestamp(char *buf, ssize_t buflen, const char *pipename);
static void reopen_k8s_file(void);


gboolean logging_is_passthrough(void)
{
	return use_logging_passthrough;
}

/*
 * configures container log specific information, such as the drivers the user
 * called with and the max log size for log file types. For the log file types
 * (currently just k8s log file), it will also open the log_fd for that specific
 * log file.
 */
void configure_log_drivers(gchar **log_drivers, int64_t log_size_max_, int64_t log_global_size_max_, char *cuuid_, char *name_, char *tag)
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
		k8s_log_fd = open(k8s_log_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
		if (k8s_log_fd < 0)
			pexit("Failed to open log file");

		if (!use_journald_logging && tag)
			nexit("k8s-file doesn't support --log-tag");
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
	}
}

/*
 * parse_log_path branches on log driver type the user inputted.
 * log_config will either be a ':' delimited string containing:
 * <DRIVER_NAME>:<PATH_NAME> or <PATH_NAME>
 * in the case of no colon, the driver will be kubernetes-log-file,
 * in the case the log driver is 'journald', the <PATH_NAME> is ignored.
 * exits with error if <DRIVER_NAME> isn't 'journald' or 'kubernetes-log-file'
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
		if (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO) || isatty(STDERR_FILENO))
			nexitf("cannot use a tty with passthrough logging mode to prevent attacks via TIOCSTI");

		use_logging_passthrough = TRUE;
		return;
	}

	if (!strcmp(driver, JOURNALD_FILE_STRING)) {
		use_journald_logging = TRUE;
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
	if (use_k8s_logging && write_k8s_log(pipe, buf, num_read) < 0) {
		nwarn("write_k8s_log failed");
		return G_SOURCE_CONTINUE;
	}
	if (use_journald_logging && write_journald(pipe, buf, num_read) < 0) {
		nwarn("write_journald failed");
		return G_SOURCE_CONTINUE;
	}
	return true;
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

	/* When using writev_buffer_append_segment, we should never approach the number of
	 * entries necessary to flush the buffer. Therefore, the fd passed in is for /dev/null
	 */
	_cleanup_close_ int dev_null = open("/dev/null", O_WRONLY | O_CLOEXEC);
	if (dev_null < 0)
		pexit("Failed to open /dev/null");

	/* Since we know the priority values for the journal (6 being log info and 3 being log err
	 * we can set it statically here. This will also save on runtime, at the expense of needing
	 * to be changed if this convention is changed.
	 */
	const char *message_priority = "PRIORITY=6";
	if (pipe == STDERR_PIPE) {
		message_priority = "PRIORITY=3";
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

		ssize_t msg_len = line_len + MESSAGE_EQ_LEN + *partial_buf_len;
		partial_buf[*partial_buf_len] = '\0';

		_cleanup_free_ char *message = g_malloc(msg_len);

		memcpy(message, "MESSAGE=", MESSAGE_EQ_LEN);
		memcpy(message + MESSAGE_EQ_LEN, partial_buf, *partial_buf_len);
		memcpy(message + MESSAGE_EQ_LEN + *partial_buf_len, buf, line_len);

		if (writev_buffer_append_segment(dev_null, &bufv, message, msg_len) < 0)
			return -1;

		if (writev_buffer_append_segment(dev_null, &bufv, container_id_full, cuuid_len + CID_FULL_EQ_LEN) < 0)
			return -1;

		if (writev_buffer_append_segment(dev_null, &bufv, message_priority, PRIORITY_EQ_LEN) < 0)
			return -1;

		if (writev_buffer_append_segment(dev_null, &bufv, container_id, TRUNC_ID_LEN + CID_EQ_LEN) < 0)
			return -1;

		if (container_tag && writev_buffer_append_segment(dev_null, &bufv, container_tag, container_tag_len) < 0)
			return -1;

		/* only print the name if we have a name to print */
		if (name && writev_buffer_append_segment(dev_null, &bufv, container_name, name_len + NAME_EQ_LEN) < 0)
			return -1;

		if (writev_buffer_append_segment(dev_null, &bufv, syslog_identifier, syslog_identifier_len) < 0)
			return -1;

		/* per docker journald logging format, CONTAINER_PARTIAL_MESSAGE is set to true if it's partial, but otherwise not set. */
		if (partial && writev_buffer_append_segment(dev_null, &bufv, "CONTAINER_PARTIAL_MESSAGE=true", PARTIAL_MESSAGE_EQ_LEN) < 0)
			return -1;

		int err = sd_journal_sendv(bufv.iov, bufv.iovcnt);
		if (err < 0) {
			pwarn(strerror(-err));
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
	static int64_t bytes_written = 0;
	int64_t bytes_to_be_written = 0;
	static int64_t total_bytes_written = 0;

	/*
	 * Use the same timestamp for every line of the log in this buffer.
	 * There is no practical difference in the output since write(2) is
	 * fast.
	 */
	char tsbuf[TSBUFLEN];
	if (set_k8s_timestamp(tsbuf, sizeof tsbuf, stdpipe_name(pipe)))
		/* TODO: We should handle failures much more cleanly than this. */
		return -1;

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
		if (log_global_size_max > 0 && total_bytes_written >= log_global_size_max)
			break;

		/*
		 * We re-open the log file if writing out the bytes will exceed the max
		 * log size. We also reset the state so that the new file is started with
		 * a timestamp.
		 */
		if ((log_size_max > 0) && (bytes_written + bytes_to_be_written) > log_size_max) {
			bytes_written = 0;

			if (writev_buffer_flush(k8s_log_fd, &bufv) < 0) {
				nwarn("failed to flush buffer to log");
				/*
				 * We are going to reopen the file anyway, in case of
				 * errors discard all we have in the buffer.
				 */
				bufv.iovcnt = 0;
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

		bytes_written += bytes_to_be_written;
		total_bytes_written += bytes_to_be_written;
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
	size_t count = 0;
	int iovcnt = buf->iovcnt;
	struct iovec *iov = buf->iov;

	while (iovcnt > 0) {
		ssize_t res;
		do {
			res = writev(fd, iov, iovcnt);
		} while (res == -1 && errno == EINTR);

		if (res <= 0)
			return -1;

		count += res;

		while (res > 0) {
			size_t from_this = MIN((size_t)res, iov->iov_len);
			iov->iov_len -= from_this;
			iov->iov_base += from_this;
			res -= from_this;

			if (iov->iov_len == 0) {
				iov++;
				iovcnt--;
			}
		}
	}

	buf->iovcnt = 0;

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


static int set_k8s_timestamp(char *buf, ssize_t buflen, const char *pipename)
{
	static int tzset_called = 0;
	int err = -1;

	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
		/* If CLOCK_REALTIME is not supported, we set nano seconds to 0 */
		if (errno == EINVAL) {
			ts.tv_nsec = 0;
		} else {
			return err;
		}
	}

	if (!tzset_called) {
		tzset();
		tzset_called = 1;
	}

	struct tm current_tm;
	if (localtime_r(&ts.tv_sec, &current_tm) == NULL)
		return err;


	char off_sign = '+';
	int off = (int)current_tm.tm_gmtoff;
	if (current_tm.tm_gmtoff < 0) {
		off_sign = '-';
		off = -off;
	}

	int len = snprintf(buf, buflen, "%d-%02d-%02dT%02d:%02d:%02d.%09ld%c%02d:%02d %s ", current_tm.tm_year + 1900,
			   current_tm.tm_mon + 1, current_tm.tm_mday, current_tm.tm_hour, current_tm.tm_min, current_tm.tm_sec, ts.tv_nsec,
			   off_sign, off / 3600, (off % 3600) / 60, pipename);

	if (len < buflen)
		err = 0;
	return err;
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

/* reopen the k8s log file fd.  */
static void reopen_k8s_file(void)
{
	if (!use_k8s_logging)
		return;

	_cleanup_free_ char *k8s_log_path_tmp = g_strdup_printf("%s.tmp", k8s_log_path);

	/* Sync the logs to disk */
	if (!opt_no_sync_log && fsync(k8s_log_fd) < 0) {
		pwarn("Failed to sync log file on reopen");
	}

	/* Close the current k8s_log_fd */
	close(k8s_log_fd);

	/* Open the log path file again */
	k8s_log_fd = open(k8s_log_path_tmp, O_WRONLY | O_TRUNC | O_CREAT | O_CLOEXEC, 0600);
	if (k8s_log_fd < 0)
		pexitf("Failed to open log file %s", k8s_log_path);

	/* Replace the previous file */
	if (rename(k8s_log_path_tmp, k8s_log_path) < 0) {
		pexit("Failed to rename log file");
	}
}


void sync_logs(void)
{
	/* Sync the logs to disk */
	if (k8s_log_fd > 0)
		if (fsync(k8s_log_fd) < 0)
			pwarn("Failed to sync log file before exit");
}
