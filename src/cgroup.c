#define _GNU_SOURCE

#include "cgroup.h"
#include "globals.h"
#include "utils.h"
#include "cli.h"
#include "config.h"

#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#ifdef __linux__
#include <linux/limits.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#endif

#ifndef CGROUP2_SUPER_MAGIC
#define CGROUP2_SUPER_MAGIC 0x63677270
#endif

#define CGROUP_ROOT "/sys/fs/cgroup"

int oom_event_fd = -1;
int oom_cgroup_fd = -1;

#ifdef __linux__

static char *process_cgroup_subsystem_path(int pid, bool cgroup2, const char *subsystem);
static void setup_oom_handling_cgroup_v2(int pid);
static void setup_oom_handling_cgroup_v1(int pid);
static gboolean oom_cb_cgroup_v2(int fd, GIOCondition condition, G_GNUC_UNUSED gpointer user_data);
static gboolean oom_cb_cgroup_v1(int fd, GIOCondition condition, G_GNUC_UNUSED gpointer user_data);
static int write_oom_files();

void setup_oom_handling(int pid)
{
	struct statfs sfs;

	if (statfs("/sys/fs/cgroup", &sfs) == 0 && sfs.f_type == CGROUP2_SUPER_MAGIC) {
		is_cgroup_v2 = TRUE;
		setup_oom_handling_cgroup_v2(pid);
		return;
	}
	setup_oom_handling_cgroup_v1(pid);
}

/*
 * Returns the path for specified controller name for a pid.
 * Returns NULL on error.
 */
static char *process_cgroup_subsystem_path(int pid, bool cgroup2, const char *subsystem)
{
	_cleanup_free_ char *cgroups_file_path = g_strdup_printf("/proc/%d/cgroup", pid);
	_cleanup_fclose_ FILE *fp = fopen(cgroups_file_path, "re");
	if (fp == NULL) {
		nwarnf("Failed to open cgroups file: %s", cgroups_file_path);
		return NULL;
	}

	_cleanup_free_ char *line = NULL;
	ssize_t read;
	size_t len = 0;
	char *ptr, *path;
	while ((read = getline(&line, &len, fp)) != -1) {
		_cleanup_strv_ char **subsystems = NULL;
		ptr = strchr(line, ':');
		if (ptr == NULL) {
			nwarnf("Error parsing cgroup, ':' not found: %s", line);
			return NULL;
		}
		ptr++;
		path = strchr(ptr, ':');
		if (path == NULL) {
			nwarnf("Error parsing cgroup, second ':' not found: %s", line);
			return NULL;
		}
		*path = 0;
		path++;
		if (cgroup2) {
			char *subsystem_path = g_strdup_printf("%s%s", CGROUP_ROOT, path);
			subsystem_path[strlen(subsystem_path) - 1] = '\0';
			return subsystem_path;
		}
		subsystems = g_strsplit(ptr, ",", -1);
		for (int i = 0; subsystems[i] != NULL; i++) {
			if (strcmp(subsystems[i], subsystem) == 0) {
				char *subpath = strchr(subsystems[i], '=');
				if (subpath == NULL) {
					subpath = ptr;
				} else {
					*subpath = 0;
				}

				char *subsystem_path = g_strdup_printf("%s/%s%s", CGROUP_ROOT, subpath, path);
				subsystem_path[strlen(subsystem_path) - 1] = '\0';
				return subsystem_path;
			}
		}
	}

	return NULL;
}


static void setup_oom_handling_cgroup_v2(int pid)
{
	cgroup2_path = process_cgroup_subsystem_path(pid, true, "");
	if (!cgroup2_path) {
		nwarn("Failed to get cgroup path. Container may have exited");
		return;
	}

	_cleanup_free_ char *memory_events_file_path = g_build_filename(cgroup2_path, "memory.events", NULL);

	_cleanup_close_ int ifd = -1;
	if ((ifd = inotify_init()) < 0) {
		nwarnf("Failed to create inotify fd");
		return;
	}

	if (inotify_add_watch(ifd, memory_events_file_path, IN_MODIFY) < 0) {
		nwarnf("Failed to add inotify watch for %s", memory_events_file_path);
		return;
	}

	/* Move ownership to inotify_fd.  */
	inotify_fd = ifd;
	ifd = -1;

	g_unix_fd_add(inotify_fd, G_IO_IN, oom_cb_cgroup_v2, NULL);
}

static void setup_oom_handling_cgroup_v1(int pid)
{
	/* Setup OOM notification for container process */
	_cleanup_free_ char *memory_cgroup_path = process_cgroup_subsystem_path(pid, false, "memory");
	if (!memory_cgroup_path) {
		nwarn("Failed to get memory cgroup path. Container may have exited");
		return;
	}

	/* this will be cleaned up in oom_cb_cgroup_v1 */
	char *memory_cgroup_file_path = g_build_filename(memory_cgroup_path, "cgroup.event_control", NULL);
	_cleanup_close_ int cfd = open(memory_cgroup_file_path, O_WRONLY | O_CLOEXEC);
	if (cfd == -1) {
		nwarnf("Failed to open %s", memory_cgroup_file_path);
		g_free(memory_cgroup_file_path);
		return;
	}

	_cleanup_free_ char *memory_cgroup_file_oom_path = g_build_filename(memory_cgroup_path, "memory.oom_control", NULL);

	oom_cgroup_fd = open(memory_cgroup_file_oom_path, O_RDONLY | O_CLOEXEC); /* Not closed */
	if (oom_cgroup_fd == -1)
		pexitf("Failed to open %s", memory_cgroup_file_oom_path);

	if ((oom_event_fd = eventfd(0, EFD_CLOEXEC)) == -1)
		pexit("Failed to create eventfd");

	_cleanup_free_ char *data = g_strdup_printf("%d %d", oom_event_fd, oom_cgroup_fd);
	if (write_all(cfd, data, strlen(data)) < 0)
		pexit("Failed to write to cgroup.event_control");

	g_unix_fd_add(oom_event_fd, G_IO_IN, oom_cb_cgroup_v1, memory_cgroup_file_path);
}

