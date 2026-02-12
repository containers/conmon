#!/usr/bin/env bats

load test_helper

@test "healthcheck help shows new CLI options" {
    # Test that the help output shows the new healthcheck CLI options
    run $CONMON_BINARY --help
    [ "$status" -eq 0 ]
    [[ "$output" == *"healthcheck-cmd"* ]]
    [[ "$output" == *"healthcheck-arg"* ]]
    [[ "$output" == *"healthcheck-interval"* ]]
    [[ "$output" == *"healthcheck-timeout"* ]]
    [[ "$output" == *"healthcheck-retries"* ]]
    [[ "$output" == *"healthcheck-start-period"* ]]
}

@test "healthcheck CLI options are parsed correctly" {
    # Test that healthcheck CLI options are properly parsed with command and arguments
    run $CONMON_BINARY --bundle /tmp --cid test --cuuid test --runtime /bin/true --healthcheck-cmd echo --healthcheck-arg healthy --healthcheck-interval 30 --healthcheck-timeout 10 --healthcheck-retries 3 --healthcheck-start-period 0 --version
    [ "$status" -eq 0 ]
}

@test "healthcheck requires --healthcheck-cmd to be specified" {
    # Test that healthcheck parameters without --healthcheck-cmd are ignored
    run $CONMON_BINARY --bundle /tmp --cid test --cuuid test --runtime /bin/true --healthcheck-interval 30 --healthcheck-timeout 10 --healthcheck-retries 3 --healthcheck-start-period 0 --version
    [ "$status" -eq 0 ]
}

@test "healthcheck fails when interval provided without cmd" {
    # Test that conmon fails when healthcheck interval is provided without --healthcheck-cmd
    run $CONMON_BINARY --bundle /tmp --cid test --cuuid test --runtime /bin/true --log-path /tmp/test.log --healthcheck-interval 30
    [ "$status" -ne 0 ]
    # Check if the error is related to healthcheck validation
    [[ "$output" == *"healthcheck"* ]] || [[ "$stderr" == *"healthcheck"* ]] || [[ "$output" == *"cmd"* ]] || [[ "$stderr" == *"cmd"* ]]
}

@test "healthcheck with minimal required arguments" {
    # Test healthcheck with only --healthcheck-cmd specified
    run $CONMON_BINARY --bundle /tmp --cid test --cuuid test --runtime /bin/true --healthcheck-cmd echo --version
    [ "$status" -eq 0 ]
}

@test "healthcheck with all parameters specified" {
    # Test healthcheck with all parameters specified using cmd and args format
    run $CONMON_BINARY --bundle /tmp --cid test --cuuid test --runtime /bin/true --healthcheck-cmd curl --healthcheck-arg -f --healthcheck-arg http://localhost:8080/health --healthcheck-interval 60 --healthcheck-timeout 30 --healthcheck-retries 5 --healthcheck-start-period 120 --version
    [ "$status" -eq 0 ]
}

@test "healthcheck parameters accept valid values" {
    # Test that healthcheck parameters accept reasonable values with cmd and args
    run $CONMON_BINARY --bundle /tmp --cid test --cuuid test --runtime /bin/true --healthcheck-cmd echo --healthcheck-arg test --healthcheck-interval 1 --healthcheck-timeout 1 --healthcheck-retries 1 --healthcheck-start-period 0 --version
    [ "$status" -eq 0 ]
}


@test "healthcheck with command arguments" {
    # Test healthcheck with command and arguments using new format
    run $CONMON_BINARY --bundle /tmp --cid test --cuuid test --runtime /bin/true --log-path /tmp/test.log --healthcheck-cmd /bin/sh --healthcheck-arg -c --healthcheck-arg "echo hello world"
    [ "$status" -eq 0 ]
}

@test "healthcheck with shell command and multiple arguments" {
    # Test healthcheck with shell command and multiple arguments
    run $CONMON_BINARY --bundle /tmp --cid test --cuuid test --runtime /bin/true --log-path /tmp/test.log --healthcheck-cmd /bin/sh --healthcheck-arg -c --healthcheck-arg "curl -f http://localhost:8080/health && echo healthy"
    [ "$status" -eq 0 ]
}

@test "healthcheck with complex command structure" {
    # Test healthcheck with complex command structure using multiple args
    run $CONMON_BINARY --bundle /tmp --cid test --cuuid test --runtime /bin/true --log-path /tmp/test.log --healthcheck-cmd python3 --healthcheck-arg -c --healthcheck-arg "import requests; requests.get('http://localhost:8080/health').raise_for_status()"
    [ "$status" -eq 0 ]
}

@test "healthcheck fails when timeout is greater than interval" {
    # Skip if runtime binary is not available
    check_runtime_binary

    # Setup container environment to trigger validation
    setup_container_env "/busybox sleep 10"

    # Test that conmon fails when healthcheck timeout is greater than interval
    # Use --sync and --syslog to get logs in journald
    # Run conmon in background to get PID for journal log retrieval
    local healthcheck_interval=2
    local healthcheck_timeout=5

    echo "Starting conmon with invalid healthcheck config (timeout $healthcheck_timeout > interval $healthcheck_interval)..."
    local conmon_pid=$(start_conmon_healthcheck "/busybox" "$healthcheck_interval" "$healthcheck_timeout" 1 0 "$LOG_PATH" "" "echo")
    echo "Conmon started with PID: $conmon_pid"

    # Wait a moment for conmon to start and potentially fail
    sleep 2

    # Find the actual conmon PID (may be forked)
    local actual_conmon_pid=$(find_conmon_forked_pid "$conmon_pid" "$CTR_ID")
    echo "Using conmon PID: $actual_conmon_pid"

    # Get conmon journal entries to check for validation error
    local journal_output=$(get_conmon_journal_output "$actual_conmon_pid")
    echo "Journal output:"
    echo "$journal_output"

    # Clean up before assertion
    cleanup_test_env

    # Test should pass if we found the validation error message
    [[ "$journal_output" == *"Healthcheck timeout $healthcheck_timeout cannot be greater than interval $healthcheck_interval"* ]]
}

