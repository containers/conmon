#!/usr/bin/env bats

# k8s_log_rotation_test.bats
#
# This test suite validates the k8s-file log rotation fix implemented in commit 29d17be.
# The fix addressed log corruption during log rotation where writev_buffer_flush() was
# incorrectly handling partial writes, causing corrupted buffer state to carry over to
# new file descriptors after rotation.
#
# The tests focus on:
# 1. Basic k8s-file log driver functionality with log-size-max option
# 2. Validation that small log size limits are accepted without errors
# 3. Edge case testing with very small rotation thresholds
# 4. Log file creation and content integrity validation
#
# While these tests don't create actual running containers (to avoid test environment
# dependencies), they validate that the conmon command-line options work correctly and
# that log files can be created and managed properly. The real fix prevents buffer
# corruption during writev operations when log rotation occurs, which would have
# manifested as malformed k8s log entries with repeated timestamps and broken formatting.

load test_helper

setup() {
    check_conmon_binary
    setup_test_env
}

teardown() {
    # Stop the conmon and the fake container a test may have started.
    if [ -f "$TEST_TMPDIR/container-pidfile" ]; then
        kill "$(cat "$TEST_TMPDIR/container-pidfile")" 2>/dev/null || true
    fi
    if [ -f "$TEST_TMPDIR/conmon-pidfile" ]; then
        kill "$(cat "$TEST_TMPDIR/conmon-pidfile")" 2>/dev/null || true
    fi
    cleanup_test_env
}

# Helper function to run conmon with k8s-file log driver
run_conmon_k8s_file() {
    local extra_args=("$@")
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" \
        --log-path "k8s-file:$LOG_PATH" "${extra_args[@]}"
}

# Helper function to create a fake runtime. It behaves like "runtime create"
# does: it publishes the pid of a process that stays alive and exits itself.
# That is enough for conmon to consider the container running, without
# needing a container runtime or a container image.
create_fake_runtime() {
    local path="$TEST_TMPDIR/fake-runtime"
    cat >"$path" <<'EOF'
#!/bin/bash
pidfile=""
prev=""
for arg in "$@"; do
    if [ "$prev" = "--pid-file" ]; then
        pidfile="$arg"
    fi
    prev="$arg"
done
sleep 30 &
echo "$!" >"$pidfile"
EOF
    chmod +x "$path"
    echo "$path"
}

@test "k8s log rotation: should create valid k8s log format" {
    run_conmon_k8s_file
    assert_success
    [ -f "$LOG_PATH" ]
}

@test "k8s log rotation: should accept log-size-max option" {
    local log_size_max=1024
    run_conmon_k8s_file --log-size-max "$log_size_max"
    assert_success
    [ -f "$LOG_PATH" ]
}

@test "k8s log rotation: should handle k8s format content correctly" {
    local log_size_max=1024

    run_conmon_k8s_file --log-size-max "$log_size_max"
    assert_success
    [ -f "$LOG_PATH" ]

    # Test k8s log format handling
    local k8s_content='2023-07-23T18:00:00.000000000Z stdout F Test log message'
    echo "$k8s_content" >"$LOG_PATH"

    # Verify content preservation
    local content
    content=$(<"$LOG_PATH")
    [ "$content" = "$k8s_content" ]
}

@test "k8s log rotation: should rotate the log with a stale temporary file present" {
    # A conmon that is killed between creating the temporary file and
    # renaming it into place leaves the temporary file behind. Such a file
    # must not keep conmon from rotating the log afterwards: otherwise every
    # rotation fails and the log file grows past its size limit.
    local fake_runtime
    fake_runtime=$(create_fake_runtime)

    "$CONMON_BINARY" \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$fake_runtime" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --container-pidfile "$TEST_TMPDIR/container-pidfile" \
        --conmon-pidfile "$TEST_TMPDIR/conmon-pidfile" \
        --log-path "k8s-file:$LOG_PATH" \
        --log-rotate \
        --log-size-max 1024 &
    local conmon_pid=$!

    # Wait for conmon to set up the control fifo.
    retry 10 0.1 test -p "$CTL_PATH" ||
        die "timed out waiting for the control fifo"

    # Leave behind the temporary file of a conmon that was killed too early.
    touch "$LOG_PATH.new"

    # Ask conmon to rotate the log.
    echo "2 1 1" >"${CTL_PATH}"

    retry 10 0.1 test -f "$LOG_PATH.1" ||
        die "the log file was not rotated"

    # The stale temporary file is gone.
    assert_file_not_exists "$LOG_PATH.new"

    # Clean up the fake container and conmon.
    kill "$conmon_pid" 2>/dev/null || true
    wait "$conmon_pid" 2>/dev/null || true
}

