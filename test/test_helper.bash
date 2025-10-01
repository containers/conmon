#!/usr/bin/env bash

# Common test helper functions for conmon BATS tests

# Provide basic assertion functions if not available
assert_success() {
    if [ "$status" -ne 0 ]; then
        echo "Command failed with status $status"
        echo "Output: $output"
        return 1
    fi
}

assert_failure() {
    if [ "$status" -eq 0 ]; then
        echo "Command succeeded but failure was expected"
        echo "Output: $output"
        return 1
    fi
}

# Default paths and variables
CONMON_BINARY="${CONMON_BINARY:-/usr/bin/conmon}"
RUNTIME_BINARY="${RUNTIME_BINARY:-/usr/bin/runc}"

# Detect architecture and set appropriate busybox source
ARCH=$(uname -m)
case "$ARCH" in
    x86_64)
        BUSYBOX_SOURCE="https://busybox.net/downloads/binaries/1.31.0-i686-uclibc/busybox"
        ;;
    aarch64|arm64)
        BUSYBOX_SOURCE="https://busybox.net/downloads/binaries/1.31.0-defconfig-multiarch-musl/busybox-armv8l"
        ;;
    ppc64le)
        BUSYBOX_SOURCE="https://busybox.net/downloads/binaries/1.31.0-defconfig-multiarch-musl/busybox-powerpc64"
        ;;
    ppc64)
        BUSYBOX_SOURCE="https://busybox.net/downloads/binaries/1.31.0-defconfig-multiarch-musl/busybox-powerpc64"
        ;;
    i?86)
        BUSYBOX_SOURCE="https://busybox.net/downloads/binaries/1.31.0-defconfig-multiarch-musl/busybox-i686"
        ;;
    *)
        # Default to multiarch build for other architectures
        BUSYBOX_SOURCE="https://busybox.net/downloads/binaries/1.31.0-defconfig-multiarch-musl/busybox-${ARCH}"
        ;;
esac

BUSYBOX_DEST_DIR="/tmp/conmon-test-images"
BUSYBOX_DEST="/tmp/conmon-test-images/busybox"
VALID_PATH="/tmp"
INVALID_PATH="/not/a/path"

# Generate a unique container ID for each test
generate_ctr_id() {
    echo "conmon-test-$(date +%s)-$$-$RANDOM"
}

# Cache busybox binary for container tests
cache_busybox() {
    if [[ -f "$BUSYBOX_DEST" ]]; then
        return 0
    fi

    mkdir -p "$BUSYBOX_DEST_DIR"
    if ! curl -s -L "$BUSYBOX_SOURCE" -o "$BUSYBOX_DEST"; then
        skip "Failed to download busybox binary"
    fi
    chmod +x "$BUSYBOX_DEST"
}

# Run conmon with given arguments and capture output
run_conmon() {
    run "$CONMON_BINARY" "$@"
}

# Run runtime command (runc)
run_runtime() {
    run "$RUNTIME_BINARY" "$@"
}

# Get journal output for conmon process
get_conmon_journal_output() {
    local pid="$1"
    local level="${2:--1}"

    if ! command -v journalctl >/dev/null 2>&1; then
        echo ""
        return 0
    fi

    local level_filter=""
    if [[ "$level" != "-1" ]]; then
        level_filter="-p $level"
    fi

    journalctl -q --no-pager $level_filter _COMM=conmon _PID="$pid" 2>/dev/null || echo ""
}

# Create a temporary directory for test
setup_tmpdir() {
    export TEST_TMPDIR
    TEST_TMPDIR=$(mktemp -d /tmp/conmon-test-XXXXXX)
}

# Cleanup temporary directory
cleanup_tmpdir() {
    if [[ -n "$TEST_TMPDIR" ]]; then
        # Handle race condition where conmon might still be creating files
        local retries=5
        while [[ $retries -gt 0 ]]; do
            if rm -rf "$TEST_TMPDIR" 2>/dev/null; then
                break
            fi
            sleep 0.1
            ((retries--))
        done
    fi
}

