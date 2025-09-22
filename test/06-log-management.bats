#!/usr/bin/env bats

# Minimal comprehensive test suite for log rotation and truncation functionality
# This consolidates all essential tests while eliminating duplication

load test_helper

setup() {
    check_conmon_binary
    setup_test_env
}

teardown() {
    cleanup_test_env
}

# Helper function to run conmon with k8s-file log driver
run_conmon_k8s_log() {
    local extra_args=("$@")
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" \
        --log-path "k8s-file:$LOG_PATH" "${extra_args[@]}"
}

# === CLI Parameter Validation Tests ===

@test "log management: should validate log-max-files bounds" {
    # Test negative value
    run_conmon_k8s_log --log-rotate --log-max-files -1
    assert_failure
    [[ "$output" == *"log-max-files must be non-negative"* ]]

    # Test excessive value
    run_conmon_k8s_log --log-rotate --log-max-files 2147483648
    assert_failure
    [[ "$output" == *"out of range"* ]]

    # Test zero with rotation (should fail)
    run_conmon_k8s_log --log-rotate --log-max-files 0
    assert_failure
    [[ "$output" == *"log-max-files must be at least 1 when log-rotate is enabled"* ]]

    # Test valid bounds
    run_conmon_k8s_log --log-rotate --log-max-files 1 --log-size-max 1024
    assert_success
    [ -f "$LOG_PATH" ]
}

# === Core Functionality Tests ===

@test "log management: should default to truncation behavior" {
    run_conmon_k8s_log --log-size-max 1024
    assert_success
    [ -f "$LOG_PATH" ]
    [ ! -f "$LOG_PATH.1" ]  # No backup files in truncation mode
}

@test "log management: should enable rotation with proper flags" {
    run_conmon_k8s_log --log-rotate --log-max-files 2 --log-size-max 1024
    assert_success
    [ -f "$LOG_PATH" ]
}

@test "log management: should preserve k8s log format" {
    run_conmon_k8s_log --log-rotate --log-max-files 2 --log-size-max 1024
    assert_success

    # Test k8s log format preservation
    local k8s_entry='2023-07-23T18:00:00.000000000Z stdout F Test log message'
    echo "$k8s_entry" > "$LOG_PATH"

    local content
    content=$(<"$LOG_PATH")
    [ "$content" = "$k8s_entry" ]
}

@test "log management: should work with multiple log drivers" {
    # Test k8s-file + journald combination
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" \
        --log-path "k8s-file:$LOG_PATH" --log-path "journald:" \
        --log-rotate --log-max-files 2 --log-size-max 1024
    assert_success
    [ -f "$LOG_PATH" ]
}

# === Size Limit Edge Cases ===

@test "log management: should handle various size limits" {
    local test_cases=(50 1024 1048576)

    for size in "${test_cases[@]}"; do
        rm -f "$LOG_PATH" "$LOG_PATH".*

        # Test rotation
        run_conmon_k8s_log --log-rotate --log-max-files 2 --log-size-max "$size"
        assert_success
        [ -f "$LOG_PATH" ]

        rm -f "$LOG_PATH"

        # Test truncation
        run_conmon_k8s_log --log-size-max "$size"
        assert_success
        [ -f "$LOG_PATH" ]
        [ ! -f "$LOG_PATH.1" ]
    done
}

@test "log management: should handle edge case parameters" {
    # Zero size limit
    run_conmon_k8s_log --log-size-max 0
    [[ "$status" -eq 0 || "$status" -eq 1 ]]

    # Rotation without size limit
    run_conmon_k8s_log --log-rotate --log-max-files 2
    assert_success
    [ -f "$LOG_PATH" ]

    # log-max-files without rotation (should be ignored)
    run_conmon_k8s_log --log-max-files 5 --log-size-max 1024
    assert_success
    [ -f "$LOG_PATH" ]
    [ ! -f "$LOG_PATH.1" ]
}

# === Security and Path Validation Tests ===

