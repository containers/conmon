package main

import (
	"fmt"
	"io/ioutil"

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

#endif // CONFIG_H
`
	if err := ioutil.WriteFile("config.h", []byte(fmt.Sprintf(output, config.BufSize, config.BufSize, config.ConnSockBufSize, config.ContainerAttachSocketDir)), 0644); err != nil {
		fmt.Errorf(err.Error())
	}
}
