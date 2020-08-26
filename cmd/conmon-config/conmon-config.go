package main

import (
	"fmt"
	"io/ioutil"
	"log"

	"github.com/containers/conmon/runner/config"
)

func main() {
	output := `
#if !defined(CONFIG_H)
#define CONFIG_H

#define BUF_SIZE %d
#define STDIO_BUF_SIZE %d
#define CONN_SOCK_BUF_SIZE %d
#define DEFAULT_SOCKET_PATH "%s"
#define WIN_RESIZE_EVENT %d
#define REOPEN_LOGS_EVENT %d
#define TIMED_OUT_MESSAGE "%s"

#endif // CONFIG_H
`
	if err := ioutil.WriteFile("config.h", []byte(fmt.Sprintf(
		output,
		config.BufSize,
		config.BufSize,
		config.ConnSockBufSize,
		config.ContainerAttachSocketDir,
		config.WinResizeEvent,
		config.ReopenLogsEvent,
		config.TimedOutMessage)),
		0644); err != nil {
		log.Fatal(err)
	}
}
