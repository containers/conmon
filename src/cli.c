#include "cli.h"
#include "globals.h"
#include "ctr_logging.h"
#include "config.h"
#include "utils.h"

#include <glib.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/limits.h>

gboolean opt_version = FALSE;
gboolean opt_terminal = FALSE;
gboolean opt_stdin = FALSE;
gboolean opt_leave_stdin_open = FALSE;
gboolean opt_syslog = FALSE;
gboolean is_cgroup_v2 = FALSE;
char *cgroup2_path = NULL;
char *opt_cid = NULL;
char *opt_cuuid = NULL;
char *opt_name = NULL;
char *opt_runtime_path = NULL;
char *opt_bundle_path = NULL;
char *opt_persist_path = NULL;
char *opt_container_pid_file = NULL;
char *opt_conmon_pid_file = NULL;
gboolean opt_systemd_cgroup = FALSE;
gboolean opt_no_pivot = FALSE;
gboolean opt_attach = FALSE;
char *opt_exec_process_spec = NULL;
gboolean opt_exec = FALSE;
int opt_api_version = 0;
char *opt_restore_path = NULL;
gchar **opt_runtime_opts = NULL;
gchar **opt_runtime_args = NULL;
gchar **opt_log_path = NULL;
char *opt_exit_dir = NULL;
int opt_timeout = 0;
int64_t opt_log_size_max = -1;
char *opt_socket_path = DEFAULT_SOCKET_PATH;
gboolean opt_no_new_keyring = FALSE;
char *opt_exit_command = NULL;
gchar **opt_exit_args = NULL;
gboolean opt_replace_listen_pid = FALSE;
char *opt_log_level = NULL;
char *opt_log_tag = NULL;
GOptionEntry opt_entries[] = {
	{"terminal", 't', 0, G_OPTION_ARG_NONE, &opt_terminal, "Terminal", NULL},
	{"stdin", 'i', 0, G_OPTION_ARG_NONE, &opt_stdin, "Stdin", NULL},
	{"leave-stdin-open", 0, 0, G_OPTION_ARG_NONE, &opt_leave_stdin_open, "Leave stdin open when attached client disconnects", NULL},
	{"cid", 'c', 0, G_OPTION_ARG_STRING, &opt_cid, "Container ID", NULL},
	{"cuuid", 'u', 0, G_OPTION_ARG_STRING, &opt_cuuid, "Container UUID", NULL},
	{"name", 'n', 0, G_OPTION_ARG_STRING, &opt_name, "Container name", NULL},
	{"runtime", 'r', 0, G_OPTION_ARG_STRING, &opt_runtime_path, "Runtime path", NULL},
	{"restore", 0, 0, G_OPTION_ARG_STRING, &opt_restore_path, "Restore a container from a checkpoint", NULL},
	{"restore-arg", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_runtime_opts,
	 "Additional arg to pass to the restore command. Can be specified multiple times. (DEPRECATED)", NULL},
	{"runtime-opt", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_runtime_opts,
	 "Additional opts to pass to the restore or exec command. Can be specified multiple times", NULL},
	{"runtime-arg", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_runtime_args,
	 "Additional arg to pass to the runtime. Can be specified multiple times", NULL},
	{"exec-attach", 0, 0, G_OPTION_ARG_NONE, &opt_attach, "Attach to an exec session", NULL},
	{"no-new-keyring", 0, 0, G_OPTION_ARG_NONE, &opt_no_new_keyring, "Do not create a new session keyring for the container", NULL},
	{"no-pivot", 0, 0, G_OPTION_ARG_NONE, &opt_no_pivot, "Do not use pivot_root", NULL},
	{"replace-listen-pid", 0, 0, G_OPTION_ARG_NONE, &opt_replace_listen_pid, "Replace listen pid if set for oci-runtime pid", NULL},
	{"bundle", 'b', 0, G_OPTION_ARG_STRING, &opt_bundle_path, "Bundle path", NULL},
	{"persist-dir", '0', 0, G_OPTION_ARG_STRING, &opt_persist_path,
	 "Persistent directory for a container that can be used for storing container data", NULL},
	{"pidfile", 0, 0, G_OPTION_ARG_STRING, &opt_container_pid_file, "PID file (DEPRECATED)", NULL},
	{"container-pidfile", 'p', 0, G_OPTION_ARG_STRING, &opt_container_pid_file, "Container PID file", NULL},
	{"conmon-pidfile", 'P', 0, G_OPTION_ARG_STRING, &opt_conmon_pid_file, "Conmon daemon PID file", NULL},
	{"systemd-cgroup", 's', 0, G_OPTION_ARG_NONE, &opt_systemd_cgroup, "Enable systemd cgroup manager", NULL},
	{"exec", 'e', 0, G_OPTION_ARG_NONE, &opt_exec, "Exec a command in a running container", NULL},
	{"api-version", 0, 0, G_OPTION_ARG_NONE, &opt_api_version, "Conmon API version to use", NULL},
	{"exec-process-spec", 0, 0, G_OPTION_ARG_STRING, &opt_exec_process_spec, "Path to the process spec for exec", NULL},
	{"exit-dir", 0, 0, G_OPTION_ARG_STRING, &opt_exit_dir, "Path to the directory where exit files are written", NULL},
	{"exit-command", 0, 0, G_OPTION_ARG_STRING, &opt_exit_command,
	 "Path to the program to execute when the container terminates its execution", NULL},
	{"exit-command-arg", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_exit_args,
	 "Additional arg to pass to the exit command.  Can be specified multiple times", NULL},
	{"log-path", 'l', 0, G_OPTION_ARG_STRING_ARRAY, &opt_log_path, "Log file path", NULL},
	{"timeout", 'T', 0, G_OPTION_ARG_INT, &opt_timeout, "Timeout in seconds", NULL},
	{"log-size-max", 0, 0, G_OPTION_ARG_INT64, &opt_log_size_max, "Maximum size of log file", NULL},
	{"socket-dir-path", 0, 0, G_OPTION_ARG_STRING, &opt_socket_path, "Location of container attach sockets", NULL},
	{"version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print the version and exit", NULL},
	{"syslog", 0, 0, G_OPTION_ARG_NONE, &opt_syslog, "Log to syslog (use with cgroupfs cgroup manager)", NULL},
	{"log-level", 0, 0, G_OPTION_ARG_STRING, &opt_log_level, "Print debug logs based on log level", NULL},
	{"log-tag", 0, 0, G_OPTION_ARG_STRING, &opt_log_tag, "Additional tag to use for logging", NULL},
	{NULL, 0, 0, 0, NULL, NULL, NULL}};