static gboolean oom_cb_cgroup_v2(int fd, GIOCondition condition, G_GNUC_UNUSED gpointer user_data)
{
	const size_t events_size = sizeof(struct inotify_event) + NAME_MAX + 1;
	char events[events_size];

	/* Drop the inotify events.  */
	ssize_t num_read = read(fd, &events, events_size);
	if (num_read < 0) {
		nwarn("Failed to read oom event from eventfd in v2");
		return G_SOURCE_CONTINUE;
	}

	gboolean ret = G_SOURCE_REMOVE;
	if ((condition & G_IO_IN) != 0) {
		ret = check_cgroup2_oom();
	}

	if (ret == G_SOURCE_REMOVE) {
		/* End of input */
		close(fd);
		inotify_fd = -1;
	}

	return ret;
}

/* user_data is expected to be the container's cgroup.event_control file,
 * used to verify the cgroup hasn't been cleaned up */
static gboolean oom_cb_cgroup_v1(int fd, GIOCondition condition, gpointer user_data)
{
	char *cgroup_event_control_path = (char *)user_data;
	if ((condition & G_IO_IN) == 0) {
		/* End of input */
		close(fd);
		oom_event_fd = -1;
		g_free(cgroup_event_control_path);
		return G_SOURCE_REMOVE;
	}

	/* Attempt to read the container's cgroup path.
	 * if the cgroup.memory_control file does not exist,
	 * we know one of the events on this fd was a cgroup removal
	 */
	gboolean cgroup_removed = FALSE;
	if (access(cgroup_event_control_path, F_OK) < 0) {
		ndebugf("Memory cgroup removal event received");
		cgroup_removed = TRUE;
	}

	/* there are three cases we need to worry about:
	 * oom kill happened (1 event)
	 * cgroup was removed (1 event)
	 * oom kill happened and cgroup was removed (2 events)
	 */
	uint64_t event_count;
	ssize_t num_read = read(fd, &event_count, sizeof(uint64_t));
	if (num_read < 0) {
		nwarn("Failed to read oom event from eventfd");
		return G_SOURCE_CONTINUE;
	}

	if (num_read == 0) {
		close(fd);
		oom_event_fd = -1;
		g_free(cgroup_event_control_path);
		return G_SOURCE_REMOVE;
	}

	if (num_read != sizeof(uint64_t)) {
		nwarn("Failed to read full oom event from eventfd");
		return G_SOURCE_CONTINUE;
	}

	ndebugf("Memory cgroup event count: %ld", (long)event_count);
	if (event_count == 0) {
		nwarn("Unexpected event count (zero) when reading for oom event");
		return G_SOURCE_CONTINUE;
	}

	/* if there's only one event, and the cgroup was removed
	 * we know the event was for a cgroup removal, not an OOM kill
	 */
	if (event_count == 1 && cgroup_removed)
		return G_SOURCE_CONTINUE;

	/* we catch the two other cases here, both of which are OOM kill events */
	ninfo("OOM event received");
	write_oom_files();

	return G_SOURCE_CONTINUE;
}

gboolean check_cgroup2_oom()
{
	static long int last_counter = 0;

	if (!is_cgroup_v2)
		return G_SOURCE_REMOVE;

	_cleanup_free_ char *memory_events_file_path = g_build_filename(cgroup2_path, "memory.events", NULL);

	_cleanup_fclose_ FILE *fp = fopen(memory_events_file_path, "re");
	if (fp == NULL) {
		nwarnf("Failed to open cgroups file: %s", memory_events_file_path);
		return G_SOURCE_CONTINUE;
	}

	_cleanup_free_ char *line = NULL;
	size_t len = 0;
	ssize_t read;
	while ((read = getline(&line, &len, fp)) != -1) {
		long int counter;
		const int oom_len = 4, oom_kill_len = 9;

		if (read >= oom_kill_len + 2 && memcmp(line, "oom_kill ", oom_kill_len) == 0)
			len = oom_kill_len;
		else if (read >= oom_len + 2 && memcmp(line, "oom ", oom_len) == 0)
			len = oom_len;
		else
			continue;

		counter = strtol(&line[len], NULL, 10);

		if (counter == LONG_MAX) {
			nwarnf("Failed to parse: %s", &line[len]);
			continue;
		}

		if (counter == 0)
			continue;

		if (counter != last_counter) {
			if (write_oom_files() == 0)
				last_counter = counter;
		}
		return G_SOURCE_CONTINUE;
	}
	return G_SOURCE_REMOVE;
}

/* write the appropriate files to tell the caller there was an oom event
 * this can be used for v1 and v2 OOMS
 * returns 0 on success, negative value on failure
 */
static int write_oom_files()
{
	ninfo("OOM received");
	if (opt_persist_path) {
		_cleanup_free_ char *ctr_oom_file_path = g_build_filename(opt_persist_path, "oom", NULL);
		_cleanup_close_ int ctr_oom_fd = open(ctr_oom_file_path, O_CREAT, 0666);
		if (ctr_oom_fd < 0) {
			nwarn("Failed to write oom file");
		}
	}
	_cleanup_close_ int oom_fd = open("oom", O_CREAT, 0666);
	if (oom_fd < 0) {
		nwarn("Failed to write oom file");
	}
	return oom_fd >= 0 ? 0 : -1;
}

#endif
