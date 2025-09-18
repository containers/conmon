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
    assert "${output}" =~ "hello from busybox"  "'hello from busybox' found in the log"
}

@test "runtime: container execution with different log drivers" {
    # Test with journald log driver
    run_conmon_with_default_args --log-path "journald:"

    run journalctl --user CONTAINER_ID_FULL="$CTR_ID"
    assert "${output}" =~ "hello from busybox"  "'hello from busybox' found in the journald"
}

@test "runtime: container execution with multiple log drivers" {
    # Test with both k8s-file and journald log drivers
    run_conmon_with_default_args --log-path "k8s-file:$LOG_PATH" --log-path "journald:"

    # Check that log file was created
    [ -f "$LOG_PATH" ]

    run cat "$LOG_PATH"
    assert "${output}" =~ "hello from busybox"  "'hello from busybox' found in the log"

    run journalctl --user CONTAINER_ID_FULL="$CTR_ID"
    assert "${output}" =~ "hello from busybox"  "'hello from busybox' found in the journald"
}

@test "runtime: container with log size limit" {
    # Test container execution with log rotation
    # This effectively keeps just the last line at max.
    local log_size_max=10

    run_conmon_with_default_args --log-path "k8s-file:$LOG_PATH" --log-size-max "$log_size_max"

    # Check that log file was created
    [ -f "$LOG_PATH" ]

    run cat "$LOG_PATH"
    assert "${output}" !~ "hello from busybox 11"  "'hello from busybox 11' not in the logs"
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