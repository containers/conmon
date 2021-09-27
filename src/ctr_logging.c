#define _GNU_SOURCE
#include "ctr_logging.h"
#include "cli.h"
#include <string.h>
#include <math.h>
#include <sys/mman.h>

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

/* strlen("1997-03-25T13:20:42.999999999+01:00 stdout F ") + 1 */
#define TSBUFLEN 46

/* Different types of container logging */
static gboolean use_journald_logging = FALSE;
static gboolean use_k8s_logging = FALSE;
static gboolean use_logging_passthrough = FALSE;

/* Value the user must input for each log driver */
static const char *const K8S_FILE_STRING = "k8s-file";
static const char *const JOURNALD_FILE_STRING = "journald";

/* Max log size for any log file types */
static int64_t log_size_max = -1;

/* k8s log file parameters */
static int k8s_log_fd = -1;
static char *k8s_log_path = NULL;

/* journald log file parameters */
#define TRUNC_ID_LEN 12
#define MESSAGE_EQ_LEN 8
#define PRIORITY_EQ_LEN 10
#define CID_FULL_EQ_LEN 18
#define CID_EQ_LEN 13
#define NAME_EQ_LEN 15
#define PARTIAL_MESSAGE_EQ_LEN 30
static char short_cuuid[TRUNC_ID_LEN + 1];
static char *cuuid = NULL;
static char *name = NULL;
static size_t cuuid_len = 0;
static size_t name_len = 0;
static char *container_id_full = NULL;
static char *container_id = NULL;
static char *container_name = NULL;
static size_t container_name_len = 0;
static char *container_tag = NULL;
static size_t container_tag_len = 0;

