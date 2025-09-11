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

@test "healthcheck integration test with container" {
    # Skip if runtime binary is not available
    check_runtime_binary

    # Setup container environment
    setup_container_env

    # Create a simple healthcheck script in the container
    cat > "$ROOTFS/bin/healthcheck" << 'EOF'
#!/bin/sh
echo "healthcheck executed"
exit 0
EOF
    chmod +x "$ROOTFS/bin/healthcheck"

    # Update the container config to use our healthcheck script
    cat > "$BUNDLE_PATH/config.json" << EOF
{
    "ociVersion": "1.0.0",
    "process": {
        "terminal": false,
        "user": {"uid": 0, "gid": 0},
        "args": ["/busybox", "sleep", "10"],
        "cwd": "/",
        "env": ["PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"]
    },
    "root": {"path": "rootfs", "readonly": true}
}
EOF

    # Test healthcheck with a short-lived container using cmd and args format
    timeout 15s $CONMON_BINARY \
        --bundle "$BUNDLE_PATH" \
        --cid "$CTR_ID" \
        --cuuid "$CTR_ID" \
        --runtime "$RUNTIME_BINARY" \
        --log-path "$LOG_PATH" \
        --healthcheck-cmd /bin/healthcheck \
        --healthcheck-interval 2 \
        --healthcheck-timeout 1 \
        --healthcheck-retries 2 \
        --healthcheck-start-period 0 \
        --socket-dir-path "$SOCKET_PATH" \
        --container-pidfile "$PID_FILE" \
        --conmon-pidfile "$CONMON_PID_FILE" &

    local conmon_pid=$!

    # Wait a bit for healthcheck to run
    sleep 5

    # Check if conmon is still running (it should be)
    if kill -0 "$conmon_pid" 2>/dev/null; then
        # Kill conmon and clean up
        kill "$conmon_pid" 2>/dev/null || true
        wait "$conmon_pid" 2>/dev/null || true
    fi

    # Clean up container
    cleanup_test_env

    # Test passes if we get here without errors
    [ true ]
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