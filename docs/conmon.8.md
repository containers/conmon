conmon 8 "User Commands"
==================================================

# NAME

conmon - container monitor utility

# SYNOPSIS

conmon [options]

# DESCRIPTION

conmon is a command-line program for monitoring and managing the lifecycle of
Linux containers that follow the Open Container Initiative (OCI) format.

# APPLICATION OPTIONS

**--api-version**
Conmon API version to use.

**-b**, **--bundle**
Location of the OCI Bundle path.

**-c**, **--cid**
Identification of Container.

**--exec-attach**
Attach to an exec session.

**-e**, **--exec**
Exec a command into a running container.

**--exec-process-spec**
Path to the process spec for execution.

**--exit-command**
Path to the program to execute when the container terminates its execution.

**--exit-command-arg**
Additional arguments to pass to the exit command.  Can be specified multiple time.

**--exit-delay**
Delay before invoking the exit command (in seconds).

**--exit-dir**
Path to the directory where exit files are written.

**-h**, **--help**
Show help options.

**-i**, **--stdin**
Open up a pipe to pass stdin to the container.

This option tells conmon to setup the pipe regardless of whether there is a terminal connection.

**-l**, **--log-path**
Path to store all stdout and stderr messages from the container.

**--leave-stdin-open**
Leave stdin open when the attached client disconnects.

**--log-level**
Print debug logs based on the log level.

**--log-size-max**
Maximum size of the log file.

**--log-tag**
Additional tag to use for logging.

**-n**, **--name**
Container name.

**--no-new-keyring**
Do not create a new session keyring for the container.

**--no-pivot**
Do not use pivot_root.

**--no-sync-log**
Do not manually call sync on logs after container shutdown.

**-0**, **--persist-dir**
Persistent directory for a container that can be used for storing container data.

**-p**, **--container-pidfile**
PID file for the initial pid inside of the container.

**-P**, **--conmon-pidfile**
PID file for the conmon process.

**-r**, **--runtime**
Path to store runtime data for the container.

**--replace-listen-pid**
Replace listen pid if set for oci-runtime pid.

**--restore**
Restore a container from a checkpoint.

**--runtime-arg**
Additional arguments to pass to the runtime. Can be specified multiple times.

**--runtime-opt**
Additional options to pass to the restore or exec command. Can be specified multiple times.

**-s**, **--systemd-cgroup**
Enable systemd cgroup manager, rather then use the cgroupfs directly.

**--socket-dir-path**
Location of container attach sockets.

**--sync**
Keep the main conmon process as its child by only forking once.

**--syslog**
Log to syslog (use with cgroupfs cgroup manager).

**-t**, **--terminal**
Allocate a pseudo-TTY. The default is false.

When set to true, conmon will allocate a pseudo-tty and attach  to  the
standard  input of the container. This can be used, for example, to run
a throwaway interactive shell. The default is false.

**-T**, **--timeout**
Kill container after specified timeout in seconds.

**-u**, **--cuuid**
Specify the Container UUID to use.

**--version**
Print the version and exit.

## SEE ALSO
podman(1), buildah(1), cri-o(1), crun(8), runc(8)

## HISTORY
October 2020, Originally compiled by Dan Walsh <dwalsh@redhat.com>
