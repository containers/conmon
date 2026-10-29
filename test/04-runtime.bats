#!/usr/bin/env bats

load test_helper

setup() {
    check_conmon_binary
    check_runtime_binary
    setup_container_env
}

teardown() {
    cleanup_test_env
}

@test "runtime: simple runtime test" {
    run_conmon_with_default_args --log-path "k8s-file:$LOG_PATH"

    # Check that log file was created
    [ -f "$LOG_PATH" ]
    run cat "$LOG_PATH"
    assert "${output}" =~ "hello from ubi10" "'hello from ubi10' found in the log"
}

@test "runtime: container execution with different log drivers" {
    # Test with journald log driver
    run_conmon_with_default_args --log-path "journald:"

    run journalctl --user CONTAINER_ID_FULL="$CTR_ID"
    assert "${output}" =~ "hello from ubi10" "'hello from ubi10' found in the journald"
}

@test "runtime: container execution with multiple log drivers" {
    # Test with both k8s-file and journald log drivers
    run_conmon_with_default_args --log-path "k8s-file:$LOG_PATH" --log-path "journald:"

    # Check that log file was created
    [ -f "$LOG_PATH" ]

    run cat "$LOG_PATH"
    assert "${output}" =~ "hello from ubi10" "'hello from ubi10' found in the log"

    run journalctl --user CONTAINER_ID_FULL="$CTR_ID"
    assert "${output}" =~ "hello from ubi10" "'hello from ubi10' found in the journald"
}

@test "runtime: container with log size limit" {
    # Test container execution with log rotation
    # This effectively keeps just the last line at max.
    local log_size_max=10

    run_conmon_with_default_args --log-path "k8s-file:$LOG_PATH" --log-size-max "$log_size_max"

    # Check that log file was created
    [ -f "$LOG_PATH" ]

    run cat "$LOG_PATH"
    assert "${output}" !~ "hello from ubi10 11" "'hello from ubi10 11' not in the logs"
}

@test "runtime: invalid runtime binary should fail" {
    # Test with non-existent runtime binary
    run_conmon \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "/nonexistent/runtime" \
        --log-path "k8s-file:$LOG_PATH" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --container-pidfile "$PID_FILE" \
        --conmon-pidfile "$CONMON_PID_FILE"

    assert_failure
}

@test "runtime: configuration validation works" {
    # Test that conmon can validate its configuration
    # This is a basic smoke test for the runtime integration
    run_conmon --version
    assert_success
    assert_output_contains "conmon version"
}

@test "runtime: simple test with _OCI_SYNCPIPE defined" {
    start_oci_sync_pipe_reader
    run_conmon_with_default_args \
        --log-path "k8s-file:$LOG_PATH" 6>"$OCI_SYNCPIPE_PATH"

    # Check that the pid is sent to the sync pipe.
    wait_for_syncpipe_output 1
    run cat "$TEST_TMPDIR/syncpipe-output"
    CONTAINER_PID=$(cat "$CONTAINER_PIDFILE")
    assert_json "${output}" =~ "\"pid\": $CONTAINER_PID"
}

# Print the files conmon has open.
conmon_open_files() {
    local fd
    for fd in /proc/"$CONMON_PID"/fd/*; do
        readlink "$fd"
    done
}

@test "runtime: LISTEN_FDS for another process are not counted" {
    # Like podman with socket activation, which passes the LISTEN_FDS
    # sockets via --preserve-fds, and also leaves LISTEN_* (with its own
    # pid) in the environment. The runtime only gets --preserve-fds, so
    # conmon must only close those, and keep fd 5 and its sync pipe.
    setup_container_env "sleep 30"
    touch "$TEST_TMPDIR"/{preserved1,preserved2,other}
    start_oci_sync_pipe_reader
    LISTEN_FDS=2 LISTEN_PID=1 start_conmon_with_default_args \
        --runtime-opt --preserve-fds=2 \
        --log-path "k8s-file:$LOG_PATH" \
        3<"$TEST_TMPDIR/preserved1" 4<"$TEST_TMPDIR/preserved2" \
        5<"$TEST_TMPDIR/other" 6>"$OCI_SYNCPIPE_PATH"

    # Check that the pid is sent to the sync pipe.
    wait_for_syncpipe_output 1
    run cat "$TEST_TMPDIR/syncpipe-output"
    CONTAINER_PID=$(cat "$CONTAINER_PIDFILE")
    assert_json "${output}" =~ "\"pid\": $CONTAINER_PID"

    run conmon_open_files
    assert "$output" !~ "preserved" "conmon closes the fds passed to the runtime"
    assert "$output" =~ "$TEST_TMPDIR/other" "conmon keeps other fds"
}

@test "runtime: LISTEN_FDS with --replace-listen-pid are closed" {
    # With --replace-listen-pid, the runtime gets LISTEN_FDS (fds 3 and 4)
    # followed by --preserve-fds (fd 5), so conmon must close all these,
    # and keep fd 6 and its sync pipe.
    setup_container_env "sleep 30"
    touch "$TEST_TMPDIR"/{listen1,listen2,preserved,other}
    _start_pipe_reader "$OCI_SYNCPIPE_PATH" "_OCI_SYNCPIPE" 7 "$TEST_TMPDIR/syncpipe-output"
    LISTEN_FDS=2 LISTEN_PID=1 start_conmon_with_default_args \
        --replace-listen-pid --runtime-opt --preserve-fds=1 \
        --log-path "k8s-file:$LOG_PATH" \
        3<"$TEST_TMPDIR/listen1" 4<"$TEST_TMPDIR/listen2" \
        5<"$TEST_TMPDIR/preserved" 6<"$TEST_TMPDIR/other" \
        7>"$OCI_SYNCPIPE_PATH"

    # Check that the pid is sent to the sync pipe.
    wait_for_syncpipe_output 1
    run cat "$TEST_TMPDIR/syncpipe-output"
    CONTAINER_PID=$(cat "$CONTAINER_PIDFILE")
    assert_json "${output}" =~ "\"pid\": $CONTAINER_PID"

    run conmon_open_files
    assert "$output" !~ "listen|preserved" "conmon closes the fds passed to the runtime"
    assert "$output" =~ "$TEST_TMPDIR/other" "conmon keeps other fds"
}

@test "runtime: runc error with _OCI_SYNCPIPE defined" {
    if [[ $(basename "$RUNTIME_BINARY") != "runc" ]]; then
        skip "test requires runc"
    fi
    # This trailing " results in wrong config.json. We expect the runtime
    # failure.
    setup_container_env '"'
    start_oci_sync_pipe_reader
    run_conmon \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$RUNTIME_BINARY" \
        --log-path "k8s-file:$LOG_PATH" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --syslog \
        --container-pidfile "$PID_FILE" \
        --conmon-pidfile "$CONMON_PID_FILE" 6>"$OCI_SYNCPIPE_PATH"

    # Give conmon some time to run the runtime and fail.
    sleep 1

    assert_file_exists "$CONMON_PID_FILE"
    CONMON_PID=$(cat "$CONMON_PID_FILE")
    wait_for_conmon_exit "$CONMON_PID"

    # Check that the error is sent to the sync pipe.
    wait_for_syncpipe_output 1
    run cat "$TEST_TMPDIR/syncpipe-output"
    assert_json "${output}" =~ "\"pid\": -1"
    assert_json "${output}" =~ "\"message\":"
    assert_json "${output}" =~ "runc create failed"
}
