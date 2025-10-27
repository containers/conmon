#!/usr/bin/env bats

load test_helper

setup() {
    check_conmon_binary
    check_runtime_binary
    setup_container_env "while [ ! -f /tmp/test.txt ]; do sleep 0.1; done; stty size" "true"
    generate_process_spec "echo 'Hello from exec!' && echo 'Hello there!' > /tmp/test.txt"
}

teardown() {
    cleanup_test_env
}

# Helper function to start conmon, send the control command to it and terminate it.
test_ctl_command() {
    local command="$1"
    start_conmon_with_default_args --log-path "k8s-file:$LOG_PATH" -t
    wait_for_runtime_status "$CTR_ID" running

    echo "$command" > ${CTL_PATH}

    start_conmon_with_default_args \
        --log-path "k8s-file:$LOG_PATH.exec" \
        --exec \
        --exec-process-spec "${BUNDLE_PATH}/process.json"

    wait_for_runtime_status "$CTR_ID" stopped
}

# Helper function to send the resize command. Fails if the resize command
# triggers the tty resize.
test_resize_command_fail() {
    local command="$1"
    test_ctl_command "$command"

    # Check that the main process noticed the /tmp/test.txt.
    assert_file_exists "$LOG_PATH"
    run cat "$LOG_PATH"
    # No terminal resize - should show default/initial size (0 0)
    # When resize fails, terminal stays at initial size
    assert "${output}" =~ "0 0"
}

# Helper function to send the resize command. Fails if the resize command
# does not trigger the tty resize.
test_resize_command_ok() {
    local command="$1"
    local expected_size="$2"
    test_ctl_command "$command"

    # Check that the main process noticed the /tmp/test.txt.
    assert_file_exists "$LOG_PATH"
    run cat "$LOG_PATH"
    assert "${output}" =~ "$expected_size"
}

@test "ctrl: resize the terminal, negative width and height" {
    test_resize_command_fail "1 -1 -1"
}

@test "ctrl: resize the terminal, not enough variables" {
    test_resize_command_fail "1 2"
}

@test "ctrl: resize the terminal, no variables" {
    test_resize_command_fail "1"
}

@test "ctrl: resize the terminal, too many variables" {
    # This should probably fail, but it works and we want to stay
    # backward compatible.
    test_resize_command_ok "1 2 2 3" "2 2"
}

@test "ctrl: resize the terminal, too long line" {
    # Generate a very long line, longer than conmon's buffer.
    long_line=$(printf '%*s' "65535" | tr ' ' "#")
    test_resize_command_fail "1 2 2 $long_line"
}

@test "ctrl: resize the terminal" {
    test_resize_command_ok "1 2 2" "2 2"
}

@test "ctrl: rotate logs" {
    start_conmon_with_default_args --log-path "k8s-file:$LOG_PATH" -t
    wait_for_runtime_status "$CTR_ID" running

    # Remove the log.
    rm -f $LOG_PATH
    # The control message should reopen/recreate it.
    echo "2 1 1" > ${CTL_PATH}

    start_conmon_with_default_args \
        --log-path "k8s-file:$LOG_PATH.exec" \
        --exec \
        --exec-process-spec "${BUNDLE_PATH}/process.json"

    wait_for_runtime_status "$CTR_ID" stopped

    # Check that the log exists now.
    assert_file_exists "$LOG_PATH"
    run cat "$LOG_PATH"
    # No terminal resize - should show default/initial size (0 0)
    assert "${output}" =~ "0 0"
}

@test "ctrl: unknown message 'foo'" {
    test_resize_command_fail "foo"
}

@test "ctrl: unknown message '999'" {
    test_resize_command_fail "999"
}

@test "ctrl: unknown message '999 2 2'" {
    test_resize_command_fail "999 2 2"
}

@test "ctrl: resize with floating point dimensions" {
    test_resize_command_fail "1 10.5 20.3"
}

@test "ctrl: resize with hex numbers" {
    test_resize_command_fail "1 0x10 0x20"
}

@test "ctrl: resize overflow rejected" {
    # Test that values > 65535 are properly rejected
    # This prevents silent wraparound bugs (100000 -> 34464)
    test_resize_command_fail "1 100000 100000"
}

@test "ctrl: resize with leading zeros" {
    test_resize_command_ok "1 0010 0020" "10 20"
}

@test "ctrl: maximum valid size" {
    # Test with maximum valid practical terminal size (1000x1000)
    # This prevents potential DoS from applications allocating large screen buffers
    test_resize_command_ok "1 1000 1000" "1000 1000"
}

@test "ctrl: size exceeding maximum - height too large" {
    # Values above 1000 should be rejected
    test_resize_command_fail "1 1001 1000"
}

@test "ctrl: size exceeding maximum - width too large" {
    # Values above 1000 should be rejected
    test_resize_command_fail "1 1000 1001"
}

@test "ctrl: size exceeding maximum - both too large" {
    # Values above 1000 should be rejected
    test_resize_command_fail "1 65535 65535"
}

@test "ctrl: rotate logs with --log-rotate" {
    setup_container_env "echo 'before rotation'; while [ ! -f /tmp/test.txt ]; do sleep 0.1; done; echo 'after rotation'" "true"
    generate_process_spec "echo 'Hello from exec!' && echo 'Hello there!' > /tmp/test.txt"
    start_conmon_with_default_args \
        --log-path "k8s-file:$LOG_PATH" \
        -t \
        --log-rotate
    wait_for_runtime_status "$CTR_ID" running

    # The control message should rotate the log
    echo "2 1 1" > ${CTL_PATH}

    run_conmon_with_default_args \
        --log-path "k8s-file:$LOG_PATH.exec" \
        --exec \
        --exec-process-spec "${BUNDLE_PATH}/process.json"

    assert_file_exists "$LOG_PATH.exec"
    run cat "$LOG_PATH.exec"
    assert "${output}" =~ "Hello from exec!"

    assert_file_exists "$LOG_PATH.1"
    run cat "$LOG_PATH.1"
    assert "${output}" =~ "before rotation"

    assert_file_exists "$LOG_PATH"
    run cat "$LOG_PATH"
    assert "${output}" =~ "after rotation"
}
