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
#include "runtime_args.h"

#include <sys/prctl.h>
#include <sys/stat.h>

int main(int argc, char *argv[])
{
	_cleanup_gerror_ GError *err = NULL;
	char buf[BUF_SIZE];
	int num_read;
	_cleanup_close_ int dev_null_r = -1;
	_cleanup_close_ int dev_null_w = -1;
	_cleanup_close_ int dummyfd = -1;

	int initialize_ec = initialize_cli(argc, argv);
	if (initialize_ec >= 0) {
		exit(initialize_ec);
	}

	process_cli();

	attempt_oom_adjust();

	/* ignoring SIGPIPE prevents conmon from being spuriously killed */
	signal(SIGPIPE, SIG_IGN);

	int start_pipe_fd = get_pipe_fd_from_env("_OCI_STARTPIPE");
	if (start_pipe_fd > 0) {
		/* Block for an initial write to the start pipe before
		   spawning any childred or exiting, to ensure the
		   parent can put us in the right cgroup. */
		num_read = read(start_pipe_fd, buf, BUF_SIZE);
		if (num_read < 0) {
			pexit("start-pipe read failed");
		}
		/* If we aren't attaching in an exec session,
		   we don't need this anymore. */
		if (!opt_attach)
			close(start_pipe_fd);
	}

	dev_null_r = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (dev_null_r < 0)
		pexit("Failed to open /dev/null");

	dev_null_w = open("/dev/null", O_WRONLY | O_CLOEXEC);
	if (dev_null_w < 0)
		pexit("Failed to open /dev/null");

	/* In the non-sync case, we double-fork in
	 * order to disconnect from the parent, as we want to
	 * continue in a daemon-like way */
	if (!opt_sync) {
		pid_t main_pid = fork();
		if (main_pid < 0) {
			pexit("Failed to fork the create command");
		} else if (main_pid != 0) {
			if (opt_conmon_pid_file) {
				char content[12];
				sprintf(content, "%i", main_pid);

				if (!g_file_set_contents(opt_conmon_pid_file, content, strlen(content), &err)) {
					_pexitf("Failed to write conmon pidfile: %s", err->message);
				}
			}
			_exit(0);
		}
	}

	/* before we fork, ensure our children will be reaped */
	atexit(reap_children);

	/* Environment variables */
	sync_pipe_fd = get_pipe_fd_from_env("_OCI_SYNCPIPE");

	int attach_pipe_fd = -1;
	if (opt_attach) {
		attach_pipe_fd = get_pipe_fd_from_env("_OCI_ATTACHPIPE");
		if (attach_pipe_fd < 0) {
			pexit("--attach specified but _OCI_ATTACHPIPE was not");
		}
	}


	/* Disconnect stdio from parent. We need to do this, because
	   the parent is waiting for the stdout to end when the intermediate
	   child dies */
	if (dup2(dev_null_r, STDIN_FILENO) < 0)
		pexit("Failed to dup over stdin");
	if (dup2(dev_null_w, STDOUT_FILENO) < 0)
		pexit("Failed to dup over stdout");
	if (dup2(dev_null_w, STDERR_FILENO) < 0)
		pexit("Failed to dup over stderr");

	/* Create a new session group */
	setsid();

	/*
	 * Set self as subreaper so we can wait for container process
	 * and return its exit code.
	 */
	int ret = prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
	if (ret != 0) {
		pexit("Failed to set as subreaper");
	}

	_cleanup_free_ char *csname = NULL;
	int workerfd_stdin = -1;
	int workerfd_stdout = -1;
	int workerfd_stderr = -1;
	int fds[2];
	if (opt_terminal) {
		csname = setup_console_socket();
	} else {

		/*
		 * Create a "fake" main fd so that we can use the same epoll code in
		 * both cases. The workerfd_*s will be closed after we dup over
		 * everything.
		 *
		 * We use pipes here because open(/dev/std{out,err}) will fail if we
		 * used anything else (and it wouldn't be a good idea to create a new
		 * pty pair in the host).
		 */

		if (opt_stdin) {
			if (pipe2(fds, O_CLOEXEC) < 0)
				pexit("Failed to create !terminal stdin pipe");

			mainfd_stdin = fds[1];
			workerfd_stdin = fds[0];

			if (g_unix_set_fd_nonblocking(mainfd_stdin, TRUE, NULL) == FALSE)
				nwarn("Failed to set mainfd_stdin to non blocking");
		}

		if (pipe2(fds, O_CLOEXEC) < 0)
			pexit("Failed to create !terminal stdout pipe");

		mainfd_stdout = fds[0];
		workerfd_stdout = fds[1];

		/* now that we've set mainfd_stdout, we can register the ctrl_winsz_cb
		 * if we didn't set it here, we'd risk attempting to run ioctl on
		 * a negative fd, and fail to resize the window */
		g_unix_fd_add(winsz_fd_r, G_IO_IN, ctrl_winsz_cb, NULL);
	}

	/* We always create a stderr pipe, because that way we can capture
	   runc stderr messages before the tty is created */
	if (pipe2(fds, O_CLOEXEC) < 0)
		pexit("Failed to create stderr pipe");

	mainfd_stderr = fds[0];
	workerfd_stderr = fds[1];

	GPtrArray *runtime_argv = configure_runtime_args(csname);

	/* Setup endpoint for attach */
	_cleanup_free_ char *attach_symlink_dir_path = NULL;
	if (opt_bundle_path != NULL) {
		attach_symlink_dir_path = setup_attach_socket();
		dummyfd = setup_terminal_control_fifo();
		setup_console_fifo();

		if (opt_attach) {
			ndebug("sending attach message to parent");
			write_sync_fd(attach_pipe_fd, 0, NULL);
			ndebug("sent attach message to parent");
		}
	}

	sigset_t mask, oldmask;
	if ((sigemptyset(&mask) < 0) || (sigaddset(&mask, SIGTERM) < 0) || (sigaddset(&mask, SIGQUIT) < 0) || (sigaddset(&mask, SIGINT) < 0)
	    || sigprocmask(SIG_BLOCK, &mask, &oldmask) < 0)
		pexit("Failed to block signals");
	/*
	 * We have to fork here because the current runC API dups the stdio of the
	 * calling process over the container's fds. This is actually *very bad*
	 * but is currently being discussed for change in
	 * https://github.com/opencontainers/runtime-spec/pull/513. Hopefully this
	 * won't be the case for very long.
	 */

	/* Create our container. */
	create_pid = fork();
	if (create_pid < 0) {
		pexit("Failed to fork the create command");
	} else if (!create_pid) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0)
			_pexit("Failed to set PDEATHSIG");
		if (sigprocmask(SIG_SETMASK, &oldmask, NULL) < 0)
			_pexit("Failed to unblock signals");

		if (workerfd_stdin < 0)
			workerfd_stdin = dev_null_r;
		if (dup2(workerfd_stdin, STDIN_FILENO) < 0)
			_pexit("Failed to dup over stdin");
		if (fchmod(STDIN_FILENO, 0777) < 0)
			nwarn("Failed to chown stdin");

		if (workerfd_stdout < 0)
			workerfd_stdout = dev_null_w;
		if (dup2(workerfd_stdout, STDOUT_FILENO) < 0)
			_pexit("Failed to dup over stdout");
		if (fchmod(STDOUT_FILENO, 0777) < 0)
			nwarn("Failed to chown stdout");

		if (workerfd_stderr < 0)
			workerfd_stderr = workerfd_stdout;
		if (dup2(workerfd_stderr, STDERR_FILENO) < 0)
			_pexit("Failed to dup over stderr");
		if (fchmod(STDERR_FILENO, 0777) < 0)
			nwarn("Failed to chown stderr");

		/* If LISTEN_PID env is set, we need to set the LISTEN_PID
		   it to the new child process */
		char *listenpid = getenv("LISTEN_PID");
		if (listenpid != NULL) {
			errno = 0;
			int lpid = strtol(listenpid, NULL, 10);
			if (errno != 0 || lpid <= 0)
				_pexitf("Invalid LISTEN_PID %.10s", listenpid);
			if (opt_replace_listen_pid || lpid == getppid()) {
				gchar *pidstr = g_strdup_printf("%d", getpid());
				if (!pidstr)
					_pexit("Failed to g_strdup_sprintf pid");
				if (setenv("LISTEN_PID", pidstr, true) < 0)
					_pexit("Failed to setenv LISTEN_PID");
				free(pidstr);
			}
		}

		// If we are execing, and the user is trying to attach to this exec session,
		// we need to wait until they attach to the console before actually execing,
		// or else we may lose output
		if (opt_attach) {
			if (start_pipe_fd > 0) {
				ndebug("exec with attach is waiting for start message from parent");
				num_read = read(start_pipe_fd, buf, BUF_SIZE);
				ndebug("exec with attach got start message from parent");
				if (num_read < 0) {
					_pexit("start-pipe read failed");
				}
				close(start_pipe_fd);
			}
		}

		execv(g_ptr_array_index(runtime_argv, 0), (char **)runtime_argv->pdata);
		exit(127);
	}

	if ((signal(SIGTERM, on_sig_exit) == SIG_ERR) || (signal(SIGQUIT, on_sig_exit) == SIG_ERR)
	    || (signal(SIGINT, on_sig_exit) == SIG_ERR))
		pexit("Failed to register the signal handler");


	if (sigprocmask(SIG_SETMASK, &oldmask, NULL) < 0)
		pexit("Failed to unblock signals");

	/* Map pid to its handler.  */
	GHashTable *pid_to_handler = g_hash_table_new(g_int_hash, g_int_equal);
	g_hash_table_insert(pid_to_handler, (pid_t *)&create_pid, runtime_exit_cb);

	/*
	 * Glib does not support SIGCHLD so use SIGUSR1 with the same semantic.  We will
	 * catch SIGCHLD and raise(SIGUSR1) in the signal handler.
	 */
	struct pid_check_data data = {
		.pid_to_handler = pid_to_handler,
		.exit_status_cache = NULL,
	};
	g_unix_signal_add(SIGUSR1, on_sigusr1_cb, &data);

	if (signal(SIGCHLD, on_sigchld) == SIG_ERR)
		pexit("Failed to set handler for SIGCHLD");

	if (opt_exit_command)
		atexit(do_exit_command);

	g_ptr_array_free(runtime_argv, TRUE);

	/* The runtime has that fd now. We don't need to touch it anymore. */
	if (workerfd_stdin > -1)
		close(workerfd_stdin);
	if (workerfd_stdout > -1)
		close(workerfd_stdout);
	if (workerfd_stderr > -1)
		close(workerfd_stderr);

	if (csname != NULL) {
		g_unix_fd_add(console_socket_fd, G_IO_IN, terminal_accept_cb, csname);
		/* Process any SIGCHLD we may have missed before the signal handler was in place.  */
		if (!opt_exec || !opt_terminal || container_status < 0) {
			GHashTable *exit_status_cache = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, g_free);
			data.exit_status_cache = exit_status_cache;
			g_idle_add(check_child_processes_cb, &data);
			g_main_loop_run(main_loop);
		}
	} else {
		int ret;
		/* Wait for our create child to exit with the return code. */
		do
			ret = waitpid(create_pid, &runtime_status, 0);
		while (ret < 0 && errno == EINTR);
		if (ret < 0) {
			if (create_pid > 0) {
				int old_errno = errno;
				kill(create_pid, SIGKILL);
				errno = old_errno;
			}
			pexitf("Failed to wait for `runtime %s`", opt_exec ? "exec" : "create");
		}
	}

	if (!WIFEXITED(runtime_status) || WEXITSTATUS(runtime_status) != 0) {
		if (sync_pipe_fd > 0) {
			/*
			 * Read from container stderr for any error and send it to parent
			 * We send -1 as pid to signal to parent that create container has failed.
			 */
			num_read = read(mainfd_stderr, buf, BUF_SIZE - 1);
			if (num_read > 0) {
				buf[num_read] = '\0';
				int to_report = -1;
				if (opt_exec && container_status > 0) {
					to_report = -1 * container_status;
				}

				write_sync_fd(sync_pipe_fd, to_report, buf);
			}
		}
		nexitf("Failed to create container: exit status %d", get_exit_status(runtime_status));
	}

	if (opt_terminal && mainfd_stdout == -1)
		nexit("Runtime did not set up terminal");

	/* Read the pid so we can wait for the process to exit */
	_cleanup_free_ char *contents = NULL;
	if (!g_file_get_contents(opt_container_pid_file, &contents, NULL, &err)) {
		nwarnf("Failed to read pidfile: %s", err->message);
		exit(1);
	}

	container_pid = atoi(contents);
	ndebugf("container PID: %d", container_pid);

	g_hash_table_insert(pid_to_handler, (pid_t *)&container_pid, container_exit_cb);

	/* Send the container pid back to parent
	 * Only send this pid back if we are using the current exec API. Old consumers expect
	 * conmon to only send one value down this pipe, which will later be the exit code
	 * Thus, if we are legacy and we are exec, skip this write.
	 */
	if ((opt_api_version >= 1 || !opt_exec) && sync_pipe_fd >= 0)
		write_sync_fd(sync_pipe_fd, container_pid, NULL);

	setup_oom_handling(container_pid);

	if (mainfd_stdout >= 0) {
		g_unix_fd_add(mainfd_stdout, G_IO_IN, stdio_cb, GINT_TO_POINTER(STDOUT_PIPE));
	}
	if (mainfd_stderr >= 0) {
		g_unix_fd_add(mainfd_stderr, G_IO_IN, stdio_cb, GINT_TO_POINTER(STDERR_PIPE));
	}

	if (opt_timeout > 0) {
		g_timeout_add_seconds(opt_timeout, timeout_cb, NULL);
	}

	if (data.exit_status_cache) {
		GHashTableIter iter;
		gpointer key, value;

		g_hash_table_iter_init(&iter, data.exit_status_cache);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			pid_t *k = (pid_t *)key;
			int *v = (int *)value;
			void (*cb)(GPid, int, gpointer) = g_hash_table_lookup(pid_to_handler, k);
			if (cb)
				cb(*k, *v, 0);
		}
		g_hash_table_destroy(data.exit_status_cache);
		data.exit_status_cache = NULL;
	}

	/* There are three cases we want to run this main loop:
	   1. If we are using the legacy API
	   2. if we are running create or restore
	   3. if we are running exec without a terminal
	       no matter the speed of the command being executed, having outstanding
	       output to process from the child process keeps it alive, so we can read the io,
	       and let the callback handler take care of the container_status as normal.
	   4. if we are exec with a tty open, and our container_status hasn't been changed
	      by any callbacks yet
	       specifically, the check child processes call above could set the container
	       status if it is a quickly exiting command. We only want to run the loop if
	       this hasn't happened yet.
		Note: there exists a chance that we have the container_status, are exec, and api>=1,
		but are not terminal. In this case, we still want to run to process all of the output,
		but will need to exit once all the i/o is read. This will be handled in stdio_cb above.
	*/
	if (opt_api_version < 1 || !opt_exec || !opt_terminal || container_status < 0) {
		g_idle_add(check_child_processes_cb, &data);
		g_main_loop_run(main_loop);
	}

	check_cgroup2_oom();

	/* Drain stdout and stderr only if a timeout doesn't occur */
	if (!timed_out)
		drain_stdio();

	if (!opt_no_sync_log)
		sync_logs();

	int exit_status = -1;
	const char *exit_message = NULL;

	/*
	 * If timed_out is TRUE but container_pid is -1, the process must have died before
	 * the timer elapsed. Ignore the timeout and treat it like a normal container exit.
	 */
	if (timed_out && container_pid > 0) {
		pid_t process_group = getpgid(container_pid);

		if (process_group > 0)
			kill(-process_group, SIGKILL);
		else
			kill(container_pid, SIGKILL);
		exit_message = TIMED_OUT_MESSAGE;
	} else {
		exit_status = get_exit_status(container_status);
	}

	/*
	 * Podman injects some fd's into the conmon process so that exposed ports are kept busy while
	 * the container runs.  Close them before we notify the container exited, so that they can be
	 * reused immediately.
	 */
	DIR *fdsdir = opendir("/proc/self/fd");
	if (fdsdir != NULL) {
		int fd;
		int dfd = dirfd(fdsdir);
		struct dirent *next;

		for (next = readdir(fdsdir); next; next = readdir(fdsdir)) {
			const char *name = next->d_name;
			if (name[0] == '.')
				continue;

			fd = strtoll(name, NULL, 10);
			if (fd == dfd || fd == sync_pipe_fd || fd == attach_pipe_fd || fd == dev_null_r || fd == dev_null_w)
				continue;
			close(fd);
		}
		closedir(fdsdir);
	}

	_cleanup_free_ char *status_str = g_strdup_printf("%d", exit_status);

	/* Write the exit file to container persistent directory if it is specified */
	if (opt_persist_path) {
		_cleanup_free_ char *ctr_exit_file_path = g_build_filename(opt_persist_path, "exit", NULL);
		if (!g_file_set_contents(ctr_exit_file_path, status_str, -1, &err))
			nexitf("Failed to write %s to container exit file: %s", status_str, err->message);
	}

	/*
	 * Writing to this directory helps if a daemon process wants to monitor all container exits
	 * using inotify.
	 */
	if (opt_exit_dir) {
		_cleanup_free_ char *exit_file_path = g_build_filename(opt_exit_dir, opt_cid, NULL);
		if (!g_file_set_contents(exit_file_path, status_str, -1, &err))
			nexitf("Failed to write %s to exit file: %s", status_str, err->message);
	}

	/* Send the command exec exit code back to the parent */
	if (opt_exec && sync_pipe_fd >= 0)
		write_sync_fd(sync_pipe_fd, exit_status, exit_message);

	if (attach_symlink_dir_path != NULL && unlink(attach_symlink_dir_path) == -1 && errno != ENOENT)
		pexit("Failed to remove symlink for attach socket directory");

	return exit_status;
}