int initialize_cli(int argc, char *argv[])
{
	GOptionContext *context = g_option_context_new("- conmon utility");
	g_option_context_add_main_entries(context, opt_entries, "conmon");

	GError *error = NULL;
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_print("conmon: option parsing failed: %s\n", error->message);
		exit(EXIT_FAILURE);
	}
	if (opt_version) {
		g_print("conmon version " VERSION "\ncommit: " GIT_COMMIT "\n");
		exit(EXIT_SUCCESS);
	}

	if (opt_cid == NULL) {
		fprintf(stderr, "conmon: Container ID not provided. Use --cid\n");
		exit(EXIT_FAILURE);
	}
	return -1;
}

void process_cli()
{
	/* Command line parameters */
	set_conmon_logs(opt_log_level, opt_cid, opt_syslog, opt_log_tag);


	main_loop = g_main_loop_new(NULL, FALSE);

	if (opt_restore_path && opt_exec)
		nexit("Cannot use 'exec' and 'restore' at the same time");

	if (!opt_exec && opt_attach)
		nexit("Attach can only be specified with exec");

	if (opt_api_version < 1 && opt_attach)
		nexit("Attach can only be specified for a non-legacy exec session");

	/* The old exec API did not require opt_cuuid */
	if (opt_cuuid == NULL && (!opt_exec || opt_api_version >= 1))
		nexit("Container UUID not provided. Use --cuuid");

	if (opt_runtime_path == NULL)
		nexit("Runtime path not provided. Use --runtime");
	if (access(opt_runtime_path, X_OK) < 0)
		pexitf("Runtime path %s is not valid", opt_runtime_path);

	if (opt_exec && opt_exec_process_spec == NULL) {
		nexit("Exec process spec path not provided. Use --exec-process-spec");
	}

	char cwd[PATH_MAX];
	if (getcwd(cwd, sizeof(cwd)) == NULL) {
		nexit("Failed to get working directory");
	}

	// opt_bundle_path in exec means we will set up the attach socket
	// for the exec session. the legacy version of exec does not need this
	// and thus we only override an empty opt_bundle_path when we're not exec
	if (opt_bundle_path == NULL && !opt_exec) {
		opt_bundle_path = cwd;
	}

	// we should always override the container pid file if it's empty
	// TODO FIXME I removed default_pid_file here. shouldn't opt_container_pid_file be cleaned up?
	if (opt_container_pid_file == NULL)
		opt_container_pid_file = g_strdup_printf("%s/pidfile-%s", cwd, opt_cid);

	configure_log_drivers(opt_log_path, opt_log_size_max, opt_cid, opt_name, opt_log_tag);
}
