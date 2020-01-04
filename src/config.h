
#if !defined(CONFIG_H)
#define CONFIG_H

#define BUF_SIZE 8192
#define STDIO_BUF_SIZE 8192
#define CONN_SOCK_BUF_SIZE 32768
#define DEFAULT_SOCKET_PATH "/var/run/crio"
#define WIN_RESIZE_EVENT 1
#define REOPEN_LOGS_EVENT 2
#define TIMED_OUT_MESSAGE "command timed out"

#endif // CONFIG_H
