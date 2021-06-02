#if !defined(CLI_H)
#define CLI_H

#include <glib.h>   /* gboolean and GOptionEntry */
#include <stdint.h> /* int64_t */

extern gboolean opt_version;
extern gboolean opt_terminal;
extern gboolean opt_stdin;
extern gboolean opt_leave_stdin_open;
extern gboolean opt_syslog;
extern gboolean is_cgroup_v2;
extern char *cgroup2_path;
extern char *opt_cid;
extern char *opt_cuuid;
extern char *opt_name;
extern char *opt_runtime_path;
extern char *opt_bundle_path;
extern char *opt_persist_path;
extern char *opt_container_pid_file;
extern char *opt_conmon_pid_file;
extern gboolean opt_systemd_cgroup;
extern gboolean opt_no_pivot;
extern gboolean opt_attach;
extern char *opt_exec_process_spec;
extern gboolean opt_exec;
extern int opt_api_version;
extern char *opt_restore_path;
extern gchar **opt_runtime_opts;
extern gchar **opt_runtime_args;
extern gchar **opt_log_path;
extern char *opt_exit_dir;
extern int opt_timeout;
extern int64_t opt_log_size_max;
extern char *opt_socket_path;
extern gboolean opt_no_new_keyring;
extern char *opt_exit_command;
extern gchar **opt_exit_args;
extern int opt_exit_delay;
extern gboolean opt_replace_listen_pid;
extern char *opt_log_level;
extern char *opt_log_tag;
extern gboolean opt_no_sync_log;
extern gboolean opt_sync;
extern char *opt_sdnotify_socket;
extern char *opt_seccomp_notify_socket;
extern char *opt_seccomp_notify_plugins;
extern GOptionEntry opt_entries[];
extern gboolean opt_full_attach_path;

int initialize_cli(int argc, char *argv[]);
void process_cli();

#endif // CLI_H