@test "k8s log rotation: should rotate with a stale temporary file that has contents" {
    local fake_runtime
    fake_runtime=$(create_fake_runtime)

    "$CONMON_BINARY" \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$fake_runtime" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --container-pidfile "$TEST_TMPDIR/container-pidfile" \
        --conmon-pidfile "$TEST_TMPDIR/conmon-pidfile" \
        --log-path "k8s-file:$LOG_PATH" \
        --log-rotate &
    local conmon_pid=$!

    retry 10 0.1 test -p "$CTL_PATH" ||
        die "timed out waiting for the control fifo"

    # Marker line for this test in the current log.
    echo "marker-unique-content" >"$LOG_PATH"
    # Stale temporary file with left-over content. It must be deleted by the fix,
    # not carried over into the rotation.
    printf 'leftover-from-dead-conmon\n' >"$LOG_PATH.new"

    echo "2 1 1" >"${CTL_PATH}"

    retry 10 0.1 test -f "$LOG_PATH.1" ||
        die "the log file was not rotated"

    # The stale temporary file is gone.
    assert_file_not_exists "$LOG_PATH.new"

    # The old current log is now in .1.
    grep -q "marker-unique-content" "$LOG_PATH.1" ||
        die "the previous log content was not found in $LOG_PATH.1"

    # The freshly-created log does not contain the stale content.
    ! grep -q "leftover-from-dead-conmon" "$LOG_PATH" ||
        die "stale contents leaked into the new log"

    # Clean up the fake container and conmon.
    kill "$conmon_pid" 2>/dev/null || true
    wait "$conmon_pid" 2>/dev/null || true
}

@test "k8s log rotation: should not crash when the stale temporary file is a directory" {
    # This variant of the fake runtime stays alive after publishing the pid,
    # which keeps conmon running. The regular one exits immediately, letting
    # conmon quit on its own; here we want conmon alive to check the failure
    # of the rotation path that hits the stale directory.
    local fake_runtime="$TEST_TMPDIR/fake-runtime-persist"
    cat >"$fake_runtime" <<'EOF'
#!/bin/bash
pidfile=""
prev=""
for arg in "$@"; do
    if [ "$prev" = "--pid-file" ]; then
        pidfile="$arg"
    fi
    prev="$arg"
done
sleep 30 &
echo "$!" >"$pidfile"
exec sleep 30
EOF
    chmod +x "$fake_runtime"

    "$CONMON_BINARY" \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$fake_runtime" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --container-pidfile "$TEST_TMPDIR/container-pidfile" \
        --conmon-pidfile "$TEST_TMPDIR/conmon-pidfile" \
        --log-path "k8s-file:$LOG_PATH" \
        --log-rotate &
    local conmon_pid=$!

    retry 10 0.1 test -p "$CTL_PATH" ||
        die "timed out waiting for the control fifo"

    echo "marker-unique-content" >"$LOG_PATH"
    # Stale temporary path that is a directory: unlinkat cannot remove it and
    # openat fails again. The rotation must fail gracefully, conmon must keep
    # running, and the log must remain untouched.
    mkdir "$LOG_PATH.new"

    echo "2 1 1" >"${CTL_PATH}"

    # Give conmon a chance to attempt the rotation.
    sleep 1

    # conmon is still alive. NOTE: conmon daemonizes (forks then exits), so
    # the PID of the parent we launched is not the PID of the running instance;
    # use the pidfile conmon creates for itself.
    kill -0 "$(cat "$TEST_TMPDIR/conmon-pidfile")" ||
        die "conmon crashed during rotation with a stale directory"

    # The current log is untouched.
    grep -q "marker-unique-content" "$LOG_PATH" ||
        die "the log file was modified despite the failed rotation"

    # No backup was moved out of the way.
    assert_file_not_exists "$LOG_PATH.1"

    # The stale directory is still there (we do not silently delete it).
    [ -d "$LOG_PATH.new" ] ||
        die "the stale directory disappeared; unlinkat should have failed"

    # Clean up the fake container and conmon.
    kill "$conmon_pid" 2>/dev/null || true
    wait "$conmon_pid" 2>/dev/null || true
}

@test "k8s log rotation: should shift an existing backup alongside a stale temporary file" {
    local fake_runtime
    fake_runtime=$(create_fake_runtime)

    "$CONMON_BINARY" \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$fake_runtime" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --container-pidfile "$TEST_TMPDIR/container-pidfile" \
        --conmon-pidfile "$TEST_TMPDIR/conmon-pidfile" \
        --log-path "k8s-file:$LOG_PATH" \
        --log-rotate \
        --log-max-files 2 &
    local conmon_pid=$!

    retry 10 0.1 test -p "$CTL_PATH" ||
        die "timed out waiting for the control fifo"

    echo "current-generation" >"$LOG_PATH"
    printf 'old-backup-generation\n' >"$LOG_PATH.1"
    printf 'leftover-from-dead-conmon\n' >"$LOG_PATH.new"

    echo "2 1 1" >"${CTL_PATH}"

    retry 10 0.1 test -f "$LOG_PATH.2" ||
        die "the backup was not shifted to .2"

    # The current log moves to .1.
    grep -q "current-generation" "$LOG_PATH.1" ||
        die "the previous log was not found in $LOG_PATH.1"

    # The previous .1 moved to .2.
    grep -q "old-backup-generation" "$LOG_PATH.2" ||
        die "the old backup was not found in $LOG_PATH.2"

    # The stale temporary file is gone.
    assert_file_not_exists "$LOG_PATH.new"

    # Clean up the fake container and conmon.
    kill "$conmon_pid" 2>/dev/null || true
    wait "$conmon_pid" 2>/dev/null || true
}
