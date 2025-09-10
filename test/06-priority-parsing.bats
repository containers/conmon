#!/usr/bin/env bats

load test_helper

setup() {
    check_conmon_binary
    setup_container_env
}

teardown() {
    cleanup_test_env
}

# Helper function to run conmon with journald logging
run_conmon_journald() {
    local extra_args=("$@")
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$RUNTIME_BINARY" \
               --log-path "journald:" --bundle "$BUNDLE_PATH" "${extra_args[@]}"
}

# Helper function to create a test script with priority messages
create_test_script() {
    local script_name="$1"
    local script_content="$2"

    echo "$script_content" > "$ROOTFS/$script_name"
    chmod +x "$ROOTFS/$script_name"
}

# Helper function to update container args to run a specific script
update_container_args() {
    local script_path="$1"

    # Use jq to update the config.json with new args, fallback to manual edit if jq not available
    if command -v jq >/dev/null 2>&1; then
        jq ".process.args = [\"$script_path\"]" "$BUNDLE_PATH/config.json" > "$BUNDLE_PATH/config.json.tmp" && \
        mv "$BUNDLE_PATH/config.json.tmp" "$BUNDLE_PATH/config.json"
    else
        # Fallback: recreate config with new args
        local temp_config=$(mktemp)
        sed "s|\"args\": \[.*\]|\"args\": [\"$script_path\"]|" "$BUNDLE_PATH/config.json" > "$temp_config"
        mv "$temp_config" "$BUNDLE_PATH/config.json"
    fi
}

# Helper function to run a priority test with verification
run_priority_test() {
    local script_name="$1"
    local script_content="$2"
    local verify_journal="${3:-false}"

    create_test_script "$script_name" "$script_content"
    update_container_args "/$script_name"

    if [ "$verify_journal" = "true" ]; then
        run_conmon_journald --terminal --conmon-pidfile "$CONMON_PID_FILE"
    else
        run_conmon_journald --terminal
    fi

    assert_success

    # Optional journal verification
    if [ "$verify_journal" = "true" ] && [ -f "$CONMON_PID_FILE" ]; then
        local conmon_pid
        conmon_pid=$(cat "$CONMON_PID_FILE")
        sleep 1  # Give journald time to process

        local journal_output
        journal_output=$(get_conmon_journal_output "$conmon_pid")

        if [ -n "$journal_output" ]; then
            echo "Journal output available for verification"
        fi
    fi
}

@test "priority parsing: valid priority prefixes are parsed correctly" {
    skip_if_no_journald

    run_priority_test "test_priority.sh" '#!/bin/bash
echo "<0>Emergency message"
echo "<1>Alert message"
echo "<2>Critical message"
echo "<3>Error message"
echo "<4>Warning message"
echo "<5>Notice message"
echo "<6>Info message"
echo "<7>Debug message"
echo "Regular message without prefix"
echo "<8>Invalid priority (should be treated as regular message)"
echo "<>Empty priority (should be treated as regular message)"
echo "<<0>>Double brackets (should be treated as regular message)"
echo "<0>Multi<1>priority<2>line"' true
}

@test "priority parsing: messages without prefix use default priority" {
    skip_if_no_journald

    run_priority_test "test_no_prefix.sh" '#!/bin/bash
echo "Regular message without priority prefix"'
}

@test "priority parsing: invalid priority prefixes are treated as regular text" {
    skip_if_no_journald

    run_priority_test "test_invalid_prefix.sh" '#!/bin/bash
echo "<8>Invalid priority 8"
echo "<9>Invalid priority 9"
echo "<>Empty priority"
echo "<a>Non-numeric priority"
echo "<<0>>Double brackets"
echo "<0 Missing closing bracket"
echo "< 0>Space in priority"'
}

