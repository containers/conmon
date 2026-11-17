#if !defined(PARENT_PIPE_FD_H)
#define PARENT_PIPE_FD_H


void write_or_close_sync_fd(int *fd, int res, const char *message);
int get_pipe_fd_from_env(const char *envname);
extern int sync_pipe_fd;

/*
 * Register an atexit handler that sends a fallback error message on the
 * sync pipe if the normal code path did not write to it. Must be called
 * after sync_pipe_fd is set up.
 */
void setup_sync_pipe_atexit(void);

/*
 * Send a fallback error on the sync pipe if nothing was written yet.
 * Safe to call multiple times. Called explicitly from do_exit_command()
 * before it closes all file descriptors.
 */
void report_sync_pipe_if_needed(void);


#endif // PARENT_PIPE_FD_H