# Generate process.json
generate_process_spec() {
    local command="$1"
    if [[ -z "$command" ]]; then
        command="for i in \$(/busybox seq 1 100); do /busybox echo \\\"hello from busybox \$i\\\"; done"
    fi
    if [[ -z "$BUNDLE_PATH" || ! -e "$BUNDLE_PATH" ]]; then
        die "The BUNDLE_PATH directory does not exist. Ensure 'generate_process_spec'" \
        " is called after the 'setup_test_env'"
    fi
    local config_path="$BUNDLE_PATH/process.json"

    cat > "$config_path" << EOF
{
    "terminal": false,
    "user": {
        "uid": 0,
        "gid": 0
    },
    "args": [
        "/busybox",
        "sh",
        "-c",
        "$command"
    ],
    "env": [
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
    ],
    "cwd": "/",
    "capabilities": {
        "bounding": [],
        "effective": [],
        "inheritable": [],
        "permitted": [],
        "ambient": []
    },
    "rlimits": [
        {
            "type": "RLIMIT_NOFILE",
            "hard": 1024,
            "soft": 1024
        }
    ],
    "noNewPrivileges": true
}
EOF
}

# Generate OCI runtime configuration
generate_runtime_config() {
    local bundle_path="$1"
    local rootfs="$2"
    local use_terminal="$3"
    local command="$4"
    if [[ -z "$use_terminal" ]]; then
        use_terminal="false"
    fi
    if [[ -z "$command" ]]; then
        command="for i in \$(/busybox seq 1 100); do /busybox echo \\\"hello from busybox \$i\\\"; done"
    fi
    local config_path="$bundle_path/config.json"

    # Make rootfs path relative to bundle
    local relative_rootfs
    relative_rootfs=$(basename "$rootfs")

    # Get current user UID and GID
    local host_uid host_gid
    host_uid=$(id -u)
    host_gid=$(id -g)

    cat > "$config_path" << EOF
{
    "ociVersion": "1.0.0",
    "process": {
        "terminal": $use_terminal,
        "user": {
            "uid": 0,
            "gid": 0
        },
        "args": [
            "/busybox",
            "sh",
            "-c",
            "$command"
        ],
        "env": [
            "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
        ],
        "cwd": "/",
        "capabilities": {
            "bounding": [],
            "effective": [],
            "inheritable": [],
            "permitted": [],
            "ambient": []
        },
        "rlimits": [
            {
                "type": "RLIMIT_NOFILE",
                "hard": 1024,
                "soft": 1024
            }
        ],
        "noNewPrivileges": true
    },
    "root": {
        "path": "$relative_rootfs",
        "readonly": true
    },
    "hostname": "conmon-test",
    "mounts": [
        {
            "destination": "/proc",
            "type": "proc",
            "source": "proc"
        },
        {
            "destination": "/tmp",
            "type": "tmpfs",
            "source": "tmpfs",
            "options": [
                "nosuid",
                "nodev",
                "mode=1777"
            ]
        },
        {
            "destination": "/dev",
            "type": "tmpfs",
            "source": "tmpfs",
            "options": [
                "nosuid",
                "strictatime",
                "mode=755",
                "size=65536k"
            ]
        },
        {
            "destination": "/dev/pts",
            "type": "devpts",
            "source": "devpts",
            "options": [
                "nosuid",
                "noexec",
                "newinstance",
                "ptmxmode=0666",
                "mode=0620"
            ]
        }
    ],
    "linux": {
        "resources": {
            "devices": [
                {
                    "allow": false,
                    "access": "rwm"
                }
            ]
        },
        "namespaces": [
            {
                "type": "pid"
            },
            {
                "type": "ipc"
            },
            {
                "type": "uts"
            },
            {
                "type": "mount"
            },
            {
                "type": "user"
            }
        ],
        "uidMappings": [
            {
                "containerID": 0,
                "hostID": $host_uid,
                "size": 1
            }
        ],
        "gidMappings": [
            {
                "containerID": 0,
                "hostID": $host_gid,
                "size": 1
            }
        ],
        "maskedPaths": [
            "/proc/acpi",
            "/proc/kcore",
            "/proc/keys",
            "/proc/latency_stats",
            "/proc/timer_list",
            "/proc/timer_stats",
            "/proc/sched_debug",
            "/proc/scsi",
            "/sys/firmware"
        ],
        "readonlyPaths": [
            "/proc/asound",
            "/proc/bus",
            "/proc/fs",
            "/proc/irq",
            "/proc/sys",
            "/proc/sysrq-trigger"
        ]
    }
}
EOF
}

