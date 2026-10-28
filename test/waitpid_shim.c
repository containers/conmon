/*
 * LD_PRELOAD shim used by 08-exec.bats to order conmon against the runtime.
 *
 * The first waitpid(-1, ...) conmon makes reads the runtime's pid from
 * $CONMON_TEST_RUNTIME_PIDFILE, creates the file named by
 * $CONMON_TEST_WAITPID_MARK, and waits for the runtime to exit without
 * reaping it. Only then does it call the real waitpid().
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char *mark;
static char *runtime_pidfile;

__attribute__((constructor)) static void init(void)
{
	const char *m = getenv("CONMON_TEST_WAITPID_MARK");
	const char *p = getenv("CONMON_TEST_RUNTIME_PIDFILE");

	if (m == NULL || p == NULL)
		return;
	mark = strdup(m);
	runtime_pidfile = strdup(p);
	/* Keep the shim out of the runtime and everything else conmon runs. */
	unsetenv("LD_PRELOAD");
	unsetenv("CONMON_TEST_WAITPID_MARK");
	unsetenv("CONMON_TEST_RUNTIME_PIDFILE");
}

static pid_t read_runtime_pid(void)
{
	for (int i = 0; i < 1000; i++) {
		FILE *f = fopen(runtime_pidfile, "r");
		int pid = 0;

		if (f != NULL) {
			if (fscanf(f, "%d", &pid) != 1)
				pid = 0;
			fclose(f);
		}
		if (pid > 0)
			return pid;
		usleep(10000);
	}
	return -1;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
	static pid_t (*real_waitpid)(pid_t, int *, int);

	if (real_waitpid == NULL)
		real_waitpid = (pid_t (*)(pid_t, int *, int))dlsym(RTLD_NEXT, "waitpid");

	if (mark != NULL && pid == -1) {
		pid_t runtime = read_runtime_pid();

		if (runtime > 0) {
			siginfo_t info;
			int fd = open(mark, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);

			if (fd >= 0)
				close(fd);
			while (waitid(P_PID, runtime, &info, WEXITED | WNOWAIT) < 0 && errno == EINTR)
				;
		}
		free(mark);
		mark = NULL;
	}
	return real_waitpid(pid, status, options);
}
