#!/usr/bin/env bats

load test_helper

setup() {
    check_conmon_binary
    setup_test_env
}

teardown() {
    cleanup_test_env
}

@test "multibyte characters in log-tag with C locale" {
    # Test that multibyte characters like äöüß work in --log-tag even with C locale
    LANG=C LC_CTYPE=C run_conmon --log-tag="äöüß" --version
    assert_success
    assert_output_contains "conmon version"
}

@test "multibyte characters in log-tag with UTF-8 locale" {
    # Test that multibyte characters work properly with UTF-8 locale
    LC_ALL=C.UTF-8 run_conmon --log-tag="äöüß" --version
    assert_success
    assert_output_contains "conmon version"
}

@test "empty log-tag should work" {
    # Ensure empty log-tag doesn't break anything
    run_conmon --log-tag="" --version
    assert_success
    assert_output_contains "conmon version"
}

@test "very long multibyte log-tag should work" {
    # Test with a longer string containing multibyte characters
    local long_tag="äöüß-äöüß-äöüß-äöüß-äöüß-äöüß-äöüß-äöüß"
    run_conmon --log-tag="$long_tag" --version
    assert_success
    assert_output_contains "conmon version"
}

@test "log-tag with spaces and multibyte characters" {
    # Test log-tag with spaces and multibyte characters
    run_conmon --log-tag="test äöüß space" --version
    assert_success
    assert_output_contains "conmon version"
}