package conmon

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"regexp"
	"strings"
	"time"
)

// These errors are adapted from github.com/containers/podman:libpod/define
// And copied to reduce the vendor surface area of this library
var (
	// ErrInternal indicates an internal library error
	ErrInternal = fmt.Errorf("internal error")
	// ErrOCIRuntime indicates a generic error from the OCI runtime
	ErrOCIRuntime = fmt.Errorf("OCI runtime error")
	// ErrOCIRuntimePermissionDenied indicates the OCI runtime attempted to invoke a command that returned
	// a permission denied error
	ErrOCIRuntimePermissionDenied = fmt.Errorf("OCI permission denied")
	// ErrOCIRuntimeNotFound indicates the OCI runtime attempted to invoke a command
	// that was not found
	ErrOCIRuntimeNotFound = fmt.Errorf("OCI runtime attempted to invoke a command that was not found")
)

func (ci *ConmonInstance) configurePipeEnv() error {
	if ci.cmd == nil {
		return errors.New("conmon instance command must be configured")
	}
	if ci.started {
		return errors.New("conmon instance environment cannot be configured after it's started")
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
			return -1, fmt.Errorf("error received on processing data from conmon pipe: %w", ss.err)
		}
		if ss.si.Data < 0 {
			if ss.si.Message != "" {
				return ss.si.Data, getOCIRuntimeError(ss.si.Message)
			}
			return ss.si.Data, fmt.Errorf("conmon invocation failed: %w", ErrInternal)
		}
		data = ss.si.Data
	case <-time.After(1 * time.Minute):
		return -1, fmt.Errorf("conmon invocation timeout: %w", ErrInternal)
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
		return fmt.Errorf("%s: %w", strings.Trim(errStr, "\n"), ErrOCIRuntimePermissionDenied)
	}
	if match := regexp.MustCompile("(?i).*executable file not found in.*|.*no such file or directory.*").FindString(runtimeMsg); match != "" {
		errStr := match
		if includeFullOutput {
			errStr = runtimeMsg
		}
		return fmt.Errorf("%s: %w", strings.Trim(errStr, "\n"), ErrOCIRuntimeNotFound)
	}
	return fmt.Errorf("%s: %w", strings.Trim(runtimeMsg, "\n"), ErrOCIRuntime)
}

func (ci *ConmonInstance) closePipesOnCleanup() {
	ci.parentSyncPipe.Close()
	ci.parentStartPipe.Close()
	ci.parentAttachPipe.Close()
}
