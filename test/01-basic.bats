#!/usr/bin/env bats

load test_helper

setup() {
    check_conmon_binary
    setup_test_env
}

teardown() {
    cleanup_test_env
}

@test "conmon version" {
    run_conmon --version
    assert_success
    assert_output_contains "conmon version"
    assert_output_contains "commit"
}

@test "no container ID should fail" {
    run_conmon
    assert_failure
    assert_output_contains "Container ID not provided. Use --cid"
}

@test "no container UUID should fail" {
    run_conmon --cid "$CTR_ID"
    assert_failure
    assert_output_contains "Container UUID not provided. Use --cuuid"
}

@test "no runtime path should fail" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID"
    assert_failure
    assert_output_contains "Runtime path not provided. Use --runtime"
}

@test "invalid runtime path should fail" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$INVALID_PATH"
    assert_failure
    assert_output_contains "Runtime path $INVALID_PATH is not valid"
}

@test "no log driver should fail" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH"
    assert_failure
    assert_output_contains "Log driver not provided. Use --log-path"
}

@test "empty log driver should fail" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path ""
    assert_failure
    assert_output_contains "log-path must not be empty"
}

@test "empty log driver and path should fail" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path ":"
    assert_failure
    assert_output_contains "log-path must not be empty"
}

@test "k8s-file requires a filename" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "k8s-file"
    assert_failure
    assert_output_contains "k8s-file requires a filename"
}

@test "k8s-file: requires a filename" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "k8s-file:"
    assert_failure
    assert_output_contains "k8s-file requires a filename"
}

@test "log driver as path should pass" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "$LOG_PATH"
    assert_success
    [ -f "$LOG_PATH" ]
}

@test "log driver as k8s-file:path should pass" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "k8s-file:$LOG_PATH"
    assert_success
    [ -f "$LOG_PATH" ]
}

@test "log driver as :path should pass" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path ":$LOG_PATH"
    assert_success
    [ -f "$LOG_PATH" ]
}

@test "log driver as none should pass" {
    cd "$TEST_TMPDIR"
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "none:"
    assert_success
    [ ! -f "none" ]
}

@test "log driver as off should pass" {
    cd "$TEST_TMPDIR"
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "off:"
    assert_success
    [ ! -f "off" ]
}

@test "log driver as null should pass" {
    cd "$TEST_TMPDIR"
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "null:"
    assert_success
    [ ! -f "null" ]
}

@test "log driver as journald should pass" {
    cd "$TEST_TMPDIR"
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "journald:"
    assert_success
    [ ! -f "journald" ]
}

@test "log driver as :journald should pass" {
    cd "$TEST_TMPDIR"
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path ":journald"
    assert_success
    [ -f "journald" ]
}

@test "log driver as journald with short cid should fail" {
    local short_ctr_id="abcdefghijkl"
    run_conmon --cid "$short_ctr_id" --cuuid "$short_ctr_id" --runtime "$VALID_PATH" --log-path "journald:"
    assert_failure
    assert_output_contains "Container ID must be longer than 12 characters"
}

@test "log driver as k8s-file with path should pass" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "k8s-file:$LOG_PATH"
    assert_success
    [ -f "$LOG_PATH" ]
}

@test "log driver as k8s-file with invalid path should fail" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "k8s-file:$INVALID_PATH"
    assert_failure
    assert_output_contains "Failed to open log file"
}

@test "log driver as invalid driver should fail" {
    local invalid_log_driver="invalid"
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "$invalid_log_driver:$LOG_PATH"
    assert_failure
    assert_output_contains "No such log driver $invalid_log_driver"
}

@test "log driver as invalid driver with blank path should fail" {
    local invalid_log_driver="invalid"
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "$invalid_log_driver:"
    assert_failure
    assert_output_contains "No such log driver $invalid_log_driver"
}

@test "multiple log drivers should pass" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" \
        --log-path "k8s-file:$LOG_PATH" --log-path "journald:"
    assert_success
    [ -f "$LOG_PATH" ]
}

@test "multiple log drivers with one invalid should fail" {
    local invalid_log_driver="invalid"
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" \
        --log-path "k8s-file:$LOG_PATH" --log-path "$invalid_log_driver:$LOG_PATH"
    assert_failure
    assert_output_contains "No such log driver $invalid_log_driver"
}