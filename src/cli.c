#include "cli.h"
#include "globals.h"
#include "ctr_logging.h"
#include "config.h"
#include "utils.h"

#include <glib.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __linux__
#include <linux/limits.h>
#endif

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
int64_t opt_log_global_size_max = -1;
char *opt_socket_path = DEFAULT_SOCKET_PATH;
gboolean opt_no_new_keyring = FALSE;
char *opt_exit_command = NULL;
gchar **opt_exit_args = NULL;
int opt_exit_delay = 0;
gboolean opt_replace_listen_pid = FALSE;
char *opt_log_level = NULL;
char *opt_log_tag = NULL;
gboolean opt_sync = FALSE;
gboolean opt_no_sync_log = FALSE;
char *opt_sdnotify_socket = NULL;
gboolean opt_full_attach_path = FALSE;
char *opt_seccomp_notify_socket = NULL;
char *opt_seccomp_notify_plugins = NULL;
GOptionEntry opt_entries[] = {
	{"api-version", 0, 0, G_OPTION_ARG_NONE, &opt_api_version, "Conmon API version to use", NULL},
	{"bundle", 'b', 0, G_OPTION_ARG_STRING, &opt_bundle_path, "Location of the OCI Bundle path", NULL},
	{"cid", 'c', 0, G_OPTION_ARG_STRING, &opt_cid, "Identification of Container", NULL},
	{"conmon-pidfile", 'P', 0, G_OPTION_ARG_STRING, &opt_conmon_pid_file, "PID file for the conmon process", NULL},
	{"container-pidfile", 'p', 0, G_OPTION_ARG_STRING, &opt_container_pid_file, "PID file for the initial pid inside of container",
	 NULL},
	{"cuuid", 'u', 0, G_OPTION_ARG_STRING, &opt_cuuid, "Container UUID", NULL},
	{"exec", 'e', 0, G_OPTION_ARG_NONE, &opt_exec, "Exec a command into a running container", NULL},
	{"exec-attach", 0, 0, G_OPTION_ARG_NONE, &opt_attach, "Attach to an exec session", NULL},
	{"exec-process-spec", 0, 0, G_OPTION_ARG_STRING, &opt_exec_process_spec, "Path to the process spec for execution", NULL},
	{"exit-command", 0, 0, G_OPTION_ARG_STRING, &opt_exit_command,
	 "Path to the program to execute when the container terminates its execution", NULL},
	{"exit-command-arg", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_exit_args,
	 "Additional arg to pass to the exit command.  Can be specified multiple times", NULL},
	{"exit-delay", 0, 0, G_OPTION_ARG_INT, &opt_exit_delay, "Delay before invoking the exit command (in seconds)", NULL},
	{"exit-dir", 0, 0, G_OPTION_ARG_STRING, &opt_exit_dir, "Path to the directory where exit files are written", NULL},
	{"leave-stdin-open", 0, 0, G_OPTION_ARG_NONE, &opt_leave_stdin_open, "Leave stdin open when attached client disconnects", NULL},
	{"log-level", 0, 0, G_OPTION_ARG_STRING, &opt_log_level, "Print debug logs based on log level", NULL},
	{"log-path", 'l', 0, G_OPTION_ARG_STRING_ARRAY, &opt_log_path, "Log file path", NULL},
	{"log-size-max", 0, 0, G_OPTION_ARG_INT64, &opt_log_size_max, "Maximum size of log file", NULL},
	{"log-global-size-max", 0, 0, G_OPTION_ARG_INT64, &opt_log_global_size_max, "Maximum size of all log files", NULL},
	{"log-tag", 0, 0, G_OPTION_ARG_STRING, &opt_log_tag, "Additional tag to use for logging", NULL},
	{"name", 'n', 0, G_OPTION_ARG_STRING, &opt_name, "Container name", NULL},
	{"no-new-keyring", 0, 0, G_OPTION_ARG_NONE, &opt_no_new_keyring, "Do not create a new session keyring for the container", NULL},
	{"no-pivot", 0, 0, G_OPTION_ARG_NONE, &opt_no_pivot, "Do not use pivot_root", NULL},
	{"no-sync-log", 0, 0, G_OPTION_ARG_NONE, &opt_no_sync_log, "Do not manually call sync on logs after container shutdown", NULL},
	{"persist-dir", '0', 0, G_OPTION_ARG_STRING, &opt_persist_path,
	 "Persistent directory for a container that can be used for storing container data", NULL},
	{"pidfile", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &opt_container_pid_file, "PID file (DEPRECATED)", NULL},
	{"replace-listen-pid", 0, 0, G_OPTION_ARG_NONE, &opt_replace_listen_pid, "Replace listen pid if set for oci-runtime pid", NULL},
	{"restore", 0, 0, G_OPTION_ARG_STRING, &opt_restore_path, "Restore a container from a checkpoint", NULL},
	{"restore-arg", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING_ARRAY, &opt_runtime_opts,
	 "Additional arg to pass to the restore command. Can be specified multiple times. (DEPRECATED)", NULL},
	{"runtime", 'r', 0, G_OPTION_ARG_STRING, &opt_runtime_path, "Path to store runtime data for the container", NULL},
	{"runtime-arg", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_runtime_args,
	 "Additional arg to pass to the runtime. Can be specified multiple times", NULL},
	{"runtime-opt", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_runtime_opts,
	 "Additional opts to pass to the restore or exec command. Can be specified multiple times", NULL},
	{"sdnotify-socket", 0, 0, G_OPTION_ARG_STRING, &opt_sdnotify_socket, "Path to the host's sd-notify socket to relay messages to",
	 NULL},
	{"socket-dir-path", 0, 0, G_OPTION_ARG_STRING, &opt_socket_path, "Location of container attach sockets", NULL},
	{"stdin", 'i', 0, G_OPTION_ARG_NONE, &opt_stdin, "Open up a pipe to pass stdin to the container", NULL},
	{"sync", 0, 0, G_OPTION_ARG_NONE, &opt_sync, "Keep the main conmon process as its child by only forking once", NULL},
	{"syslog", 0, 0, G_OPTION_ARG_NONE, &opt_syslog, "Log to syslog (use with cgroupfs cgroup manager)", NULL},
	{"systemd-cgroup", 's', 0, G_OPTION_ARG_NONE, &opt_systemd_cgroup,
	 "Enable systemd cgroup manager, rather then use the cgroupfs directly", NULL},
	{"terminal", 't', 0, G_OPTION_ARG_NONE, &opt_terminal, "Allocate a pseudo-TTY. The default is false", NULL},
	{"timeout", 'T', 0, G_OPTION_ARG_INT, &opt_timeout, "Kill container after specified timeout in seconds.", NULL},
	{"version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print the version and exit", NULL},
	{"full-attach", 0, 0, G_OPTION_ARG_NONE, &opt_full_attach_path,
	 "Don't truncate the path to the attach socket. This option causes conmon to ignore --socket-dir-path", NULL},
	{"seccomp-notify-socket", 0, 0, G_OPTION_ARG_STRING, &opt_seccomp_notify_socket,
	 "Path to the socket where the seccomp notification fd is received", NULL},
	{"seccomp-notify-plugins", 0, 0, G_OPTION_ARG_STRING, &opt_seccomp_notify_plugins,
	 "Plugins to use for managing the seccomp notifications", NULL},
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

	g_option_context_free(context);
	context = NULL;

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

	if (opt_seccomp_notify_plugins == NULL)
		opt_seccomp_notify_plugins = getenv("CONMON_SECCOMP_NOTIFY_PLUGINS");

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

	if (opt_exit_delay < 0) {
		nexit("Delay before invoking exit command must be greater than or equal to 0");
	}

	// we should always override the container pid file if it's empty
	// TODO FIXME I removed default_pid_file here. shouldn't opt_container_pid_file be cleaned up?
	if (opt_container_pid_file == NULL)
		opt_container_pid_file = g_strdup_printf("%s/pidfile-%s", cwd, opt_cid);

	configure_log_drivers(opt_log_path, opt_log_size_max, opt_log_global_size_max, opt_cid, opt_name, opt_log_tag);
}