static void parse_log_path(char *log_config);
static const char *stdpipe_name(stdpipe_t pipe);
static int write_journald(stdpipe_t pipe, ssize_t num_read);
static int write_k8s_log(stdpipe_t pipe, ssize_t buflen);
static bool get_line_len(ptrdiff_t *line_len, const char *buf, ssize_t buflen);
static ssize_t writev_buffer_append_segment(writev_buffer_t *buf, int fd, const void *data, ssize_t len);
static ssize_t writev_buffer_flush(writev_buffer_t *buf, int fd);
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
void configure_log_drivers(gchar **log_drivers, int64_t log_size_max_, char *cuuid_, char *name_, char *tag)
{
	log_size_max = log_size_max_;
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
		if (tag) {
			container_tag = g_strdup_printf("CONTAINER_TAG=%s", tag);
			container_tag_len = strlen(container_tag);
		}

		/* To maintain backwards compatibility with older versions of conmon, we need to skip setting
		 * the name value if it isn't present
		 */
		if (name) {
			/* save the length so we don't have to compute every sd_journal_* call */
			name_len = strlen(name);
			container_name = g_strdup_printf("CONTAINER_NAME=%s", name);
			container_name_len = strlen(container_name);
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
bool write_to_logs(stdpipe_t pipe, ssize_t num_read)
{
	if (use_k8s_logging && write_k8s_log(pipe, num_read) < 0) {
		nwarn("write_k8s_log failed");
		return G_SOURCE_CONTINUE;
	}
	if (use_journald_logging && write_journald(pipe, num_read) < 0) {
		nwarn("write_journald failed");
		return G_SOURCE_CONTINUE;
	}
	return true;
}


/* write to systemd journal. If the pipe is stdout, write with notice priority,
 * otherwise, write with error priority
 */
static int write_journald(stdpipe_t pipe, ssize_t buflen)
{
	char *buf = writev_buffer.buf;

	/* Since we know the priority values for the journal (6 being log info and
	 * 3 being log err) we can set it statically here. This will also save on
	 * runtime, at the expense of needing to be changed if this convention is
	 * changed.
	 */
	char *message_priority = "PRIORITY=6";
	if (pipe == STDERR_PIPE)
		message_priority = "PRIORITY=3";

	ptrdiff_t line_len = 0;

	/*
	 * Writing to journald requires one `sd_journal_sendv()` call per line in
	 * the buffer received from the pipe.  
	 */
	struct iovec sendv_vecs[7] = {
		{ (char *)0, 0 },					 // filled in as we go
		{ container_id_full, cuuid_len + CID_FULL_EQ_LEN },
		{ message_priority, PRIORITY_EQ_LEN },
		{ container_id, TRUNC_ID_LEN + CID_EQ_LEN },
	};
	ssize_t sendv_vecs_len = 4;
    if (container_tag) {
		sendv_vecs[sendv_vecs_len].iov_base = (void *)container_tag;
		sendv_vecs[sendv_vecs_len].iov_len = container_tag_len;
		sendv_vecs_len += 1;
	}
	if (name) {
		sendv_vecs[sendv_vecs_len].iov_base = (void *)container_name;
		sendv_vecs[sendv_vecs_len].iov_len = container_name_len;
		sendv_vecs_len += 1;
	}

	char *msg_buf = (char *)alloca(buflen + MESSAGE_EQ_LEN);
	memcpy(msg_buf, "MESSAGE=", MESSAGE_EQ_LEN);

	while (buflen > 0) {
		bool partial = get_line_len(&line_len, buf, buflen);

		// Fill in the message
		memcpy(msg_buf + MESSAGE_EQ_LEN, buf, line_len);
		sendv_vecs[0].iov_base = (void *)msg_buf;
		sendv_vecs[0].iov_len = buflen + MESSAGE_EQ_LEN;

		/* per docker journald logging format, CONTAINER_PARTIAL_MESSAGE is set to true if it's partial, but otherwise not set. */
		if (partial) {
			sendv_vecs[sendv_vecs_len].iov_base = "CONTAINER_PARTIAL_MESSAGE=true";
			sendv_vecs[sendv_vecs_len].iov_len = PARTIAL_MESSAGE_EQ_LEN;
			sendv_vecs_len += 1;
		}

		int err = sd_journal_sendv(sendv_vecs, sendv_vecs_len);
		if (err < 0) {
			pwarn(strerror(err));
			return err;
		}

		if (partial) {
			// We don't have to do this, because we'll only get a partial line at the end.
			sendv_vecs_len -= 1;
		}

		buf += line_len;
		buflen -= line_len;
	}
	return 0;
}

/*
 * The CRI requires us to write logs with a (timestamp, stream, line) format
 * for every newline-separated line. write_k8s_log writes said format for every
 * line in buf, and will partially write the final line of the log if buf is
 * not terminated by a newline.
 */
static int write_k8s_log(stdpipe_t pipe, ssize_t buflen)
{
	char *buf = writev_buffer.buf;
	static int64_t bytes_written = 0;
	int64_t bytes_to_be_written = 0;

	/*
	 * Use the same timestamp for every line of the log in this buffer, as
	 * every log in this buffer was read from the pipe at the same time.
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

		/* If partial, then we add a \n, and change the default 'F' to a 'P'. */
		if (partial) {
			bytes_to_be_written += 1;
			tsbuf[TSBUFLEN - 3] = 'P';
		}

		/*
		 * We re-open the log file if writing out the bytes will exceed the max
		 * log size. We also reset the state so that the new file is started with
		 * a timestamp.
		 */
		if ((log_size_max > 0) && (bytes_written + bytes_to_be_written) > log_size_max) {
			bytes_written = 0;

			if (writev_buffer_flush(&writev_buffer, k8s_log_fd) < 0) {
				nwarn("failed to flush buffer to log");
				/*
				 * We are going to reopen the file anyway, in case of
				 * errors discard all we have in the buffer.
				 */
				writev_buffer.iovcnt = 0;
			}
			reopen_k8s_file();
		}

		/* Output the timestamp */
		if (writev_buffer_append_segment(&writev_buffer, k8s_log_fd, tsbuf, TSBUFLEN - 1) < 0) {
			nwarn("failed to write (timestamp, stream) to log");
			goto next;
		}

		/* Output the actual contents. */
		if (writev_buffer_append_segment(&writev_buffer, k8s_log_fd, buf, line_len) < 0) {
			nwarn("failed to write buffer to log");
			goto next;
		}

		/* Output a newline for partial */
		if (partial) {
			if (writev_buffer_append_segment(&writev_buffer, k8s_log_fd, "\n", 1) < 0) {
				nwarn("failed to write newline to log");
				goto next;
			}
		}

		bytes_written += bytes_to_be_written;
	next:
		/* Update the head of the buffer remaining to output. */
		buf += line_len;
		buflen -= line_len;
	}

	if (writev_buffer_flush(&writev_buffer, k8s_log_fd) < 0) {
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


/*
 * writev_buffer "class", of sorts.
 *
 * The writev_buffer_t global contains the fields of the object, and the
 * writev_buffer_*() functions are the methods that act on that object.
 *
 * In this case, the object is a singleton, the global variable, writev_buffer.
 */

/* Logging buffer that describes the mmap'd memory used for read and writing. */
writev_buffer_t writev_buffer = {0};

/*
 * We size the I/O vectors to handle an average log line length, including the
 * new line character, of AVG_LOG_LINE_TARGET bytes in order to allocate enough
 * I/O vectors to only require one writev() system call per read() system call
 * from a pipe.  If the average line length is less than AVG_LOG_LINE_TARGET,
 * then we'll end up using potentially many more writev() system calls to write
 * out the entire buffer read from a pipe.
 */
#define AVG_LOG_LINE_TARGET (float)25.0


void writev_buffer_init(int pipe_size)
{
	// Allocate a buffer that matches the size of a pipe, along with the
	// requisite I/O vectors, optimized for log lines >= AVG_LOG_LINE_TARGET
	// bytes for the size of the buffer given.

	// WARNING - This means that any buffer processed with average log
	// lines below AVG_LOG_LINE_TARGET bytes will result in multiple writev()
	// system calls per buffer read.  E.g., at a pipe size of 64 KB, for an
	// average of 16 byte lines (4,096 per 64 KB buffer), it would require
	// 8,192 I/O vectors (32 pages).

	// It takes 2 I/O vectors per new-line (timestamp + full_partial, and
	// the actual log line).  We divide the pipe_size by AVG_LOG_LINE_TARGET,
	// taking the ceiling() so that we have an I/O vector for the remainder,
	// and add one if the last buffer does not contain a new-line character.
	// We calculate the total size of the I/O vectors in bytes, and then round
	// up to the nearest page boundary.  The pipe size (in bytes, but always
	// rounded to the nearest page) is then added to that so we can allocate
	// both structures in one set of anonymous mapped pages.

	int target_lines_cnt = (int)ceilf((float)(pipe_size / AVG_LOG_LINE_TARGET)) + 1;
	if (target_lines_cnt < 0)
		nexitf("Logic bomb!  target # of lines per pipe is negative!  pipe_size = %d, target_lines_cnt = %d", pipe_size, target_lines_cnt);

	int iovectors_bytes_page_aligned;
	if (use_k8s_logging) {
		unsigned int iovectors_bytes = sizeof(struct iovec) * (unsigned int)target_lines_cnt;
		iovectors_bytes_page_aligned = (int)ceilf(iovectors_bytes / (float)getpagesize()) * getpagesize();
	}
	else {
		iovectors_bytes_page_aligned = 0;
	}

	char *memory = mmap(NULL, iovectors_bytes_page_aligned + pipe_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS, -1, 0);
	if (memory == NULL)
		nexitf("mmap() failed for I/O vectors and buffer");

	if (use_k8s_logging) {
		writev_buffer.iov = (struct iovec *)memory;
		writev_buffer.iovcnt_max = target_lines_cnt;
	}

	writev_buffer.buf = &memory[iovectors_bytes_page_aligned];
	writev_buffer.buf_len = pipe_size;
}


static ssize_t writev_buffer_flush(writev_buffer_t *buf, int fd)
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


ssize_t writev_buffer_append_segment(writev_buffer_t *buf, int fd, const void *data, ssize_t len)
{
	if (data == NULL)
		return 1;

	if (buf->iovcnt == buf->iovcnt_max && writev_buffer_flush(buf, fd) < 0)
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
		return "stdin ";
	case STDOUT_PIPE:
		return "stdout";
	case STDERR_PIPE:
		return "stderr";
	default:
		return "NONE  ";
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

	int len = snprintf(buf, buflen, "%d-%02d-%02dT%02d:%02d:%02d.%09ld%c%02d:%02d %s F ", current_tm.tm_year + 1900,
			   current_tm.tm_mon + 1, current_tm.tm_mday, current_tm.tm_hour, current_tm.tm_min, current_tm.tm_sec, ts.tv_nsec,
			   off_sign, off / 3600, (off % 3600) / 60, pipename);

	if (len < buflen)
		err = 0;
	return err;
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