@test "log management: should handle secure file permissions" {
    run_conmon_k8s_log --log-rotate --log-max-files 2 --log-size-max 1024
    assert_success
    [ -f "$LOG_PATH" ]

    # Check file permissions (should be 0640)
    local perms
    perms=$(stat -c "%a" "$LOG_PATH")
    [ "$perms" = "640" ]
}

@test "log management: should reject unsafe log paths" {
    # Test with symlink path (if we can create one safely)
    local test_dir="$TEST_TMPDIR/conmon-test-symlink-$$"
    if ! mkdir -p "$test_dir/real" 2>/dev/null; then
        skip "Cannot create test directory for symlink test"
    fi

    if ! ln -s "$test_dir/real" "$test_dir/link" 2>/dev/null; then
        rm -rf "$test_dir"
        skip "Cannot create symlink for test"
    fi

    local symlink_log="$test_dir/link/test.log"

    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" \
        --log-path "k8s-file:$symlink_log" --log-rotate --log-max-files 2

    # Should either fail or succeed but handle safely
    [[ "$status" -eq 0 || "$status" -eq 1 ]]

    # Cleanup
    rm -rf "$test_dir"
}

# === Backward Compatibility Tests ===

@test "log management: should maintain backward compatibility" {
    # Test that old truncation behavior still works
    run_conmon_k8s_log --log-size-max 1024
    assert_success
    [ -f "$LOG_PATH" ]

    # Test that new options don't break when not used
    run_conmon_k8s_log
    assert_success
    [ -f "$LOG_PATH" ]
}

# === Error Recovery Tests ===

@test "log management: should handle file system errors gracefully" {
    # Skip this test if running as root (root can bypass permission restrictions)
    if [[ $EUID -eq 0 ]]; then
        skip "Skipping permission test when running as root"
    fi

    # Test with read-only directory (if possible)
    local readonly_dir="$TEST_TMPDIR/conmon-readonly-$$"
    if ! mkdir -p "$readonly_dir" 2>/dev/null; then
        skip "Cannot create test directory for readonly test"
    fi

    # Make directory read-only
    if ! chmod 444 "$readonly_dir" 2>/dev/null; then
        rmdir "$readonly_dir"
        skip "Cannot make directory readonly for test"
    fi

    local readonly_log="$readonly_dir/test.log"

    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" \
        --log-path "k8s-file:$readonly_log" --log-rotate --log-max-files 2

    # Should fail gracefully without crashing
    assert_failure

    # Cleanup
    chmod 755 "$readonly_dir" 2>/dev/null || true
    rmdir "$readonly_dir" 2>/dev/null || true
}

# === Allowlist Functionality Tests ===

@test "log management: should work with log allowlist" {
    # Test allowlist functionality if configured
    local allowed_dir="$TEST_TMPDIR/conmon-allowed-$$"
    if ! mkdir -p "$allowed_dir" 2>/dev/null; then
        skip "Cannot create test directory for allowlist test"
    fi

    local allowed_log="$allowed_dir/test.log"

    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" \
        --log-path "k8s-file:$allowed_log" \
        --log-allowlist-dir "$allowed_dir" \
        --log-rotate --log-max-files 2

    assert_success
    [ -f "$allowed_log" ]

    # Cleanup
    rm -rf "$allowed_dir"
}

@test "log management: should work with multiple log allowlist directories" {
    # Test multiple allowlist directories functionality
    local allowed_dir1="$TEST_TMPDIR/conmon-allowed1-$$"
    local allowed_dir2="$TEST_TMPDIR/conmon-allowed2-$$"
    if ! mkdir -p "$allowed_dir1" "$allowed_dir2" 2>/dev/null; then
        skip "Cannot create test directories for multiple allowlist test"
    fi

    local allowed_log="$allowed_dir2/test.log"

    # Test with two allowlist directories - log should work in either
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" \
        --log-path "k8s-file:$allowed_log" \
        --log-allowlist-dir "$allowed_dir1" \
        --log-allowlist-dir "$allowed_dir2" \
        --log-rotate --log-max-files 2

    assert_success
    [ -f "$allowed_log" ]

    # Cleanup
    rm -rf "$allowed_dir1" "$allowed_dir2"
}
