#define _GNU_SOURCE
#if __STDC_VERSION__ >= 199901L
/* C99 or later */
#else
#error conmon.c requires C99 or later
#endif

#include "utils.h"
#include "ctr_logging.h"
#include "cgroup.h"
#include "cli.h"
#include "globals.h"
#include "oom.h"
#include "conn_sock.h"
#include "ctrl.h"
#include "ctr_stdio.h"
#include "config.h"
#include "parent_pipe_fd.h"
#include "ctr_exit.h"
#include "close_fds.h"
#include "runtime_args.h"

#include <sys/stat.h>

#ifdef __FreeBSD__
#define OPEN_FILES_DIR "/dev/fd"
#else
#define OPEN_FILES_DIR "/proc/self/fd"
#endif

static int open_files_max_fd;
static fd_set *open_files_set;

static void __attribute__((constructor)) init(void)
{
	struct dirent *ent;
	ssize_t size = 0;
	DIR *d;

	d = opendir(OPEN_FILES_DIR);
	if (!d)
		return;

	for (ent = readdir(d); ent; ent = readdir(d)) {
		int fd;

		if (ent->d_name[0] == '.')
			continue;

		fd = atoi(ent->d_name);
		if (fd == dirfd(d))
			continue;

		if (fd >= size * FD_SETSIZE) {
			int i;
			ssize_t new_size;

			new_size = (fd / FD_SETSIZE) + 1;
			open_files_set = realloc(open_files_set, new_size * sizeof(fd_set));
			if (open_files_set == NULL)
				_exit(EXIT_FAILURE);

			for (i = size; i < new_size; i++)
				FD_ZERO(&(open_files_set[i]));

			size = new_size;
		}

		if (fd > open_files_max_fd)
			open_files_max_fd = fd;

		FD_SET(fd % FD_SETSIZE, &(open_files_set[fd / FD_SETSIZE]));
	}
	closedir(d);
}

void close_other_fds(void)
{
	int fd;

	if (open_files_set == NULL)
		return;
	for (fd = 3; fd <= open_files_max_fd; fd++) {
		if (fd != sync_pipe_fd && FD_ISSET(fd % FD_SETSIZE, &(open_files_set[fd / FD_SETSIZE])))
			close(fd);
	}
}

void close_all_fds_ge_than(int firstfd)
{
	struct dirent *ent;
	DIR *d;

	d = opendir(OPEN_FILES_DIR);
	if (!d)
		return;

	for (ent = readdir(d); ent; ent = readdir(d)) {
		int fd;

		if (ent->d_name[0] == '.')
			continue;

		fd = atoi(ent->d_name);
		if (fd == dirfd(d))
			continue;
		if (fd >= firstfd)
			close(fd);
	}
	closedir(d);
}

static int parse_fd_count(const char *value, const char *name)
{
	char *end;
	long count;

	errno = 0;
	count = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || count < 0 || count > INT_MAX - 3)
		_pexitf("Invalid %s %.10s", name, value);
	return (int)count;
}

void close_runtime_fds(void)
{
	const char *listenfds = getenv("LISTEN_FDS");
	int nfds = listenfds ? parse_fd_count(listenfds, "LISTEN_FDS") : 0;
	int preserved_fds = 0;

	if (opt_runtime_opts) {
		for (size_t i = 0; opt_runtime_opts[i]; i++) {
			const char *arg = opt_runtime_opts[i];

			if (strcmp(arg, "--preserve-fds") == 0) {
				if (opt_runtime_opts[i + 1] == NULL)
					_pexitf("Missing value for %s", arg);
				preserved_fds = parse_fd_count(opt_runtime_opts[++i], "--preserve-fds");
			} else if (g_str_has_prefix(arg, "--preserve-fds=")) {
				preserved_fds = parse_fd_count(arg + strlen("--preserve-fds="), "--preserve-fds");
			}
		}
	}

	nfds += preserved_fds;

	/* The runtime already inherited these variables; do not leak them any further.  */
	unsetenv("LISTEN_FDS");
	unsetenv("LISTEN_PID");
	unsetenv("LISTEN_FDNAMES");

	/* Only close inherited descriptors, and forget them before their numbers can be reused.  */
	if (open_files_set == NULL)
		return;
	for (int fd = 3; fd < 3 + nfds && fd <= open_files_max_fd; fd++) {
		if (FD_ISSET(fd % FD_SETSIZE, &(open_files_set[fd / FD_SETSIZE]))) {
			close(fd);
			FD_CLR(fd % FD_SETSIZE, &(open_files_set[fd / FD_SETSIZE]));
		}
	}
}