# Setup common test environment
setup_test_env() {
    setup_tmpdir
    export CTR_ID
    CTR_ID=$(generate_ctr_id)
    export LOG_PATH="$TEST_TMPDIR/container.log"
    export PID_FILE="$TEST_TMPDIR/pidfile"
    export CONMON_PID_FILE="$TEST_TMPDIR/conmon-pidfile"
    export BUNDLE_PATH="$TEST_TMPDIR"
    export ROOTFS="$TEST_TMPDIR/rootfs"
    export SOCKET_PATH="$TEST_TMPDIR"
    export ATTACH_PATH="$TEST_TMPDIR/attach"
    export OCI_ATTACHPIPE_PATH="$TEST_TMPDIR/attach-pipe"
    export OCI_STARTPIPE_PATH="$TEST_TMPDIR/start-pipe"
    export OCI_SYNCPIPE_PATH="$TEST_TMPDIR/sync-pipe"
    export CTL_PATH="$TEST_TMPDIR/ctl"
}

# Setup full container environment with busybox
setup_container_env() {
    local command="$1"
    local use_terminal="$2"
    setup_test_env

    # Cache busybox binary for container tests
    cache_busybox

    # Create the rootfs directory structure
    mkdir -p "$ROOTFS"/{bin,sbin,etc,proc,sys,dev,tmp}

    # Copy busybox to rootfs and set up basic filesystem
    cp "$BUSYBOX_DEST" "$ROOTFS/busybox"
    chmod +x "$ROOTFS/busybox"

    # Create busybox symlinks for common commands
    ln -sf busybox "$ROOTFS/bin/sh"
    ln -sf busybox "$ROOTFS/bin/echo"
    ln -sf busybox "$ROOTFS/bin/ls"
    ln -sf busybox "$ROOTFS/bin/cat"

    # Create minimal /etc files
    echo "root:x:0:0:root:/:/bin/sh" > "$ROOTFS/etc/passwd"
    echo "root:x:0:" > "$ROOTFS/etc/group"

    # Generate OCI runtime configuration
    generate_runtime_config "$BUNDLE_PATH" "$ROOTFS" "$use_terminal" "$command"
}

# Cleanup test environment
cleanup_test_env() {
    # Clean up any running containers
    if [[ -n "$CTR_ID" ]]; then
        "$RUNTIME_BINARY" delete -f "$CTR_ID" 2>/dev/null || true
    fi
    cleanup_tmpdir
}

# Check if conmon binary exists and is executable
check_conmon_binary() {
    if [[ ! -x "$CONMON_BINARY" ]]; then
        skip "conmon binary not found or not executable at $CONMON_BINARY"
    fi
}

# Check if runtime binary exists and is executable
check_runtime_binary() {
    if [[ ! -x "$RUNTIME_BINARY" ]]; then
        skip "runtime binary not found or not executable at $RUNTIME_BINARY"
    fi
}

# Helper to check if a string contains a substring
assert_output_contains() {
    local expected="$1"
    if [[ "$output" != *"$expected"* ]]; then
        echo "Expected output to contain: $expected"
        echo "Actual output: $output"
        return 1
    fi
}

# Helper to check if stderr contains a substring
assert_stderr_contains() {
    local expected="$1"
    if [[ "$stderr" != *"$expected"* ]]; then
        echo "Expected stderr to contain: $expected"
        echo "Actual stderr: $stderr"
        return 1
    fi
}

# Helper function to wait until "runc state $cid" returns expected status.
wait_for_runtime_status() {
    local cid=$1
    local expected_status=$2
    local how_long=5

    t1=$(expr $SECONDS + $how_long)
    while [ $SECONDS -lt $t1 ]; do
        run_runtime state "$cid"
        echo "$output"
        if expr "$output" : ".*status\": \"$expected_status"; then
            return
        fi
        sleep 0.5
    done

    die "timed out waiting for '$expected_status' from $cid"
}

