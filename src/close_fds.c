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

static int open_files_max_fd;
static fd_set *open_files_set;

static void __attribute__((constructor)) init()
{
	struct dirent *ent;
	ssize_t size = 0;
	DIR *d;

	d = opendir("/proc/self/fd");
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

void close_other_fds()
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

	d = opendir("/proc/self/fd");
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
