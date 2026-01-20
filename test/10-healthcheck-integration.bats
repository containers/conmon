#!/usr/bin/env bats

load test_helper

setup() {
    check_conmon_binary
    check_runtime_binary
}

# Cleanup function for conmon and container
cleanup_test_resources() {
    local conmon_pid="$1"
    local ctr_id="$2"

    echo "Cleaning up test resources..."

    # Check if conmon is still running
    if kill -0 "$conmon_pid" 2>/dev/null; then
        echo "Conmon is still running, killing process..."
        kill "$conmon_pid" 2>/dev/null || true
        wait "$conmon_pid" 2>/dev/null || true
        echo "Conmon terminated"
    else
        echo "Conmon exited on its own"
    fi

    # Clean up container
    if [[ -n "$ctr_id" ]]; then
        echo "Cleaning up container $ctr_id"
        "$RUNTIME_BINARY" delete -f "$ctr_id" 2>/dev/null || true
    fi
    cleanup_tmpdir

    echo "Test cleanup completed"
}


@test "healthcheck timeout enforcement integration" {
    setup_container_env "/busybox sleep 1000"

    # Define healthcheck timeout (must match --healthcheck-timeout)
    local healthcheck_timeout=2

    # Create a slow healthcheck script that will timeout
    cat > "$ROOTFS/bin/slow_healthcheck" << 'EOF'
#!/bin/sh
echo "Starting slow healthcheck..."
sleep 10
echo "Slow healthcheck completed"
exit 0
EOF
    chmod +x "$ROOTFS/bin/slow_healthcheck"

    echo "Starting conmon with healthcheck timeout test..."
    echo "Container ID: $CTR_ID"
    echo "Healthcheck will timeout after ${healthcheck_timeout} seconds (script sleeps for 10 seconds)"

    # Run conmon in background and manually control the timeout
    echo "Starting conmon in background..."
    local conmon_pid=$(start_conmon_healthcheck "/busybox" 9 "$healthcheck_timeout" 1 0 "$LOG_PATH" "trace" sleep 10)
    echo "Conmon started with PID: $conmon_pid"

    echo "Waiting for healthcheck timeout (max 15 seconds)..."
    sleep 15

    # Get final journal output with retry for log buffering
    journal_output=$(get_conmon_journal_output "$conmon_pid")

    # Clean up
    CONMON_PROCESS_DETAILS=$(ps -ef | grep "$conmon_pid")
    # Should find timeout message
    if echo "$journal_output" | grep -q "Healthcheck command timed out after ${healthcheck_timeout} seconds"; then
        echo "✅ Found timeout message in journal output!"
        cleanup_test_resources "$conmon_pid" "$CTR_ID"
        true
    else
        echo "❌ Timeout message not found"
        echo "Journal output:"
        echo "$journal_output"
        echo "Conmon process details:"
        echo "$CONMON_PROCESS_DETAILS"
        cleanup_test_resources "$conmon_pid" "$CTR_ID"
        false
    fi
}

@test "healthcheck start_period timing validation" {
    setup_container_env "/busybox sleep 1000"

    # Define healthcheck parameters
    local healthcheck_interval=2
    local healthcheck_start_period=5
    local healthcheck_timeout=1

    echo "Starting conmon with start_period timing test..."
    echo "Container ID: $CTR_ID"
    echo "Healthcheck interval: ${healthcheck_interval}s, start_period: ${healthcheck_start_period}s"
    echo "Expected behavior: healthcheck failures ignored until start_period (5s), then failures count"

    # Run conmon in background
    echo "Starting conmon in background..."
    local conmon_pid=$(start_conmon_healthcheck "/busybox" "$healthcheck_interval" "$healthcheck_timeout" 0 "$healthcheck_start_period" "" "" false)
    echo "Conmon started with PID: $conmon_pid"

    # Find the actual conmon PID (may be forked)
    local actual_conmon_pid=$(find_conmon_forked_pid "$conmon_pid" "$CTR_ID")
    echo "Using conmon PID: $actual_conmon_pid"


    # Wait for healthcheck execution and start_period behavior
    echo "Waiting 10 seconds for healthcheck execution and start_period behavior..."
    sleep 10

    # Expected timeline:
    # 0s: healthcheck fails but ignored (start_period not reached)
    # 2s: healthcheck fails but ignored (start_period not reached)
    # 4s: healthcheck fails but ignored (start_period not reached)
    # 5s: start_period over
    # 6s: healthcheck fails and counts as failure
    # 7s: done.

    # Get final journal output with retry for log buffering
    journal_output=$(get_conmon_journal_output "$actual_conmon_pid")

    # Clean up
    cleanup_test_resources "$conmon_pid" "$CTR_ID"

    # Validate start_period behavior
    if [[ -n "$journal_output" ]]; then
        local ignored_count=$(echo "$journal_output" | grep -c "Healthcheck failure ignored during start period" 2>/dev/null | tr -d '\n' || echo "0")
        local counts_count=$(echo "$journal_output" | grep -c "Healthcheck failure counts after start period" 2>/dev/null | tr -d '\n' || echo "0")

        # Print the counts for visibility
        echo "Healthcheck counts:"
        echo "  - Ignored during start_period: $ignored_count"
        echo "  - Counted after start_period: $counts_count"

        # Validate that we have proper start_period behavior
        # Expected: at least 2 ignored during start_period, at least 1 counted after start_period
        # Note: Timing may vary between local and CI environments
        if [[ $ignored_count -ge 2 && $counts_count -ge 1 ]]; then
            echo "✅ Healthcheck start_period behavior validated"
            true
        else
            echo "❌ Insufficient start_period behavior"
            false
        fi
    else
        echo "❌ No journal output found"
        false
    fi

}

