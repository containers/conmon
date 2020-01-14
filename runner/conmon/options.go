package conmon

import (
	"fmt"
	"io"
)

type ConmonOption func(*ConmonInstance) error

func WithVersion() ConmonOption {
	return func(ci *ConmonInstance) error {
		ci.args = append(ci.args, "--version")
		return nil
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
		ci.args = append(ci.args, "--cid", ctrID)
		return nil
	}
}

func WithContainerUUID(ctrUUID string) ConmonOption {
	return func(ci *ConmonInstance) error {
		ci.args = append(ci.args, "--cuuid", ctrUUID)
		return nil
	}
}

func WithRuntimePath(path string) ConmonOption {
	return func(ci *ConmonInstance) error {
		ci.args = append(ci.args, "--runtime", path)
		return nil
	}
}

func WithLogDriver(driver, path string) ConmonOption {
	return func(ci *ConmonInstance) error {
		fullDriver := path
		if driver != "" {
			fullDriver = fmt.Sprintf("%s:%s", driver, path)
		}
		ci.args = append(ci.args, "--log-path", fullDriver)
		return nil
	}
}
