#!/usr/bin/env bats

load test_helper

setup() {
    check_conmon_binary
    check_runtime_binary
    setup_container_env "cat && echo 'Container stopped!'"
}

teardown() {
    cleanup_test_env
}

@test "attach: attach to container and send string to stdin" {
    start_conmon_with_default_args --log-path "k8s-file:$LOG_PATH" --stdin
    wait_for_runtime_status "$CTR_ID" running
    echo "Hello there!" | socat STDIN "UNIX:${ATTACH_PATH},socktype=5"

    # Check that log file was created
    assert_file_exists "$LOG_PATH"
    run cat "$LOG_PATH"
    assert "${output}" =~ "Hello there!" "'Hello there!' found in the log"
    assert "${output}" =~ "Container stopped!" "'Container stopped!' found in the log"

    assert_file_not_exists "$ATTACH_PATH"
}

@test "attach: container finishes immediately without --stdin" {
    # Pipe is closed without --stdin, so `/cat` does not hang indefinitely, but finishes.
    start_conmon_with_default_args --log-path "k8s-file:$LOG_PATH"
    wait_for_runtime_status "$CTR_ID" stopped
    # The container being stopped does not mean conmon is done writing its
    # output to the log; wait for conmon to exit before reading the log.
    wait_for_conmon_exit "$CONMON_PID"

    # Check that log file was created
    assert_file_exists "$LOG_PATH"
    run cat "$LOG_PATH"
    assert "${output}" =~ "Container stopped!" "'Container stopped!' found in the log"
}

@test "attach: unix socket remains open with --leave-stdin-open" {
    start_conmon_with_default_args --log-path "k8s-file:$LOG_PATH" --stdin --leave-stdin-open
    wait_for_runtime_status "$CTR_ID" running
    echo "Hello there!" | socat STDIN "UNIX:${ATTACH_PATH},socktype=5"

    # Check that log file was created
    assert_file_exists "$LOG_PATH"
    run cat "$LOG_PATH"
    assert "${output}" =~ "Hello there!" "'Hello there!' found in the log"
    assert "${output}" !~ "Container stopped!" "'Container stopped!' not found in the log"

    assert_file_exists "$ATTACH_PATH"
    echo "Hello there again!" | socat STDIN "UNIX:${ATTACH_PATH},socktype=5"
    run cat "$LOG_PATH"
    assert "${output}" =~ "Hello there again!" "'Hello there again!' found in the log"
    assert "${output}" !~ "Container stopped!" "'Container stopped!' not found in the log"
}

@test "attach: stdin backpressure does not produce a warning" {
    skip_if_no_journald

    create_test_script "slow_stdin.sh" '#!/bin/sh
sleep 2
cat >/dev/null'
    update_container_args "/slow_stdin.sh"

    start_conmon_with_default_args --log-path "k8s-file:$LOG_PATH" --stdin
    wait_for_runtime_status "$CTR_ID" running

    # Keep the container stdin pipe full while its reader is sleeping.
    head -c 8388608 /dev/zero | socat -u STDIN "UNIX:${ATTACH_PATH},socktype=5"

    wait_for_runtime_status "$CTR_ID" stopped
    wait_for_conmon_exit "$CONMON_PID"
    sleep 1 # Give journald time to process the final messages.

    local journal_output
    journal_output=$(get_conmon_journal_output "$CONMON_PID")
    if [[ "$journal_output" == *"Failed to write to fd container stdin"* ]]; then
        echo "Unexpected stdin write warning in journal:"
        echo "$journal_output"
        return 1
    fi
}
