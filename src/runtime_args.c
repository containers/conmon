#include "runtime_args.h"
#include "cli.h"
#include "config.h"
#include "utils.h"

static void add_argv(GPtrArray *argv_array, ...) G_GNUC_NULL_TERMINATED;
static void add_argv(GPtrArray *argv_array, ...);
static void end_argv(GPtrArray *argv_array);

static void print_argv(GPtrArray *argv);
static void append_argv(gpointer data, gpointer user_data);

GPtrArray *configure_runtime_args(const char *const csname)
{
	GPtrArray *runtime_argv = g_ptr_array_new();
	add_argv(runtime_argv, opt_runtime_path, NULL);

	/* Generate the cmdline. */
	if (!opt_exec && opt_systemd_cgroup)
		add_argv(runtime_argv, "--systemd-cgroup", NULL);

	if (opt_runtime_args) {
		size_t n_runtime_args = 0;
		while (opt_runtime_args[n_runtime_args])
			add_argv(runtime_argv, opt_runtime_args[n_runtime_args++], NULL);
	}

	/* Set the exec arguments. */
	if (opt_exec) {
		add_argv(runtime_argv, "exec", "--pid-file", opt_container_pid_file, "--process", opt_exec_process_spec, "--detach", NULL);
		if (opt_terminal)
			add_argv(runtime_argv, "--tty", NULL);
	} else {
		char *command;
		if (opt_restore_path)
			command = "restore";
		else
			command = "create";

		add_argv(runtime_argv, command, "--bundle", opt_bundle_path, "--pid-file", opt_container_pid_file, NULL);
		if (opt_no_pivot)
			add_argv(runtime_argv, "--no-pivot", NULL);
		if (opt_no_new_keyring)
			add_argv(runtime_argv, "--no-new-keyring", NULL);

		if (opt_restore_path) {
			/*
			 * 'runc restore' is different from 'runc create'
			 * as the container is immediately running after
			 * a restore. Therefore the '--detach is needed'
			 * so that runc returns once the container is running.
			 *
			 * '--image-path' is the path to the checkpoint
			 * which will be become important when using pre-copy
			 * migration where multiple checkpoints can be created
			 * to reduce the container downtime during migration.
			 *
			 * '--work-path' is the directory CRIU will run in and
			 * also place its log files.
			 */
			add_argv(runtime_argv, "--detach", "--image-path", opt_restore_path, "--work-path", opt_bundle_path, NULL);
		}
	}
	/*
	 *  opt_runtime_opts can contain 'runc restore' or 'runc exec' options like
	 *  '--tcp-established' or '--preserve-fds'. Instead of listing each option as
	 *  a special conmon option, this (--runtime-opt) provides
	 *  a generic interface to pass all those options to conmon
	 *  without requiring a code change for each new option.
	 */
	if (opt_runtime_opts) {
		size_t n_runtime_opts = 0;
		while (opt_runtime_opts[n_runtime_opts])
			add_argv(runtime_argv, opt_runtime_opts[n_runtime_opts++], NULL);
	}


	if (csname != NULL) {
		add_argv(runtime_argv, "--console-socket", csname, NULL);
	}

	/* Container name comes last. */
	add_argv(runtime_argv, opt_cid, NULL);
	end_argv(runtime_argv);

	print_argv(runtime_argv);

	return runtime_argv;
}

static void print_argv(GPtrArray *runtime_argv)
{
	if (log_level != TRACE_LEVEL)
		return;
	GString *runtime_args_string = g_string_sized_new(BUF_SIZE);

	g_ptr_array_foreach(runtime_argv, append_argv, runtime_args_string);

	ntracef("calling runtime args: %s", runtime_args_string->str);
}

static void append_argv(gpointer data, gpointer user_data)
{
	if (!data)
		return;
	char *arg = (char *)data;
	GString *args = (GString *)user_data;

	g_string_append(args, arg);
	g_string_append_c(args, ' ');
}


static void add_argv(GPtrArray *argv_array, ...) G_GNUC_NULL_TERMINATED;

static void add_argv(GPtrArray *argv_array, ...)
{
	va_list args;
	char *arg;

	va_start(args, argv_array);
	while ((arg = va_arg(args, char *)))
		g_ptr_array_add(argv_array, arg);
	va_end(args);
}

static void end_argv(GPtrArray *argv_array)
{
	g_ptr_array_add(argv_array, NULL);
}