@test "healthcheck command execution failure - non-existent command" {
    # Test healthcheck with non-existent command
    setup_container_env "/busybox sleep 10"

    echo "Testing healthcheck with non-existent command..."
    local conmon_pid=$(start_conmon_healthcheck "/nonexistent/command" 2 1 1 0 "$LOG_PATH" "")
    sleep 6

    local actual_conmon_pid=$(find_conmon_forked_pid "$conmon_pid" "$CTR_ID")
    local journal_output=$(get_conmon_journal_output "$actual_conmon_pid")

    # Clean up
    cleanup_test_resources "$conmon_pid" "$CTR_ID"

    # Should find command execution failure
    [[ "$journal_output" == *"Healthcheck command failed"* ]] || [[ "$journal_output" == *"Failed to execute healthcheck command"* ]]
}

@test "healthcheck command execution failure - command with stderr output" {
    # Test healthcheck with command that outputs to stderr
    setup_container_env "/busybox sleep 10"

    echo "Testing healthcheck with command that outputs to stderr..."
    local conmon_pid=$(start_conmon_healthcheck "/busybox" 2 1 1 0 "$LOG_PATH" "" sh -c "echo 'This is an error message' >&2; exit 1")
    sleep 6

    local actual_conmon_pid=$(find_conmon_forked_pid "$conmon_pid" "$CTR_ID")
    local journal_output=$(get_conmon_journal_output "$actual_conmon_pid")

    # Clean up
    cleanup_test_resources "$conmon_pid" "$CTR_ID"

    # Should find stderr output in logs
    [[ "$journal_output" == *"Healthcheck command stderr: This is an error message"* ]]
}

@test "healthcheck command timeout handling" {
    # Test healthcheck command timeout
    setup_container_env "/busybox sleep 10"

    echo "Testing healthcheck command timeout..."
    local conmon_pid=$(start_conmon_healthcheck "/busybox" 5 2 1 0 "$LOG_PATH" "" sleep 10)
    sleep 6

    local journal_output=$(get_conmon_journal_output "$conmon_pid")

    # Clean up
    cleanup_test_resources "$conmon_pid" "$CTR_ID"

    # Should find timeout message
    [[ "$journal_output" == *"Healthcheck command timed out after 2 seconds"* ]]
}

@test "healthcheck command signal termination" {
    # Test healthcheck command terminated by external signal
    setup_container_env "/busybox sleep 10"

    echo "Testing healthcheck command signal termination..."
    local conmon_pid=$(start_conmon_healthcheck "/busybox" 35 30 1 0 "$LOG_PATH" "" sleep 30)
    sleep 5  # Wait for healthcheck to start

    local actual_conmon_pid=$(find_conmon_forked_pid "$conmon_pid" "$CTR_ID")

    # Find the healthcheck process and send SIGTERM to it
    echo "Looking for healthcheck processes..."
    ps aux | grep "sleep 30" | grep -v grep
    local healthcheck_pid=$(pgrep -f "sleep 30" | head -1)
    if [[ -n "$healthcheck_pid" ]]; then
        echo "Found healthcheck process PID: $healthcheck_pid, sending SIGKILL..."
        kill -SIGKILL "$healthcheck_pid"
        sleep 3  # Wait for signal to be processed
    else
        echo "No healthcheck process found!"
    fi

    local journal_output=$(get_conmon_journal_output "$actual_conmon_pid")

    # Debug: show what we actually got
    echo "Journal output:"
    echo "$journal_output"

    # Clean up
    cleanup_test_resources "$conmon_pid" "$CTR_ID"

    # Should find signal termination message (SIGKILL = signal 9)
    # SIGKILL cannot be caught, so it should trigger WIFSIGNALED
    [[ "$journal_output" == *"Healthcheck command terminated by signal 9"* ]]
}

@test "healthcheck with maximum retries exceeded" {
    # Test healthcheck with retries exceeded
    setup_container_env "/busybox sleep 10"

    echo "Testing healthcheck with retries exceeded..."
    local conmon_pid=$(start_conmon_healthcheck "/busybox" 1 1 2 0 "$LOG_PATH" "" false)
    sleep 5

    local actual_conmon_pid=$(find_conmon_forked_pid "$conmon_pid" "$CTR_ID")
    local journal_output=$(get_conmon_journal_output "$actual_conmon_pid")

    # Clean up
    cleanup_test_resources "$conmon_pid" "$CTR_ID"

    # Should find multiple failure messages and eventually unhealthy status
    local failure_count=$(echo "$journal_output" | grep -c "Healthcheck command failed" 2>/dev/null || echo "0")
    [[ $failure_count -ge 2 ]]
}