# Helper function to start conmon with default arguments.
# Additional conmon arguments can be passed to this function.
start_conmon_with_default_args() {
    local extra_args=("$@")
    run timeout 10s "$CONMON_BINARY" \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$RUNTIME_BINARY" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --log-level trace \
        --container-pidfile "$PID_FILE" \
        --syslog \
        --conmon-pidfile "$CONMON_PID_FILE" "${extra_args[@]}"

    if [ "$status" -ne 0 ]; then
        return
    fi

    # Do not start the container if it's already running. This can happen
    # when `start_conmon_with_default_args` has already been called and this
    # second call uses option like --exec which connects to already running
    # container.
    run_runtime state "$CTR_ID"
    echo "$output"
    if expr "$output" : ".*status\": \"running"; then
        return
    fi

    # Wait until the container is created
    wait_for_runtime_status "$CTR_ID" created

    # Check that conmon pidfile was created
    [ -f "$CONMON_PID_FILE" ]

    # Start the container and wait until it really starts.
    run_runtime start "$CTR_ID"
}

# Helper function to run conmon with default arguments and wait until it is stopped.
# Additional conmon arguments can be passed to this function.
run_conmon_with_default_args() {
    start_conmon_with_default_args "$@"
    wait_for_runtime_status "$CTR_ID" stopped
}

# Generic helper function to create pipe and read from it.
_start_pipe_reader() {
    local pipe_path=$1
    local pipe_fd_env_name=$2
    local pipe_fd_number=$3
    local output_file=$4

    # Create the pipe and export the env variable.
    mkfifo "$pipe_path"
    export "$pipe_fd_env_name"="$pipe_fd_number"

    # Run the reader in the background, otherwise it would block until the
    # conmon opens the other side of the pipe.
    {
        exec {r}<"$pipe_path"
        while IFS= read -r -u "$r" line; do
            echo "$line" >>$output_file
        done
    } &
}

# Helper function to create the _OCI_SYNCPIPE pipe and start the reader.
# The data read from the pipe is stored in the $TEST_TMPDIR/syncpipe-output
# file.
# To pass the pipe to conmon, use the `6>"$OCI_SYNCPIPE_PATH"` as argument
# to `run_conmon_with_default_args` or `start_conmon_with_default_args`.
start_oci_sync_pipe_reader() {
    _start_pipe_reader "$OCI_SYNCPIPE_PATH" "_OCI_SYNCPIPE" 6 "$TEST_TMPDIR/syncpipe-output"
}

# Helper function to create the _OCI_ATTACHPIPE pipe and start the reader.
# The data read from the pipe is stored in the $TEST_TMPDIR/attachpipe-output
# file.
# To pass the pipe to conmon, use the `4>"$OCI_ATTACHPIPE_PATH"` as argument
# to `run_conmon_with_default_args` or `start_conmon_with_default_args`.
start_oci_attach_pipe_reader() {
    _start_pipe_reader "$OCI_ATTACHPIPE_PATH" "_OCI_ATTACHPIPE" 4 "$TEST_TMPDIR/attachpipe-output"
}

# Helper function ensuring the file does not exist.
assert_file_not_exists() {
    FILE=$1
    if [ -e "$FILE" ]; then
        die "$(date): File $FILE exists."
    fi
}

# Helper function ensuring the file does exist.
assert_file_exists() {
    FILE=$1
    if [ ! -e "$FILE" ]; then
        die "$(date): File $FILE does not exist."
    fi
}

# bail-now is how we terminate a test upon assertion failure.
# By default, and the vast majority of the time, it just triggers
# immediate test termination;
function bail-now() {
    # "false" does not apply to "bail now"! It means "nonzero exit",
    # which BATS interprets as "yes, bail immediately".
    false
}

