package conmon

import (
	"fmt"
	"io"
	"os"

	"golang.org/x/sys/unix"
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

func WithStdin(stdin io.Reader) ConmonOption {
	return func(ci *ConmonInstance) error {
		ci.stdin = stdin
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

func WithLogPath(path string) ConmonOption {
	return func(ci *ConmonInstance) error {
		return ci.addArgs("--log-path", path)
	}
}

func WithBundlePath(path string) ConmonOption {
	return func(ci *ConmonInstance) error {
		return ci.addArgs("--bundle", path)
	}
}

func WithSyslog() ConmonOption {
	return func(ci *ConmonInstance) error {
		return ci.addArgs("--syslog")
	}
}

func WithLogLevel(level string) ConmonOption {
	return func(ci *ConmonInstance) error {
		// TODO verify level is right
		return ci.addArgs("--log-level", level)
	}
}

func WithSocketPath(path string) ConmonOption {
	return func(ci *ConmonInstance) error {
		// TODO verify path is right
		// TODO automatically add container ID? right now it's callers responsibility
		return ci.addArgs("--socket-dir-path", path)
	}
}

func WithContainerPidFile(path string) ConmonOption {
	return func(ci *ConmonInstance) error {
		// TODO verify path is right
		return ci.addArgs("--container-pidfile", path)
	}
}

func WithRuntimeConfig(path string) ConmonOption {
	return func(ci *ConmonInstance) error {
		// TODO verify path is right
		return ci.addArgs("--container-pidfile", path)
	}
}

func WithConmonPidFile(path string) ConmonOption {
	return func(ci *ConmonInstance) error {
		// TODO verify path is right
		ci.pidFile = path
		return ci.addArgs("--conmon-pidfile", path)
	}
}

func WithStartPipe() ConmonOption {
	return func(ci *ConmonInstance) error {
		read, write, err := newPipe()
		if err != nil {
			return err
		}
		ci.parentStartPipe = write
		ci.childStartPipe = read
		return nil
	}
}

func WithAttachPipe() ConmonOption {
	return func(ci *ConmonInstance) error {
		read, write, err := newPipe()
		if err != nil {
			return err
		}
		ci.parentAttachPipe = read
		ci.childAttachPipe = write
		return nil
	}
}

func WithSyncPipe() ConmonOption {
	return func(ci *ConmonInstance) error {
		read, write, err := newPipe()
		if err != nil {
			return err
		}
		ci.parentSyncPipe = read
		ci.childSyncPipe = write
		return nil
	}
}

// newPipe creates a unix socket pair for communication
func newPipe() (read *os.File, write *os.File, err error) {
	fds, err := unix.Socketpair(unix.AF_LOCAL, unix.SOCK_SEQPACKET|unix.SOCK_CLOEXEC, 0)
	if err != nil {
		return nil, nil, err
	}
	return os.NewFile(uintptr(fds[1]), "read"), os.NewFile(uintptr(fds[0]), "write"), nil
}

func (ci *ConmonInstance) addArgs(args ...string) error {
	ci.args = append(ci.args, args...)
	return nil
}
