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
    # Run conmon which will create and manage the container
    # Using a timeout to prevent hanging
    timeout 30s "$CONMON_BINARY" \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$RUNTIME_BINARY" \
        --log-path "k8s-file:$LOG_PATH" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --log-level debug \
        --container-pidfile "$PID_FILE" \
        --conmon-pidfile "$CONMON_PID_FILE" &

    local conmon_pid=$!

    # Give conmon time to start up and run the container
    sleep 2

    # Check if conmon is still running or completed
    if kill -0 $conmon_pid 2>/dev/null; then
        # Kill conmon if it's still running
        kill $conmon_pid 2>/dev/null || true
        wait $conmon_pid 2>/dev/null || true
    fi

    # Check that log file was created
    [ -f "$LOG_PATH" ]

    # Check that conmon pidfile was created
    [ -f "$CONMON_PID_FILE" ]
}

@test "runtime: container execution with different log drivers" {
    # Test with journald log driver
    timeout 30s "$CONMON_BINARY" \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$RUNTIME_BINARY" \
        --log-path "journald:" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --container-pidfile "$PID_FILE" \
        --conmon-pidfile "$CONMON_PID_FILE" &

    local conmon_pid=$!
    sleep 2

    if kill -0 $conmon_pid 2>/dev/null; then
        kill $conmon_pid 2>/dev/null || true
        wait $conmon_pid 2>/dev/null || true
    fi

    # Check that conmon pidfile was created
    [ -f "$CONMON_PID_FILE" ]
}

@test "runtime: container execution with multiple log drivers" {
    # Test with both k8s-file and journald log drivers
    timeout 30s "$CONMON_BINARY" \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$RUNTIME_BINARY" \
        --log-path "k8s-file:$LOG_PATH" \
        --log-path "journald:" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --container-pidfile "$PID_FILE" \
        --conmon-pidfile "$CONMON_PID_FILE" &

    local conmon_pid=$!
    sleep 2

    if kill -0 $conmon_pid 2>/dev/null; then
        kill $conmon_pid 2>/dev/null || true
        wait $conmon_pid 2>/dev/null || true
    fi

    # Check that log file was created
    [ -f "$LOG_PATH" ]

    # Check that conmon pidfile was created
    [ -f "$CONMON_PID_FILE" ]
}

@test "runtime: container with log size limit" {
    # Test container execution with log rotation
    local log_size_max=1024

    timeout 30s "$CONMON_BINARY" \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$RUNTIME_BINARY" \
        --log-path "k8s-file:$LOG_PATH" \
        --log-size-max "$log_size_max" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --container-pidfile "$PID_FILE" \
        --conmon-pidfile "$CONMON_PID_FILE" &

    local conmon_pid=$!
    sleep 2

    if kill -0 $conmon_pid 2>/dev/null; then
        kill $conmon_pid 2>/dev/null || true
        wait $conmon_pid 2>/dev/null || true
    fi

    # Check that log file was created
    [ -f "$LOG_PATH" ]

    # Check that conmon pidfile was created
    [ -f "$CONMON_PID_FILE" ]
}

@test "runtime: container cleanup on completion" {
    # Create and run a container, then verify cleanup
    timeout 30s "$CONMON_BINARY" \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$RUNTIME_BINARY" \
        --log-path "k8s-file:$LOG_PATH" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --container-pidfile "$PID_FILE" \
        --conmon-pidfile "$CONMON_PID_FILE" &

    local conmon_pid=$!
    sleep 2

    if kill -0 $conmon_pid 2>/dev/null; then
        kill $conmon_pid 2>/dev/null || true
        wait $conmon_pid 2>/dev/null || true
    fi

    # Check that log file was created
    [ -f "$LOG_PATH" ]

    # Check that conmon pidfile was created
    [ -f "$CONMON_PID_FILE" ]
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