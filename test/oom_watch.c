/* Exercise the real reader, callback and marker helpers without a live cgroup. */
#include "../src/cgroup.c"

#include <signal.h>
#include <sys/stat.h>

gboolean is_cgroup_v2 = TRUE;
char *cgroup2_path;
char *opt_bundle_path;
char *opt_persist_path;

static unsigned int failures;
static guint watch_id;
static int event_pipe[2];

static void check(gboolean ok, const char *message)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", message);
		failures++;
	}
}

static gboolean watch_alive(void)
{
	return g_main_context_find_source_by_id(NULL, watch_id) != NULL;
}

static void snapshot(int max, int oom, int kill)
{
	FILE *fp = fopen("cgroup/memory.events", "w");
	g_assert_nonnull(fp);
	g_assert_cmpint(fprintf(fp, "low 0\nhigh 0\nmax %d\noom %d\noom_kill %d\n", max, oom, kill), >, 0);
	g_assert_cmpint(fclose(fp), ==, 0);
}

static void notify_and_check(const char *step, gboolean alive, gboolean marker)
{
	/* The callback discards notification bytes. A pipe makes delivery deterministic;
	 * this tests the GLib watch lifetime, not kernel inotify or cgroup counters. */
	if (watch_alive()) {
		struct inotify_event event = {.wd = 1, .mask = IN_MODIFY};
		g_assert_cmpint(write(event_pipe[1], &event, sizeof(event)), ==, sizeof(event));
		g_assert_true(g_main_context_iteration(NULL, FALSE));
	}
	gboolean source = watch_alive();
	gboolean fd_open = fcntl(event_pipe[0], F_GETFD) != -1;
	gboolean bundle = access("bundle/oom", F_OK) == 0;
	gboolean persist = access("persist/oom", F_OK) == 0;
	printf("%s: source=%d fd=%d bundle_marker=%d persist_marker=%d\n", step, source, fd_open, bundle, persist);
	check(source == alive, "GLib source lifetime");
	check(fd_open == alive, "notification fd lifetime");
	check((inotify_fd == event_pipe[0]) == alive, "global notification fd ownership");
	check(bundle == marker, "bundle OOM marker");
	check(persist == marker, "persist OOM marker");
	/* A retained source must not spin after the notification has been drained. */
	check(!g_main_context_iteration(NULL, FALSE), "no dispatch without another notification");
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s CASE EMPTY_WORK_DIRECTORY\n", argv[0]);
		return 2;
	}
	g_assert_cmpint(chdir(argv[2]), ==, 0);
	g_assert_cmpint(mkdir("cgroup", 0700), ==, 0);
	g_assert_cmpint(mkdir("bundle", 0700), ==, 0);
	g_assert_cmpint(mkdir("persist", 0700), ==, 0);
	cgroup2_path = g_strdup("cgroup");
	opt_bundle_path = g_strdup("bundle");
	opt_persist_path = g_strdup("persist");
	g_assert_cmpint(pipe(event_pipe), ==, 0);
	inotify_fd = event_pipe[0];
	watch_id = g_unix_fd_add(inotify_fd, G_IO_IN, oom_cb_cgroup_v2, NULL);
	g_assert_cmpuint(watch_id, !=, 0);
	alarm(5);

	if (strcmp(argv[1], "max-only") == 0) {
		snapshot(6, 0, 0);
		notify_and_check("max-only", TRUE, FALSE);
	} else if (strcmp(argv[1], "max-then-oom") == 0) {
		snapshot(6, 0, 0);
		notify_and_check("max-only", TRUE, FALSE);
		snapshot(18, 1, 3);
		notify_and_check("later-oom", TRUE, TRUE);
	} else if (strcmp(argv[1], "max-burst") == 0) {
		for (int count = 1; count <= 3; count++) {
			snapshot(count, 0, 0);
			notify_and_check("max-burst", TRUE, FALSE);
		}
	} else if (strcmp(argv[1], "oom-first") == 0) {
		snapshot(18, 1, 0);
		notify_and_check("oom-first", TRUE, TRUE);
	} else if (strcmp(argv[1], "kill-only") == 0) {
		snapshot(18, 0, 1);
		notify_and_check("kill-only", TRUE, TRUE);
	} else if (strcmp(argv[1], "unchanged-positive") == 0) {
		snapshot(18, 1, 3);
		notify_and_check("first-oom", TRUE, TRUE);
		snapshot(19, 1, 3);
		notify_and_check("unchanged-oom", TRUE, TRUE);
	} else if (strcmp(argv[1], "removed-file") == 0) {
		snapshot(18, 1, 3);
		notify_and_check("first-oom", TRUE, TRUE);
		g_assert_cmpint(unlink("cgroup/memory.events"), ==, 0);
		notify_and_check("removed-file", FALSE, TRUE);
	} else if (strcmp(argv[1], "marker-retry") == 0) {
		g_assert_cmpint(rmdir("bundle"), ==, 0);
		g_assert_cmpint(rmdir("persist"), ==, 0);
		snapshot(18, 1, 3);
		notify_and_check("marker-unavailable", TRUE, FALSE);
		g_assert_cmpint(mkdir("bundle", 0700), ==, 0);
		g_assert_cmpint(mkdir("persist", 0700), ==, 0);
		notify_and_check("marker-retry", TRUE, TRUE);
	} else {
		check(FALSE, "unknown case");
	}

	alarm(0);
	if (watch_alive())
		g_source_remove(watch_id);
	if (inotify_fd >= 0)
		close(inotify_fd);
	close(event_pipe[1]);
	unlink("cgroup/memory.events");
	unlink("bundle/oom");
	unlink("persist/oom");
	rmdir("cgroup");
	rmdir("bundle");
	rmdir("persist");
	g_free(cgroup2_path);
	g_free(opt_bundle_path);
	g_free(opt_persist_path);
	return failures ? 1 : 0;
}
