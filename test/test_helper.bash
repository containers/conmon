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
BUSYBOX_SOURCE="https://busybox.net/downloads/binaries/1.31.0-i686-uclibc/busybox"
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

# Generate OCI runtime configuration
generate_runtime_config() {
    local bundle_path="$1"
    local rootfs="$2"
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
        "terminal": false,
        "user": {
            "uid": 0,
            "gid": 0
        },
        "args": [
            "/busybox",
            "echo",
            "busybox"
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
}

# Setup full container environment with busybox
setup_container_env() {
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
    generate_runtime_config "$BUNDLE_PATH" "$ROOTFS"
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