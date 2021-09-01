package conmon

import (
	"io"
	"io/ioutil"
	"os"
	"os/exec"
	"strconv"

	"github.com/pkg/errors"
)

var (
	ErrConmonNotStarted = errors.New("conmon instance is not started")
)

type ConmonInstance struct {
	args    []string
	cmd     *exec.Cmd
	started bool
	path    string
	pidFile string
	stdout  io.Writer
	stderr  io.Writer
	stdin   io.Reader

	parentStartPipe  *os.File
	parentAttachPipe *os.File
	parentSyncPipe   *os.File
	childSyncPipe    *os.File
	childStartPipe   *os.File
	childAttachPipe  *os.File
}

func CreateAndExecConmon(options ...ConmonOption) (*ConmonInstance, error) {
	ci, err := NewConmonInstance(options...)
	if err != nil {
		return nil, err
	}

	ci.Start()
	return ci, nil
}

func NewConmonInstance(options ...ConmonOption) (*ConmonInstance, error) {
	ci := &ConmonInstance{
		args: make([]string, 0),
	}
	for _, option := range options {
		if err := option(ci); err != nil {
			return nil, err
		}
	}

	// TODO verify path more
	if ci.path == "" {
		return nil, errors.New("conmon path not specified")
	}

	ci.cmd = exec.Command(ci.path, ci.args...)
	ci.configurePipeEnv()

	ci.cmd.Stdout = ci.stdout
	ci.cmd.Stderr = ci.stderr
	ci.cmd.Stdin = ci.stdin
	return ci, nil
}

func (ci *ConmonInstance) Start() error {
	ci.started = true
	return ci.cmd.Start()
}

func (ci *ConmonInstance) Wait() error {
	if !ci.started {
		return ErrConmonNotStarted
	}
	defer func() {
		ci.childSyncPipe.Close()
		ci.childStartPipe.Close()
		ci.childAttachPipe.Close()
	}()
	return ci.cmd.Wait()
}

func (ci *ConmonInstance) Stdout() (io.Writer, error) {
	if !ci.started {
		return nil, ErrConmonNotStarted
	}
	return ci.cmd.Stdout, nil
}

func (ci *ConmonInstance) Stderr() (io.Writer, error) {
	if !ci.started {
		return nil, ErrConmonNotStarted
	}
	return ci.cmd.Stderr, nil
}

func (ci *ConmonInstance) Pid() (int, error) {
	if ci.pidFile == "" {
		return -1, errors.Errorf("conmon pid file not specified")
	}
	if !ci.started {
		return -1, ErrConmonNotStarted
	}

	pid, err := readConmonPidFile(ci.pidFile)
	if err != nil {
		return -1, errors.Wrapf(err, "failed to find conmon pid file")
	}
	return pid, nil
}

// readConmonPidFile attempts to read conmon's pid from its pid file
func readConmonPidFile(pidFile string) (int, error) {
	// Let's try reading the Conmon pid at the same time.
	if pidFile != "" {
		contents, err := ioutil.ReadFile(pidFile)
		if err != nil {
			return -1, err
		}
		// Convert it to an int
		conmonPID, err := strconv.Atoi(string(contents))
		if err != nil {
			return -1, err
		}
		return conmonPID, nil
	}
	return 0, nil
}

func (ci *ConmonInstance) Cleanup() {
	ci.closePipesOnCleanup()
}
