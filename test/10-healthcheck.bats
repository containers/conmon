#!/usr/bin/env bats

load test_helper

@test "healthcheck help shows enable-healthcheck flag" {
    # Test that the help output shows the new healthcheck flag
    run $CONMON_BINARY --help
    [ "$status" -eq 0 ]
    [[ "$output" == *"enable-healthcheck"* ]]
}