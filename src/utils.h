#pragma once
#if !defined(UTILS_H)
#define UTILS_H

#include <stdio.h>
#include <syslog.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <glib.h>
#include <glib-unix.h>
#include <sys/uio.h>
#include <string.h>

/* stdpipe_t represents one of the std pipes (or NONE).
 * Sync with const in container_attach.go */
typedef enum {
	NO_PIPE,
	STDIN_PIPE, /* unused */
	STDOUT_PIPE,
	STDERR_PIPE,
} stdpipe_t;

/* Different levels of logging */
typedef enum {
	EXIT_LEVEL,
	WARN_LEVEL,
	INFO_LEVEL,
	DEBUG_LEVEL,
	TRACE_LEVEL,
} log_level_t;

// Default log level is Warning, This will be configured before any logging
// should happen
extern log_level_t log_level;
extern char *log_cid;
extern gboolean use_syslog;


#define _pexit(s) \
	do { \
		int saved_errno = errno; \
		errno = saved_errno; \
		fprintf(stderr, "[conmon:e]: %s %m\n", s); \
		if (use_syslog) { \
			errno = saved_errno; \
			syslog(LOG_ERR, "conmon %.20s <error>: %s %m\n", log_cid, s); \
		} \
		_exit(EXIT_FAILURE); \
	} while (0)

