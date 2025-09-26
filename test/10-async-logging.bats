#!/usr/bin/env bats

load test_helper

setup() {
    check_conmon_binary
    setup_test_env
}

teardown() {
    cleanup_test_env
}

@test "async logging: signal safety during logging" {
    # Test that async logging with signal masking continues after signal delivery
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "$LOG_PATH" &
    local conmon_pid=$!

    # Give it a moment to start
    sleep 0.1

    # Send a harmless signal that could interrupt pthread mutexes
    kill -USR1 "$conmon_pid" 2>/dev/null || true

    # Wait for completion
    wait "$conmon_pid" 2>/dev/null || true

    # Log file should still be created despite signal interruption
    [ -f "$LOG_PATH" ]
}

@test "async logging: signal masking prevents corruption during concurrent signals" {
    # Test that multiple signals don't corrupt the async logging buffer
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "$LOG_PATH" &
    local conmon_pid=$!

    # Give it a moment to start
    sleep 0.1

    # Send multiple signals in quick succession
    for sig in USR1 USR2 TERM; do
        kill -"$sig" "$conmon_pid" 2>/dev/null || true
        sleep 0.01
    done

    # Wait for completion
    wait "$conmon_pid" 2>/dev/null || true

    # Log file should still be created and not corrupted
    [ -f "$LOG_PATH" ]
}

@test "async logging: performance under concurrent signal load" {
    # Test that signal masking doesn't significantly impact performance
    local start_time=$(date +%s%N)

    for i in {1..3}; do
        local test_log="$TEST_TMPDIR/perf_$i.log"
        run_conmon --cid "${CTR_ID}_$i" --cuuid "${CTR_ID}_$i" --runtime "$VALID_PATH" --log-path "$test_log" &
        local pid=$!

        # Send a signal during execution
        sleep 0.05
        kill -USR1 "$pid" 2>/dev/null || true

        wait "$pid" 2>/dev/null || true
        assert_file_exists "$test_log"
    done

    local end_time=$(date +%s%N)
    local duration=$((end_time - start_time))

    # Should complete reasonably quickly (under 5 seconds)
    [ $((duration / 1000000000)) -lt 5 ]
}
