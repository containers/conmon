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

#include <sys/prctl.h>
#include <sys/stat.h>

static int open_files_max_fd;
static fd_set *open_files_set;

static GHashTable *fd_sources;

void remove_g_unix_fd(gpointer key, gpointer value, gpointer user_data);

/* Initialize fds passed by caller, so we can differentiate
 * between them and the ones we open ourselves
 */
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

/* Close fds passed by caller */
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

/* Save the GLib sources returned so they can be removed later */
void add_save_g_unix_fd(guint fd, GIOCondition condition, GUnixFDSourceFunc function, gpointer user_data)
{
	if (fd_sources == NULL)
		fd_sources = g_hash_table_new(g_int_hash, g_int_equal);

	guint source = g_unix_fd_add(fd, condition, function, user_data);
	g_hash_table_insert(fd_sources, &fd, &source);
}

void close_remove_g_unix_fd(guint fd)
{
	close(fd);
	remove_g_unix_fd(NULL, &fd, NULL);
}

/* Remove all saved GLib sources */
void remove_g_unix_fds()
{
	if (fd_sources == NULL)
		return;

	g_hash_table_foreach(fd_sources, remove_g_unix_fd, NULL);
	g_hash_table_destroy(fd_sources);
	fd_sources = NULL;
}

void remove_g_unix_fd(G_GNUC_UNUSED gpointer key, gpointer value, G_GNUC_UNUSED gpointer user_data) {
	guint *source = (guint *) value;
	if (source == NULL)
		return;
	g_source_remove(*source);
}
