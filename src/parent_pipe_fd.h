#if !defined(PARENT_PIPE_FD_H)
#define PARENT_PIPE_FD_H


void write_sync_fd(int fd, int res, const char *message);
int get_pipe_fd_from_env(const char *envname);
extern int sync_pipe_fd;


#endif // PARENT_PIPE_FD_H