############
#  assert  #  Compare actual vs expected string; fail if mismatch
############
#
# Compares string (default: $output) against the given string argument.
# By default we do an exact-match comparison against $output, but there
# are two different ways to invoke us, each with an optional description:
#
#      assert               "EXPECT" [DESCRIPTION]
#      assert "RESULT" "OP" "EXPECT" [DESCRIPTION]
#
# The first form (one or two arguments) does an exact-match comparison
# of "$output" against "EXPECT". The second (three or four args) compares
# the first parameter against EXPECT, using the given OPerator. If present,
# DESCRIPTION will be displayed on test failure.
#
# Examples:
#
#   assert "this is exactly what we expect"
#   assert "${lines[0]}" =~ "^abc"  "first line begins with abc"
#
function assert() {
    local actual_string="$output"
    local operator='=='
    local expect_string="$1"
    local testname="$2"

    case "${#*}" in
        0)   die "Internal error: 'assert' requires one or more arguments" ;;
        1|2) ;;
        3|4) actual_string="$1"
             operator="$2"
             expect_string="$3"
             testname="$4"
             ;;
        *)   die "Internal error: too many arguments to 'assert'" ;;
    esac

    # Comparisons.
    # Special case: there is no !~ operator, so fake it via '! x =~ y'
    local not=
    local actual_op="$operator"
    if [[ $operator == '!~' ]]; then
        not='!'
        actual_op='=~'
    fi
    if [[ $operator == '=' || $operator == '==' ]]; then
        # Special case: we can't use '=' or '==' inside [[ ... ]] because
        # the right-hand side is treated as a pattern... and '[xy]' will
        # not compare literally. There seems to be no way to turn that off.
        if [ "$actual_string" = "$expect_string" ]; then
            return
        fi
    elif [[ $operator == '!=' ]]; then
        # Same special case as above
        if [ "$actual_string" != "$expect_string" ]; then
            return
        fi
    else
        if eval "[[ $not \$actual_string $actual_op \$expect_string ]]"; then
            return
        elif [ $? -gt 1 ]; then
            die "Internal error: could not process 'actual' $operator 'expect'"
        fi
    fi

    # Test has failed. Get a descriptive test name.
    if [ -z "$testname" ]; then
        testname="${MOST_RECENT_PODMAN_COMMAND:-[no test name given]}"
    fi

    # Display optimization: the typical case for 'expect' is an
    # exact match ('='), but there are also '=~' or '!~' or '-ge'
    # and the like. Omit the '=' but show the others; and always
    # align subsequent output lines for ease of comparison.
    local op=''
    local ws=''
    if [ "$operator" != '==' ]; then
        op="$operator "
        ws=$(printf "%*s" ${#op} "")
    fi

    # This is a multi-line message, which may in turn contain multi-line
    # output, so let's format it ourself to make it more readable.
    local expect_split
    mapfile -t expect_split <<<"$expect_string"
    local actual_split
    mapfile -t actual_split <<<"$actual_string"

    # bash %q is really nice, except for the way it backslashes spaces
    local -a expect_split_q
    for line in "${expect_split[@]}"; do
        local q=$(printf "%q" "$line" | sed -e 's/\\ / /g')
        expect_split_q+=("$q")
    done
    local -a actual_split_q
    for line in "${actual_split[@]}"; do
        local q=$(printf "%q" "$line" | sed -e 's/\\ / /g')
        actual_split_q+=("$q")
    done

    printf "#/vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv\n"    >&2
    printf "#|     FAIL: %s\n" "$testname"                        >&2
    printf "#| expected: %s%s\n" "$op" "${expect_split_q[0]}"     >&2
    local line
    for line in "${expect_split_q[@]:1}"; do
        printf "#|         > %s%s\n" "$ws" "$line"                >&2
    done
    printf "#|   actual: %s%s\n" "$ws" "${actual_split_q[0]}"     >&2
    for line in "${actual_split_q[@]:1}"; do
        printf "#|         > %s%s\n" "$ws" "$line"                >&2
    done
    printf "#\\^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n"   >&2
    bail-now
}

function die() {
    # FIXME: handle multi-line output
    echo "#/vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv"  >&2
    echo "#| FAIL: $*"                                           >&2
    echo "#\\^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^" >&2
    bail-now
}