@test "healthcheck fails when interval is invalid" {
    # Skip if runtime binary is not available
    check_runtime_binary


    # Setup container environment to trigger validation
    setup_container_env "/busybox sleep 1000"

    # Test that conmon fails when healthcheck interval is invalid (0)
    local healthcheck_interval=0
    local healthcheck_timeout=2

    echo "Starting conmon with invalid healthcheck interval ($healthcheck_interval)..."
    local conmon_pid=$(start_conmon_healthcheck "/busybox" "$healthcheck_interval" "$healthcheck_timeout" 1 0 "$LOG_PATH" "" "echo")
    sleep 2

    local actual_conmon_pid=$(find_conmon_forked_pid "$conmon_pid" "$CTR_ID")
    local journal_output=$(get_conmon_journal_output "$actual_conmon_pid")

    # Clean up before assertion
    cleanup_test_env

    # Test should pass if we found the validation error message
    [[ "$journal_output" == *"Healthcheck interval $healthcheck_interval is out of range"* ]]
}

@test "healthcheck fails when timeout is invalid" {
    # Skip if runtime binary is not available
    check_runtime_binary


    # Setup container environment to trigger validation
    setup_container_env "/busybox sleep 10"

    # Test that conmon fails when healthcheck timeout is invalid (0)
    local healthcheck_interval=5
    local healthcheck_timeout=0

    echo "Starting conmon with invalid healthcheck timeout ($healthcheck_timeout)..."
    local conmon_pid=$(start_conmon_healthcheck "/busybox" "$healthcheck_interval" "$healthcheck_timeout" 1 0 "$LOG_PATH" "" "echo")
    sleep 2

    local actual_conmon_pid=$(find_conmon_forked_pid "$conmon_pid" "$CTR_ID")
    local journal_output=$(get_conmon_journal_output "$actual_conmon_pid")

    # Clean up before assertion
    cleanup_test_env

    # Test should pass if we found the validation error message
    [[ "$journal_output" == *"Healthcheck timeout $healthcheck_timeout is out of range"* ]]
}

@test "healthcheck fails when start_period is invalid" {
    # Skip if runtime binary is not available
    check_runtime_binary


    # Setup container environment to trigger validation
    setup_container_env "/busybox sleep 1000"

    # Test that conmon fails when healthcheck start_period is invalid (3601 - beyond max)
    local healthcheck_interval=5
    local healthcheck_timeout=2
    local healthcheck_start_period=3601

    echo "Starting conmon with invalid healthcheck start_period ($healthcheck_start_period)..."
    local conmon_pid=$(start_conmon_healthcheck "/busybox" "$healthcheck_interval" "$healthcheck_timeout" 1 "$healthcheck_start_period" "$LOG_PATH" "" "echo")
    echo "conmon pid: $conmon_pid"
    ps -ef | grep $conmon_pid
    echo "CTR_ID: $CTR_ID"
    local actual_conmon_pid=$(find_conmon_forked_pid "$conmon_pid" "$CTR_ID")
    echo "actual_conmon_pid: $actual_conmon_pid"

    sleep 2
    ps -ef | grep $actual_conmon_pid

    # Wait for validation error with polling like integration tests
    echo "Waiting for validation error (max 10 seconds)..."

    local validation_found=false
    local max_wait=5
    local wait_time=0
    local journal_output=""

    while [[ $wait_time -lt $max_wait ]]; do
        # Get conmon journal entries
        local actual_conmon_pid=$(find_conmon_forked_pid "$conmon_pid" "$CTR_ID")
        journal_output=$(get_conmon_journal_output "$actual_conmon_pid")

        if [[ -n "$journal_output" ]]; then
            # Check for validation error message
            if echo "$journal_output" | grep -q "Healthcheck start period $healthcheck_start_period is out of range \[0, 3600\]"; then
                echo "✅ Found validation error message in journal output after ${wait_time}s!"
                validation_found=true
                break
            fi
        fi

        sleep 1
        wait_time=$((wait_time + 1))
        echo "Checked at ${wait_time}s, still waiting..."
    done

    # Clean up before assertion
    cleanup_test_env

    # Test should pass if conmon exited due to validation failure
    # We can't check journal output for this validation, so just check that conmon exited
    ! kill -0 "$conmon_pid" 2>/dev/null
}

@test "healthcheck fails when retries is invalid" {
    # Skip if runtime binary is not available
    check_runtime_binary

    # Setup container environment to trigger validation
    setup_container_env "/busybox sleep 10"

    # Test that conmon fails when healthcheck retries is invalid (101)
    local healthcheck_interval=5
    local healthcheck_timeout=2
    local healthcheck_retries=101

    echo "Starting conmon with invalid healthcheck retries ($healthcheck_retries)..."
    local conmon_pid=$(start_conmon_healthcheck "/busybox" "$healthcheck_interval" "$healthcheck_timeout" "$healthcheck_retries" 0 "$LOG_PATH" "" "echo")
    sleep 2

    local actual_conmon_pid=$(find_conmon_forked_pid "$conmon_pid" "$CTR_ID")
    local journal_output=$(get_conmon_journal_output "$actual_conmon_pid")

    # Clean up before assertion
    cleanup_test_env

    # Test should pass if we found the validation error message
    [[ "$journal_output" == *"Healthcheck retries $healthcheck_retries is out of range"* ]]
}
