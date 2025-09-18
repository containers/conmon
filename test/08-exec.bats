#!/usr/bin/env bats

load test_helper

setup() {
    check_conmon_binary
    check_runtime_binary
    setup_container_env "while [ ! -f /tmp/test.txt ]; do /busybox sleep 0.1; done; /busybox cat /tmp/test.txt"
}

teardown() {
    cleanup_test_env
}

@test "exec: simple --exec --exec-process-spec" {
    generate_process_spec "echo 'Hello from exec!' && /busybox echo 'Hello there!' > /tmp/test.txt"
    start_conmon_with_default_args --log-path "k8s-file:$LOG_PATH"
    wait_for_runtime_status "$CTR_ID" running

    timeout 2s "$CONMON_BINARY" \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$RUNTIME_BINARY" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --log-level trace \
        --container-pidfile "$PID_FILE" \
        --syslog \
        --conmon-pidfile "$CONMON_PID_FILE" \
        --log-path "k8s-file:$LOG_PATH.exec" \
        --exec \
        --exec-process-spec "${BUNDLE_PATH}/process.json"

    wait_for_runtime_status "$CTR_ID" stopped

    # Check that the main process noticed the /tmp/test.txt.
    assert_file_exists "$LOG_PATH"
    run cat "$LOG_PATH"
    assert "${output}" =~ "Hello there!"  "'Hello there!' found in the log"

    # Check that the exec process output is stored in the log.
    assert_file_exists "$LOG_PATH.exec"
    run cat "$LOG_PATH.exec"
    assert "${output}" =~ "Hello from exec!"  "'Hello from exec!' found in the log"
}
