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

@test "timer-command flag in help" {
    run_conmon --help
    assert_success
    assert_output_contains "--timer-command"
    assert_output_contains "Execute COMMAND every SECONDS"
}

@test "invalid timer-command format should fail" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "/bin/true" \
        --log-path "$LOG_PATH" --timer-command "invalid"
    assert_failure
    assert_output_contains "Invalid timer-command format 'invalid'. Expected ID:SECONDS:COMMAND"
}

@test "incomplete timer-command format should fail" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "/bin/true" \
        --log-path "$LOG_PATH" --timer-command "0:5"
    assert_failure
    assert_output_contains "Invalid timer-command format '0:5'. Expected ID:SECONDS:COMMAND"
}

@test "invalid timer-command ID should fail" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "/bin/true" \
        --log-path "$LOG_PATH" --timer-command "abc:5:echo test"
    assert_failure
    assert_output_contains "Invalid timer-command ID 'abc'"
}

@test "invalid timer-command interval should fail" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "/bin/true" \
        --log-path "$LOG_PATH" --timer-command "0:abc:echo test"
    assert_failure
    assert_output_contains "Invalid timer-command interval 'abc'"
}

@test "zero timer-command interval should fail" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "/bin/true" \
        --log-path "$LOG_PATH" --timer-command "0:0:echo test"
    assert_failure
    assert_output_contains "Invalid timer-command interval '0'"
}

@test "empty timer-command command should fail" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "/bin/true" \
        --log-path "$LOG_PATH" --timer-command "0:5:"
    assert_failure
    assert_output_contains "Empty timer-command command"
}


@test "timer-command argument without timer-command should fail" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "/bin/true" \
        --log-path "$LOG_PATH" --timer-command-argument "0:arg1"
    assert_failure
    assert_output_contains "Timer-command ID 0 not found for argument 'arg1'"
}

@test "invalid timer-command argument format should fail" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "/bin/true" \
        --log-path "$LOG_PATH" --timer-command "0:5:echo" --timer-command-argument "invalid"
    assert_failure
    assert_output_contains "Invalid timer-command-argument format 'invalid'. Expected ID:ARGUMENT"
}

@test "valid timer-command format should be accepted" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "/bin/true" \
        --log-path "$LOG_PATH" --timer-command "0:5:echo test"
    if [[ "$output" == *"Invalid timer-command"* ]]; then
        echo "Timer-command parsing failed: $output"
        return 1
    fi
}

@test "multiple timer-command entries should be accepted" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "/bin/true" \
        --log-path "$LOG_PATH" \
        --timer-command "0:5:echo test1" \
        --timer-command "1:10:echo test2"
    if [[ "$output" == *"Invalid timer-command"* ]]; then
        echo "Multiple timer-command parsing failed: $output"
        return 1
    fi
}

@test "timer-command with arguments should be accepted" {
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "/bin/true" \
        --log-path "$LOG_PATH" \
        --timer-command "0:5:echo" \
        --timer-command-argument "0:arg1" \
        --timer-command-argument "0:arg2"
    if [[ "$output" == *"Invalid timer-command"* ]]; then
        echo "Timer-command with arguments parsing failed: $output"
        return 1
    fi
}

@test "timer-commands execute and create files" {
    TEST_FILE1="$TEST_TMPDIR/timer_test_1_$$"
    TEST_FILE2="$TEST_TMPDIR/timer_test_2_$$"

    jq '.process.args = ["/busybox", "sleep", "15"]' "$BUNDLE_PATH/config.json" > "$BUNDLE_PATH/config.json.tmp"
    mv "$BUNDLE_PATH/config.json.tmp" "$BUNDLE_PATH/config.json"

    timeout 15s "$CONMON_BINARY" \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$RUNTIME_BINARY" \
        --log-path "k8s-file:$LOG_PATH" \
        --bundle "$BUNDLE_PATH" \
        --socket-dir-path "$SOCKET_PATH" \
        --container-pidfile "$PID_FILE" \
        --conmon-pidfile "$CONMON_PID_FILE" \
        --timer-command "0:1:/bin/sh" \
        --timer-command-argument "0:-c" \
        --timer-command-argument "0:echo 'timer0_executed' >> $TEST_FILE1" \
        --timer-command "1:2:/bin/sh" \
        --timer-command-argument "1:-c" \
        --timer-command-argument "1:echo 'timer1_executed' >> $TEST_FILE2" &

    local conmon_pid=$!

    local start_time=$(date +%s)
    local file1_lines=0
    local file2_lines=0
    local runtime_available=false

    while [ $(($(date +%s) - start_time)) -lt 15 ]; do
        if [ -f "$TEST_FILE1" ]; then
            file1_lines=$(wc -l < "$TEST_FILE1" 2>/dev/null || echo 0)
            runtime_available=true
        fi
        if [ -f "$TEST_FILE2" ]; then
            file2_lines=$(wc -l < "$TEST_FILE2" 2>/dev/null || echo 0)
            runtime_available=true
        fi

        if [ $file1_lines -ge 2 ] && [ $file2_lines -ge 2 ]; then
            break
        fi

        sleep 0.125
    done

    kill $conmon_pid 2>/dev/null || true
    wait $conmon_pid 2>/dev/null || true

    # Verify results for functional test
    [ -f "$TEST_FILE1" ] || {
        echo "Timer-command 0 file was not created"
        rm -f "$TEST_FILE1" "$TEST_FILE2"
        return 1
    }

    [ -f "$TEST_FILE2" ] || {
        echo "Timer-command 1 file was not created"
        rm -f "$TEST_FILE1" "$TEST_FILE2"
        return 1
    }

    [ $file1_lines -ge 2 ] || {
        echo "Timer-command 0 executed only $file1_lines times (expected at least 2)"
        rm -f "$TEST_FILE1" "$TEST_FILE2"
        return 1
    }

    [ $file2_lines -ge 2 ] || {
        echo "Timer-command 1 executed only $file2_lines times (expected at least 2)"
        rm -f "$TEST_FILE1" "$TEST_FILE2"
        return 1
    }

    grep -q "timer0_executed" "$TEST_FILE1" || {
        echo "Timer-command 0 file has incorrect content"
        rm -f "$TEST_FILE1" "$TEST_FILE2"
        return 1
    }

    grep -q "timer1_executed" "$TEST_FILE2" || {
        echo "Timer-command 1 file has incorrect content"
        rm -f "$TEST_FILE1" "$TEST_FILE2"
        return 1
    }

    rm -f "$TEST_FILE1" "$TEST_FILE2"
}
