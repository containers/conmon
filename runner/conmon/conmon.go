package conmon

import (
	"errors"
	"io"
	"os/exec"
)

var (
	ErrConmonNotStarted = errors.New("conmon instance is not started")
)

type ConmonInstance struct {
	args    []string
	cmd     *exec.Cmd
	started bool
	path    string
	stdout  io.Writer
	stderr  io.Writer
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

	ci.cmd.Stdout = ci.stdout
	ci.cmd.Stderr = ci.stderr
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
		return nil, errors.New("conmon instance is not started")
	}
	return ci.cmd.Stderr, nil
}

func CreateAndExecConmon(options ...ConmonOption) (*ConmonInstance, error) {
	ci, err := NewConmonInstance(options...)
	if err != nil {
		return nil, err
	}

	ci.Start()
	return ci, nil
}
