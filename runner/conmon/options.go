package conmon

import (
	"fmt"
	"io"
)

type ConmonOption func(*ConmonInstance) error

func WithVersion() ConmonOption {
	return func(ci *ConmonInstance) error {
		return ci.addArgs("--version")
	}
}

func WithStdout(stdout io.Writer) ConmonOption {
	return func(ci *ConmonInstance) error {
		ci.stdout = stdout
		return nil
	}
}

func WithStderr(stderr io.Writer) ConmonOption {
	return func(ci *ConmonInstance) error {
		ci.stderr = stderr
		return nil
	}
}

func WithPath(path string) ConmonOption {
	return func(ci *ConmonInstance) error {
		ci.path = path
		return nil
	}
}

func WithContainerID(ctrID string) ConmonOption {
	return func(ci *ConmonInstance) error {
		return ci.addArgs("--cid", ctrID)
	}
}

func WithContainerUUID(ctrUUID string) ConmonOption {
	return func(ci *ConmonInstance) error {
		return ci.addArgs("--cuuid", ctrUUID)
	}
}

func WithRuntimePath(path string) ConmonOption {
	return func(ci *ConmonInstance) error {
		return ci.addArgs("--runtime", path)
	}
}

func WithLogDriver(driver, path string) ConmonOption {
	return func(ci *ConmonInstance) error {
		fullDriver := path
		if driver != "" {
			fullDriver = fmt.Sprintf("%s:%s", driver, path)
		}
		return ci.addArgs("--log-path", fullDriver)
	}
}

func (ci *ConmonInstance) addArgs(args ...string) error {
	ci.args = append(ci.args, args...)
	return nil
}
