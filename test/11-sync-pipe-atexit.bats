#!/usr/bin/env bats

# Tests for sync pipe atexit handler - ensures sync pipe always gets a
# response even when --exit-command is specified (which registers an
# atexit handler that closes all fds >= 3).

load test_helper

setup() {
    check_conmon_binary
    check_runtime_binary
    setup_container_env '"'
}

teardown() {
    cleanup_test_env
}

@test "sync-pipe-atexit: error reported with exit-command" {
    # This test validates the fix for the do_exit_command atexit ordering bug.
    # When --exit-command is specified, do_exit_command is registered as an
    # atexit handler AFTER the sync pipe handler. Since atexit runs in
    # reverse order, do_exit_command runs FIRST and calls
    # close_all_fds_ge_than(3), which would close the sync pipe fd before
    # the sync pipe handler gets to write.
    #
    # The fix calls report_sync_pipe_if_needed() at the start of
    # do_exit_command() before closing fds.
    #
    # We use a malformed config.json (the trailing quote in setup) to make
    # the runtime fail, which causes conmon to exit with error.

    start_oci_sync_pipe_reader

    run_conmon \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$RUNTIME_BINARY" \
        --log-path "k8s-file:$LOG_PATH" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --syslog \
        --log-level trace \
        --container-pidfile "$PID_FILE" \
        --conmon-pidfile "$CONMON_PID_FILE" \
        --exit-command /bin/true \
        6>"$OCI_SYNCPIPE_PATH"

    # Give conmon some time to run the runtime and fail.
    sleep 1

    assert_file_exists "$CONMON_PID_FILE"
    CONMON_PID=$(cat "$CONMON_PID_FILE")
    wait "$CONMON_PID" 2>/dev/null || true

    # The sync pipe MUST have received error JSON, even with --exit-command.
    # Without the fix, this file would be empty because do_exit_command
    # closes fds before the sync pipe atexit handler runs.
    assert_file_exists "$TEST_TMPDIR"/syncpipe-output
    run cat "$TEST_TMPDIR"/syncpipe-output
    echo "syncpipe output: $output"
    assert "${output}" =~ '"pid": -1' "sync pipe should report error (pid: -1)"
    assert "${output}" =~ '"message":' "sync pipe should contain error message"
}

@test "sync-pipe-atexit: fallback message on unexpected exit" {
    # Test that the atexit handler sends a fallback message when conmon
    # exits before writing to the sync pipe (without --exit-command).
    # This validates the basic atexit handler works.

    start_oci_sync_pipe_reader

    run_conmon \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$RUNTIME_BINARY" \
        --log-path "k8s-file:$LOG_PATH" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --syslog \
        --log-level trace \
        --container-pidfile "$PID_FILE" \
        --conmon-pidfile "$CONMON_PID_FILE" \
        6>"$OCI_SYNCPIPE_PATH"

    # Give conmon some time to run.
    sleep 1

    assert_file_exists "$CONMON_PID_FILE"
    CONMON_PID=$(cat "$CONMON_PID_FILE")
    wait "$CONMON_PID" 2>/dev/null || true

    # Sync pipe should have received error JSON from either the normal
    # error path or the atexit fallback handler.
    assert_file_exists "$TEST_TMPDIR"/syncpipe-output
    run cat "$TEST_TMPDIR"/syncpipe-output
    echo "syncpipe output: $output"
    assert "${output}" =~ '"pid": -1' "sync pipe should report error (pid: -1)"
    assert "${output}" =~ '"message":' "sync pipe should contain error message"
}
