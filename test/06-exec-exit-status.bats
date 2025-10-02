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
        skip "conmon binary not found for integration testing at $conmon_path"
    fi

    # Check if we can create a simple container for testing
    if ! timeout 10 podman --conmon $conmon_path run --rm registry.access.redhat.com/ubi10/ubi-micro:latest true >/dev/null 2>&1; then
        skip "Cannot create test containers with podman"
    fi

    echo "Running integration test with podman..."

    # Create a test container
    local container_id
    container_id=$(podman --conmon $conmon_path run -dt registry.access.redhat.com/ubi10/ubi-micro:latest sleep 30)

    if [ -z "$container_id" ]; then
        skip "Failed to create test container"
    fi

    # Test 1: Success case
    if ! podman --conmon $conmon_path exec "$container_id" true; then
        podman --conmon $conmon_path rm -f "$container_id" >/dev/null 2>&1
        echo "FAIL: true command should succeed"
        return 1
    fi

    # Test 2: Failure case - this would fail with the regression
    if podman --conmon $conmon_path exec "$container_id" false; then
        podman --conmon $conmon_path rm -f "$container_id" >/dev/null 2>&1
        echo "FAIL: false command should fail (regression detected!)"
        echo "This indicates the fc0a342 regression where all exec commands return 0"
        return 1
    fi

    # Test 3: Custom exit code - this would return 0 with the regression
    if podman --conmon $conmon_path exec "$container_id" sh -c 'exit 42'; then
        podman --conmon $conmon_path rm -f "$container_id" >/dev/null 2>&1
        echo "FAIL: 'exit 42' should fail with code 42 (regression detected!)"
        echo "This indicates the fc0a342 regression where all exec commands return 0"
        return 1
    fi

    # Clean up
    podman --conmon $conmon_path rm -f "$container_id" >/dev/null 2>&1

    echo "Integration test passed: exec exit codes work correctly"
}