#!/usr/bin/env bats

load test_helper

setup() {
    check_conmon_binary
    check_runtime_binary
    setup_test_env
}

teardown() {
    cleanup_test_env
}

@test "conmon help contains exec option" {
    # Basic test to ensure exec functionality is present
    run_conmon --help
    assert_success
    assert_output_contains "--exec"
}

@test "exec requires proper arguments" {
    # Test that exec requires proper arguments (validation working)
    run_conmon \
        --cid "test" \
        --cuuid "test" \
        --runtime /bin/true \
        --exec \
        --socket-dir-path /tmp \
        --container-pidfile /dev/null \
        --log-path /dev/null

    # Should fail due to missing --exec-process-spec
    assert_failure
}

# Integration test that can be run manually or in CI
@test "integration: exec exit codes work correctly" {
    # This test can only run if podman is available and configured
    if ! command -v podman >/dev/null 2>&1; then
        skip "podman not available for integration testing"
    fi

    # Use the conmon binary from the build (using absolute path)
    local conmon_path="$(dirname "$CONMON_BINARY")/conmon"

    if [ ! -f "$conmon_path" ]; then
        die "conmon binary not found for integration testing at $conmon_path"
    fi

    # Check if we can create a simple container for testing.
    #
    # NB: every podman invocation in this test is wrapped in a timeout. None
    # of them has any business taking long, and an unbounded one hangs the
    # whole suite -- bats runs tests serially, and a command substitution
    # waits for stdout to be closed, which a misbehaving conmon may never do.
    run timeout 10 podman --conmon "$conmon_path" run --rm "$UBI10_MICRO_IMAGE" true
    if [ "$status" -ne 0 ]; then
        die "cannot create test containers with podman: $output"
    fi

    echo "Running integration test with podman..."

    # Create a test container
    local container_id
    container_id=$(timeout 60 podman --conmon "$conmon_path" run -dt "$UBI10_MICRO_IMAGE" sleep 30)

    if [ -z "$container_id" ]; then
        die "failed to create test container"
    fi

    # Test 1: Success case
    if ! timeout 60 podman --conmon "$conmon_path" exec "$container_id" true; then
        timeout 60 podman --conmon "$conmon_path" rm -f "$container_id" >/dev/null 2>&1
        echo "FAIL: true command should succeed"
        return 1
    fi

    # Test 2: Failure case - this would fail with the regression
    if timeout 60 podman --conmon "$conmon_path" exec "$container_id" false; then
        timeout 60 podman --conmon "$conmon_path" rm -f "$container_id" >/dev/null 2>&1
        echo "FAIL: false command should fail (regression detected!)"
        echo "This indicates the fc0a342 regression where all exec commands return 0"
        return 1
    fi

    # Test 3: Custom exit code - this would return 0 with the regression
    if timeout 60 podman --conmon "$conmon_path" exec "$container_id" sh -c 'exit 42'; then
        timeout 60 podman --conmon "$conmon_path" rm -f "$container_id" >/dev/null 2>&1
        echo "FAIL: 'exit 42' should fail with code 42 (regression detected!)"
        echo "This indicates the fc0a342 regression where all exec commands return 0"
        return 1
    fi

    # Clean up
    timeout 60 podman --conmon "$conmon_path" rm -f "$container_id" >/dev/null 2>&1

    echo "Integration test passed: exec exit codes work correctly"
}
