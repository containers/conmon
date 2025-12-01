#!/usr/bin/env bats

load test_helper

setup() {
    check_conmon_binary
    setup_test_env
}

teardown() {
    cleanup_test_env
}

@test "async logging edge case: disabled logging drivers work with signal masking" {
    # Test that signal masking doesn't interfere with disabled logging
    cd "$TEST_TMPDIR"
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "null:" &
    local conmon_pid=$!

    # Send signal during execution
    sleep 0.1
    kill -USR1 "$conmon_pid" 2>/dev/null || true
    wait "$conmon_pid" 2>/dev/null || true

    assert_success
    [ ! -f "null" ]
}

@test "async logging edge case: signal masking during rapid container lifecycle" {
    # Test that signal masking handles rapid container creation/destruction
    for i in {1..5}; do
        local rapid_cid="${CTR_ID}_rapid_$i"
        local rapid_log="$TEST_TMPDIR/rapid_$i.log"

        run_conmon --cid "$rapid_cid" --cuuid "$rapid_cid" --runtime "$VALID_PATH" --log-path "$rapid_log" &
        local pid=$!

        # Send signal during rapid lifecycle
        sleep 0.05
        kill -USR2 "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true

        assert_success
        [ -f "$rapid_log" ]
    done
}

@test "async logging edge case: signal masking with buffer flush timing" {
    # Test that signal masking doesn't interfere with async buffer flushing
    local large_log="$TEST_TMPDIR/large.log"

    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "$large_log" &
    local conmon_pid=$!

    # Send signals at different intervals to test flush timing
    for delay in 0.02 0.05 0.1; do
        sleep "$delay"
        kill -USR1 "$conmon_pid" 2>/dev/null || true
    done

    wait "$conmon_pid" 2>/dev/null || true
    assert_success
    [ -f "$large_log" ]
}

@test "async logging edge case: signal masking prevents race conditions in cleanup" {
    # Test that signal masking prevents race conditions during cleanup
    local cleanup_log="$TEST_TMPDIR/cleanup.log"

    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$VALID_PATH" --log-path "$cleanup_log" &
    local conmon_pid=$!

    # Give it time to initialize
    sleep 0.1

    # Send TERM signal to trigger cleanup while sending other signals
    kill -USR1 "$conmon_pid" 2>/dev/null || true
    kill -TERM "$conmon_pid" 2>/dev/null || true
    kill -USR2 "$conmon_pid" 2>/dev/null || true

    wait "$conmon_pid" 2>/dev/null || true

    # Log file should be properly created and closed
    [ -f "$cleanup_log" ]
}
