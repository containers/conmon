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

# Helper function to run conmon with basic log options
run_conmon_with_log_opts() {
    local extra_args=("$@")
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" "${extra_args[@]}"
}

@test "ctr logs: no log driver should fail" {
    run_conmon_with_log_opts
    assert_failure
    assert_output_contains "Log driver not provided. Use --log-path"
}

@test "ctr logs: log driver as path should pass" {
    run_conmon_with_log_opts --log-path "$LOG_PATH"
    assert_success
    [ -f "$LOG_PATH" ]
}

@test "ctr logs: log driver as journald should pass" {
    run_conmon_with_log_opts --log-path "journald:"
    assert_success
}

@test "ctr logs: log driver as passthrough should pass" {
    run_conmon_with_log_opts --log-path "passthrough:"
    assert_success
}

@test "ctr logs: log driver as k8s-file with invalid path should fail" {
    run_conmon_with_log_opts --log-path "k8s-file:$INVALID_PATH"
    assert_failure
    assert_output_contains "Failed to open log file"
}

@test "ctr logs: log driver as invalid driver should fail" {
    local invalid_log_driver="invalid"
    run_conmon_with_log_opts --log-path "$invalid_log_driver:$LOG_PATH"
    assert_failure
    assert_output_contains "No such log driver $invalid_log_driver"
}

@test "ctr logs: multiple log drivers should pass" {
    run_conmon_with_log_opts --log-path "k8s-file:$LOG_PATH" --log-path "journald:"
    assert_success
    [ -f "$LOG_PATH" ]
}

@test "ctr logs: --no-container-partial-message option should pass with journald" {
    run_conmon_with_log_opts --log-path "journald:" --no-container-partial-message
    assert_success
}

@test "ctr logs: --no-container-partial-message should warn without journald" {
    run_conmon_with_log_opts --log-path "$LOG_PATH" --no-container-partial-message
    assert_success
    assert_output_contains "no effect without journald log driver"
}

@test "ctr logs: multiple log drivers with one invalid should fail" {
    local invalid_log_driver="invalid"
    run_conmon_with_log_opts --log-path "k8s-file:$LOG_PATH" --log-path "$invalid_log_driver:$LOG_PATH"
    assert_failure
    assert_output_contains "No such log driver $invalid_log_driver"
}

@test "ctr logs: journald with --log-label, no '=' in label" {
    start_conmon_with_default_args \
        --log-path "journald:" \
        --log-label "CONMON_TEST_LABEL1"

    assert_output_contains "Container labels must be in format LABEL=VALUE"
}

@test "ctr logs: journald with --log-label, multiple '=' in label" {
    start_conmon_with_default_args \
        --log-path "journald:" \
        --log-label "CONMON_TEST_LABEL1=FOO=$CTR_ID"

    assert_output_contains "Container labels must be in format LABEL=VALUE"
}

@test "ctr logs: journald with --log-label, no label name" {
    start_conmon_with_default_args \
        --log-path "journald:" \
        --log-label "=$CTR_ID"

    assert_output_contains "Container labels must be in format LABEL=VALUE"
}

@test "ctr logs: journald with --log-label, invalid character" {
    start_conmon_with_default_args \
        --log-path "journald:" \
        --log-label "MY%LABEL=$CTR_ID"

    assert_output_contains "Container label names must contain only uppercase letters, numbers and underscore"
}

@test "ctr logs: k8s-file with --log-label" {
    start_conmon_with_default_args \
        --log-path "k8s-file:$LOG_PATH" \
        --log-label "CONMON_TEST_LABEL1=$CTR_ID"

    assert_output_contains "k8s-file doesn't support --log-label"
}

@test "ctr logs: journald with --log-label" {
    run_conmon_with_default_args \
        --log-path "journald:" \
        --log-label "CONMON_TEST_LABEL1=$CTR_ID" \
        --log-label "CONMON_TEST_LABEL2=$CTR_ID"

    run journalctl --user CONMON_TEST_LABEL1="$CTR_ID"
    assert "${output}" =~ "hello from busybox"

    run journalctl --user CONMON_TEST_LABEL2="$CTR_ID"
    assert "${output}" =~ "hello from busybox"

    run journalctl --user NON_EXISTING_LABEL="$CTR_ID"
    assert "${output}" !~ "hello from busybox"
}

@test "ctr logs: k8s-file with --log-tag" {
    start_conmon_with_default_args \
        --log-path "k8s-file:$LOG_PATH" \
        --log-tag "CONMON_TEST_LABEL1"

    assert_output_contains "k8s-file doesn't support --log-tag"
}

@test "ctr logs: journald with --log-tag" {
    run_conmon_with_default_args \
        --log-path "journald:" \
        --log-tag "tag_$CTR_ID"

    run journalctl --user CONTAINER_TAG="tag_$CTR_ID"
    assert "${output}" =~ "hello from busybox"
}

@test "ctr logs: journald partial message" {
    # Print a message longer than the conmon buffer.
    # It should split it into multiple partial messages.
    setup_container_env "printf '%*s' "65535" | /busybox tr ' ' '#'"
    run_conmon_with_default_args \
        --log-path "journald:"

    run journalctl --user CONTAINER_ID_FULL="$CTR_ID" CONTAINER_PARTIAL_MESSAGE=true
    assert "${output}" =~ "######"
}

@test "ctr logs: k8s partial message" {
    # Print a message longer than the conmon buffer.
    # It should split it into multiple partial messages.
    setup_container_env "printf '%*s' "65535" | /busybox tr ' ' '#'"
    run_conmon_with_default_args \
        --log-path "k8s-file:$LOG_PATH"

    assert_file_exists "$LOG_PATH"
    run cat "$LOG_PATH"
    assert "${output}" =~ "stdout P"
    assert "${output}" =~ "stdout F"
}
