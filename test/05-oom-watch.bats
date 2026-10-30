#!/usr/bin/env bats

load test_helper

setup_file() {
    if [[ $(uname -s) != Linux ]]; then
        skip "cgroup v2 reader requires Linux"
    fi
    local source_dir
    source_dir="$(dirname "$BATS_TEST_DIRNAME")"
    # Deliberately compile the production C code, not a copy of its algorithm.
    # shellcheck disable=SC2046
    "${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
        $(pkg-config --cflags glib-2.0) \
        "$BATS_TEST_DIRNAME/oom_watch.c" \
        "$source_dir/src/globals.c" "$source_dir/src/utils.c" \
        -o "$BATS_FILE_TMPDIR/oom-watch" $(pkg-config --libs glib-2.0)
}

@test "OOM watch: max-only notification retains observation without a marker" {
    run "$BATS_FILE_TMPDIR/oom-watch" max-only "$BATS_TEST_TMPDIR"
    assert_success
}

@test "OOM watch: max-only notification followed by OOM creates markers" {
    run "$BATS_FILE_TMPDIR/oom-watch" max-then-oom "$BATS_TEST_TMPDIR"
    assert_success
}

@test "OOM watch: repeated max-only notifications retain observation" {
    run "$BATS_FILE_TMPDIR/oom-watch" max-burst "$BATS_TEST_TMPDIR"
    assert_success
}

@test "OOM watch: OOM as the first notification creates markers" {
    run "$BATS_FILE_TMPDIR/oom-watch" oom-first "$BATS_TEST_TMPDIR"
    assert_success
}

@test "OOM watch: kill-only notification creates markers" {
    run "$BATS_FILE_TMPDIR/oom-watch" kill-only "$BATS_TEST_TMPDIR"
    assert_success
}

@test "OOM watch: unchanged positive counters retain observation" {
    run "$BATS_FILE_TMPDIR/oom-watch" unchanged-positive "$BATS_TEST_TMPDIR"
    assert_success
}

@test "OOM watch: removed memory.events closes observation" {
    run "$BATS_FILE_TMPDIR/oom-watch" removed-file "$BATS_TEST_TMPDIR"
    assert_success
}

@test "OOM watch: marker failure leaves observation available for retry" {
    run "$BATS_FILE_TMPDIR/oom-watch" marker-retry "$BATS_TEST_TMPDIR"
    assert_success
}
