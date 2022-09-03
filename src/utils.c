#define _GNU_SOURCE

#include "utils.h"
#include <string.h>
#include <strings.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/signalfd.h>
#endif
#ifdef __FreeBSD__
#include <sys/procctl.h>
#include <sys/event.h>
#endif

log_level_t log_level = WARN_LEVEL;
char *log_cid = NULL;
gboolean use_syslog = FALSE;

/* Set the log level for this call. log level defaults to warning.
   parse the string value of level_name to the appropriate log_level_t enum value
*/
void set_conmon_logs(char *level_name, char *cid_, gboolean syslog_, char *tag)
{
	if (tag == NULL)
		log_cid = cid_;
	else
		log_cid = g_strdup_printf("%s: %s", cid_, tag);
	use_syslog = syslog_;
	// log_level is initialized as Warning, no need to set anything
	if (level_name == NULL)
		return;
	if (!strcasecmp(level_name, "error") || !strcasecmp(level_name, "fatal") || !strcasecmp(level_name, "panic")) {
		log_level = EXIT_LEVEL;
		return;
	} else if (!strcasecmp(level_name, "warn") || !strcasecmp(level_name, "warning")) {
		log_level = WARN_LEVEL;
		return;
	} else if (!strcasecmp(level_name, "info")) {
		log_level = INFO_LEVEL;
		return;
	} else if (!strcasecmp(level_name, "debug")) {
		log_level = DEBUG_LEVEL;
		return;
	} else if (!strcasecmp(level_name, "trace")) {
		log_level = TRACE_LEVEL;
		return;
	}
	ntracef("set log level to %s", level_name);
	nexitf("No such log level %s", level_name);
}

#ifdef __FreeBSD__
static bool retryable_error(int err)
{
	return err == EINTR || err == EAGAIN;
}
#else
static bool retryable_error(int err)
{
	return err == EINTR;
}
#endif

static void get_signal_descriptor_mask(sigset_t *set)
{
	sigemptyset(set);
	sigaddset(set, SIGCHLD);
	sigaddset(set, SIGUSR1);
	sigprocmask(SIG_BLOCK, set, NULL);
}

ssize_t write_all(int fd, const void *buf, size_t count)
{
	size_t remaining = count;
	const char *p = buf;
	ssize_t res;

	while (remaining > 0) {
		do {
			res = write(fd, p, remaining);
		} while (res == -1 && retryable_error(errno));

		if (res <= 0)
			return -1;

		remaining -= res;
		p += res;
	}

	return count;
}

#ifdef __linux__

int set_subreaper(gboolean enabled)
{
	return prctl(PR_SET_CHILD_SUBREAPER, enabled, 0, 0, 0);
}

int set_pdeathsig(int sig)
{
	return prctl(PR_SET_PDEATHSIG, sig);
}

int get_signal_descriptor()
{
	sigset_t set;
	get_signal_descriptor_mask(&set);
	return signalfd(-1, &set, SFD_CLOEXEC);
}

void drop_signal_event(int fd)
{
	struct signalfd_siginfo siginfo;
	ssize_t s = read(fd, &siginfo, sizeof siginfo);
	g_assert_cmpint(s, ==, sizeof siginfo);
}

#endif

#ifdef __FreeBSD__

int set_subreaper(gboolean enabled)
{
	if (enabled) {
		return procctl(P_PID, getpid(), PROC_REAP_ACQUIRE, NULL);
	} else {
		return procctl(P_PID, getpid(), PROC_REAP_RELEASE, NULL);
	}
}

int set_pdeathsig(int sig)
{
	return procctl(P_PID, getpid(), PROC_PDEATHSIG_CTL, &sig);
}

int get_signal_descriptor()
{
	sigset_t set;
	get_signal_descriptor_mask(&set);

	int kq = kqueue();
	fcntl(kq, F_SETFD, FD_CLOEXEC);
	for (int sig = 1; sig < SIGRTMIN; sig++) {
		if (sigismember(&set, sig)) {
			struct kevent kev;
			EV_SET(&kev, sig, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
			if (kevent(kq, &kev, 1, NULL, 0, NULL)) {
				pexitf("failed to add kevent signal %d", sig);
			}
		}
	}
	return kq;
}

void drop_signal_event(int kq)
{
	struct kevent kev;
	int n = kevent(kq, NULL, 0, &kev, 1, NULL);
	if (n != 1) {
		pexit("failed to read signal event");
	}
}

#endif
