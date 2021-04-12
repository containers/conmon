package conmon

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"regexp"
	"strings"
	"time"

	"github.com/containers/libpod/libpod/define"
	"github.com/pkg/errors"
)

func (ci *ConmonInstance) configurePipeEnv() error {
	if ci.cmd == nil {
		return errors.Errorf("conmon instance command must be configured")
	}
	if ci.started {
		return errors.Errorf("conmon instance environment cannot be configured after it's started")
	}
	// TODO handle PreserveFDs
	preserveFDs := 0
	fdCount := 3
	if ci.childSyncPipe != nil {
		ci.cmd.Env = append(ci.cmd.Env, fmt.Sprintf("_OCI_SYNCPIPE=%d", preserveFDs+fdCount))
		ci.cmd.ExtraFiles = append(ci.cmd.ExtraFiles, ci.childSyncPipe)
		fdCount++
	}
	if ci.childStartPipe != nil {
		ci.cmd.Env = append(ci.cmd.Env, fmt.Sprintf("_OCI_STARTPIPE=%d", preserveFDs+fdCount))
		ci.cmd.ExtraFiles = append(ci.cmd.ExtraFiles, ci.childStartPipe)
		fdCount++
	}
	if ci.childAttachPipe != nil {
		ci.cmd.Env = append(ci.cmd.Env, fmt.Sprintf("_OCI_ATTACHPIPE=%d", preserveFDs+fdCount))
		ci.cmd.ExtraFiles = append(ci.cmd.ExtraFiles, ci.childAttachPipe)
		fdCount++
	}
	return nil
}

func (ci *ConmonInstance) ContainerExitCode() (int, error) {
	return readConmonPipeData(ci.parentSyncPipe)
}

// readConmonPipeData attempts to read a syncInfo struct from the pipe
// TODO podman checks for ociLog capability
func readConmonPipeData(pipe *os.File) (int, error) {
	// syncInfo is used to return data from monitor process to daemon
	type syncInfo struct {
		Data    int    `json:"data"`
		Message string `json:"message,omitempty"`
	}

	// Wait to get container pid from conmon
	type syncStruct struct {
		si  *syncInfo
		err error
	}
	ch := make(chan syncStruct)
	go func() {
		var si *syncInfo
		rdr := bufio.NewReader(pipe)
		b, err := rdr.ReadBytes('\n')
		if err != nil {
			ch <- syncStruct{err: err}
		}
		if err := json.Unmarshal(b, &si); err != nil {
			ch <- syncStruct{err: err}
			return
		}
		ch <- syncStruct{si: si}
	}()

	data := -1
	select {
	case ss := <-ch:
		if ss.err != nil {
			return -1, errors.Wrapf(ss.err, "error received on processing data from conmon pipe")
		}
		if ss.si.Data < 0 {
			if ss.si.Message != "" {
				return ss.si.Data, getOCIRuntimeError(ss.si.Message)
			}
			return ss.si.Data, errors.Wrapf(define.ErrInternal, "conmon invocation failed")
		}
		data = ss.si.Data
	case <-time.After(1 * time.Minute):
		return -1, errors.Wrapf(define.ErrInternal, "conmon invocation timeout")
	}
	return data, nil
}

func getOCIRuntimeError(runtimeMsg string) error {
	// TODO base off of log level
	// includeFullOutput := logrus.GetLevel() == logrus.DebugLevel
	includeFullOutput := true

	if match := regexp.MustCompile("(?i).*permission denied.*|.*operation not permitted.*").FindString(runtimeMsg); match != "" {
		errStr := match
		if includeFullOutput {
			errStr = runtimeMsg
		}
		return errors.Wrapf(define.ErrOCIRuntimePermissionDenied, "%s", strings.Trim(errStr, "\n"))
	}
	if match := regexp.MustCompile("(?i).*executable file not found in.*|.*no such file or directory.*").FindString(runtimeMsg); match != "" {
		errStr := match
		if includeFullOutput {
			errStr = runtimeMsg
		}
		return errors.Wrapf(define.ErrOCIRuntimeNotFound, "%s", strings.Trim(errStr, "\n"))
	}
	return errors.Wrapf(define.ErrOCIRuntime, "%s", strings.Trim(runtimeMsg, "\n"))
}

// writeConmonPipeData writes data to a pipe. The actual content does not matter
// as it is used as a signal for conmon to stop blocking on a read
func writeConmonPipeData(pipe *os.File) error {
	someData := []byte{0}
	_, err := pipe.Write(someData)
	return err
}

func (ci *ConmonInstance) closePipesOnCleanup() {
	ci.parentSyncPipe.Close()
	ci.parentStartPipe.Close()
	ci.parentAttachPipe.Close()
}