#define _pexitf(fmt, ...) \
	do { \
		int saved_errno = errno; \
		errno = saved_errno; \
		fprintf(stderr, "[conmon:e]: " fmt " %m\n", ##__VA_ARGS__); \
		if (use_syslog) { \
			errno = saved_errno; \
			syslog(LOG_ERR, "conmon %.20s <error>: " fmt ": %m\n", log_cid, ##__VA_ARGS__); \
		} \
		_exit(EXIT_FAILURE); \
	} while (0)

#define pexit(s) \
	do { \
		int saved_errno = errno; \
		errno = saved_errno; \
		fprintf(stderr, "[conmon:e]: %s %m\n", s); \
		if (use_syslog) { \
			errno = saved_errno; \
			syslog(LOG_ERR, "conmon %.20s <error>: %s %m\n", log_cid, s); \
		} \
		exit(EXIT_FAILURE); \
	} while (0)

#define pexitf(fmt, ...) \
	do { \
		int saved_errno = errno; \
		errno = saved_errno; \
		fprintf(stderr, "[conmon:e]: " fmt " %m\n", ##__VA_ARGS__); \
		if (use_syslog) { \
			errno = saved_errno; \
			syslog(LOG_ERR, "conmon %.20s <error>: " fmt ": %m\n", log_cid, ##__VA_ARGS__); \
		} \
		exit(EXIT_FAILURE); \
	} while (0)

#define nexit(s) \
	do { \
		fprintf(stderr, "[conmon:e]: %s\n", s); \
		if (use_syslog) \
			syslog(LOG_ERR, "conmon %.20s <error>: %s\n", log_cid, s); \
		exit(EXIT_FAILURE); \
	} while (0)

#define nexitf(fmt, ...) \
	do { \
		fprintf(stderr, "[conmon:e]: " fmt "\n", ##__VA_ARGS__); \
		if (use_syslog) \
			syslog(LOG_ERR, "conmon %.20s <error>: " fmt "\n", log_cid, ##__VA_ARGS__); \
		exit(EXIT_FAILURE); \
	} while (0)

#define pwarn(s) \
	do { \
		if (log_level >= WARN_LEVEL) { \
			int saved_errno = errno; \
			errno = saved_errno; \
			fprintf(stderr, "[conmon:w]: %s %m\n", s); \
			if (use_syslog) { \
				errno = saved_errno; \
				syslog(LOG_INFO, "conmon %.20s <pwarn>: %s %m\n", log_cid, s); \
			} \
		} \
	} while (0)

#define pwarnf(fmt, ...) \
	do { \
		if (log_level >= WARN_LEVEL) { \
			int saved_errno = errno; \
			errno = saved_errno; \
			fprintf(stderr, "[conmon:w]: " fmt " %m\n", ##__VA_ARGS__); \
			if (use_syslog) { \
				errno = saved_errno; \
				syslog(LOG_INFO, "conmon %.20s <pwarnf>: " fmt ": %m\n", log_cid, ##__VA_ARGS__); \
			} \
		} \
	} while (0)

#define nwarn(s) \
	if (log_level >= WARN_LEVEL) { \
		do { \
			fprintf(stderr, "[conmon:w]: %s\n", s); \
			if (use_syslog) \
				syslog(LOG_INFO, "conmon %.20s <nwarn>: %s\n", log_cid, s); \
		} while (0); \
	}

#define nwarnf(fmt, ...) \
	if (log_level >= WARN_LEVEL) { \
		do { \
			fprintf(stderr, "[conmon:w]: " fmt "\n", ##__VA_ARGS__); \
			if (use_syslog) \
				syslog(LOG_INFO, "conmon %.20s <nwarn>: " fmt "\n", log_cid, ##__VA_ARGS__); \
		} while (0); \
	}

#define ninfo(s) \
	if (log_level >= INFO_LEVEL) { \
		do { \
			fprintf(stderr, "[conmon:i]: %s\n", s); \
			if (use_syslog) \
				syslog(LOG_INFO, "conmon %.20s <ninfo>: %s\n", log_cid, s); \
		} while (0); \
	}

#define ninfof(fmt, ...) \
	if (log_level >= INFO_LEVEL) { \
		do { \
			fprintf(stderr, "[conmon:i]: " fmt "\n", ##__VA_ARGS__); \
			if (use_syslog) \
				syslog(LOG_INFO, "conmon %.20s <ninfo>: " fmt "\n", log_cid, ##__VA_ARGS__); \
		} while (0); \
	}

#define ndebug(s) \
	if (log_level >= DEBUG_LEVEL) { \
		do { \
			fprintf(stderr, "[conmon:d]: %s\n", s); \
			if (use_syslog) \
				syslog(LOG_INFO, "conmon %.20s <ndebug>: %s\n", log_cid, s); \
		} while (0); \
	}

#define ndebugf(fmt, ...) \
	if (log_level >= DEBUG_LEVEL) { \
		do { \
			fprintf(stderr, "[conmon:d]: " fmt "\n", ##__VA_ARGS__); \
			if (use_syslog) \
				syslog(LOG_INFO, "conmon %.20s <ndebug>: " fmt "\n", log_cid, ##__VA_ARGS__); \
		} while (0); \
	}

#define ntrace(s) \
	if (log_level >= TRACE_LEVEL) { \
		do { \
			fprintf(stderr, "[conmon:d]: %s\n", s); \
			if (use_syslog) \
				syslog(LOG_INFO, "conmon %.20s <ntrace>: %s\n", log_cid, s); \
		} while (0); \
	}

#define ntracef(fmt, ...) \
	if (log_level >= TRACE_LEVEL) { \
		do { \
			fprintf(stderr, "[conmon:d]: " fmt "\n", ##__VA_ARGS__); \
			if (use_syslog) \
				syslog(LOG_INFO, "conmon %.20s <ntrace>: " fmt "\n", log_cid, ##__VA_ARGS__); \
		} while (0); \
	}

/* Set the log level for this call. log level defaults to warning.
   parse the string value of level_name to the appropriate log_level_t enum value
*/
void set_conmon_logs(char *level_name, char *cid_, gboolean syslog_, char *tag);

#define _cleanup_(x) __attribute__((cleanup(x)))

static inline void freep(void *p)
{
	free(*(void **)p);
}

static inline void closep(int *fd)
{
	if (*fd >= 0)
		close(*fd);
	*fd = -1;
}

static inline void fclosep(FILE **fp)
{
	if (*fp)
		fclose(*fp);
	*fp = NULL;
}

static inline void gstring_free_cleanup(GString **string)
{
	if (*string)
		g_string_free(*string, TRUE);
}

static inline void gerror_free_cleanup(GError **err)
{
	if (*err)
		g_error_free(*err);
}

static inline void strv_cleanup(char ***strv)
{
	if (strv)
		g_strfreev(*strv);
}

static inline void hashtable_free_cleanup(GHashTable **tbl)
{
	if (tbl)
		g_hash_table_destroy(*tbl);
}

#define _cleanup_free_ _cleanup_(freep)
#define _cleanup_close_ _cleanup_(closep)
#define _cleanup_fclose_ _cleanup_(fclosep)
#define _cleanup_gerror_ _cleanup_(gerror_free_cleanup)


ssize_t write_all(int fd, const void *buf, size_t buflen);
typedef struct {
	size_t iovcnt;
	size_t max_iovcnt;
	struct iovec *iov;
} writev_iov_t;
ssize_t writev_all(int fd, writev_iov_t *iov);

int set_subreaper(gboolean enabled);

int set_pdeathsig(int sig);

int get_signal_descriptor();
void drop_signal_event(int fd);

#endif /* !defined(UTILS_H) */