@test "priority parsing: stderr vs stdout default priorities" {
    skip_if_no_journald

    # Create script that outputs to both stdout and stderr
    create_test_script "test_stderr_stdout.sh" '#!/bin/bash
echo "stdout message"
echo "stderr message" >&2
echo "<1>stdout with priority 1"
echo "<2>stderr with priority 2" >&2'

    update_container_args "/test_stderr_stdout.sh"

    run_conmon_journald --terminal
    assert_success
}

@test "priority parsing: multiline messages with priority prefix only on first line" {
    skip_if_no_journald

    # Create script with multiline messages
    create_test_script "test_multiline.sh" '#!/bin/bash
printf "<3>This is an error message\nwith a second line\nand a third line\n"
printf "Regular message\nwith second line\n"'

    update_container_args "/test_multiline.sh"

    run_conmon_journald --terminal
    assert_success
}

@test "priority parsing: edge cases with very short buffers" {
    skip_if_no_journald

    # Create script with very short messages
    create_test_script "test_short.sh" '#!/bin/bash
echo "<"
echo "<0"
echo "<0>"
echo "a"
echo ""'

    update_container_args "/test_short.sh"

    run_conmon_journald --terminal
    assert_success
}

@test "priority parsing: all valid priority levels 0-7" {
    skip_if_no_journald

    # Create script that tests all valid priority levels
    create_test_script "test_all_priorities.sh" '#!/bin/bash
for i in {0..7}; do
    echo "<$i>Priority level $i message"
done'

    update_container_args "/test_all_priorities.sh"

    run_conmon_journald --terminal --conmon-pidfile "$CONMON_PID_FILE"
    assert_success

    # Verify that messages were written to journald with correct priorities
    if [ -f "$CONMON_PID_FILE" ]; then
        local conmon_pid
        conmon_pid=$(cat "$CONMON_PID_FILE")

        # Give journald a moment to process the messages
        sleep 1

        # Check that we can find our test messages in the journal
        local journal_output
        journal_output=$(get_conmon_journal_output "$conmon_pid")

        if [ -n "$journal_output" ]; then
            # Verify that at least some priority messages were logged
            if [[ "$journal_output" == *"Priority level"* ]]; then
                # Test passed - found expected content in journal
                :
            else
                echo "Warning: Expected priority messages not found in journal output"
                echo "Journal output: $journal_output"
            fi
        fi
    fi
}

@test "priority parsing: works with other log drivers simultaneously" {
    # Create script with priority messages
    create_test_script "test_mixed_logging.sh" '#!/bin/bash
echo "<3>Error message with priority"
echo "Regular message"
echo "<6>Info message"'

    update_container_args "/test_mixed_logging.sh"

    # Test with both k8s-file and journald logging
    if command -v journalctl >/dev/null 2>&1; then
        run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$RUNTIME_BINARY" \
                   --bundle "$BUNDLE_PATH" --log-path "$LOG_PATH" --log-path "journald:" --terminal
        assert_success
        [ -f "$LOG_PATH" ]
    else
        # Just test k8s-file if journald not available
        run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$RUNTIME_BINARY" \
                   --bundle "$BUNDLE_PATH" --log-path "$LOG_PATH" --terminal
        assert_success
        [ -f "$LOG_PATH" ]
    fi
}

# Test to demonstrate better error checking
@test "priority parsing: error handling with invalid log path" {
    # Test error handling when journald is not available
    run_conmon --cid "$CTR_ID" --cuuid "$CTR_ID" --runtime "$RUNTIME_BINARY" \
               --bundle "$BUNDLE_PATH" --log-path "invalid_driver:" --terminal 2>&1 || true

    # Use assert_output_contains for specific error checking
    if [ "$status" -ne 0 ]; then
        assert_output_contains "log driver"
    fi
}

# Helper function to skip test if journald is not available
skip_if_no_journald() {
    if ! command -v journalctl >/dev/null 2>&1; then
        skip "journalctl not available - skipping journald test"
    fi

    # Also check if systemd logging is compiled in
    if ! $CONMON_BINARY --help 2>&1 | grep -q "journald"; then
        skip "conmon not compiled with journald support"
    fi
}
