#!/usr/bin/env bats

load test_helper

setup() {
    check_conmon_binary
    setup_test_env
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

@test "ctr logs: multiple log drivers with one invalid should fail" {
    local invalid_log_driver="invalid"
    run_conmon_with_log_opts --log-path "k8s-file:$LOG_PATH" --log-path "$invalid_log_driver:$LOG_PATH"
    assert_failure
    assert_output_contains "No such log driver $invalid_log_driver"
}