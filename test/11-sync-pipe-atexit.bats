#!/usr/bin/env bats

# Tests for sync pipe atexit handler - ensures the sync pipe always gets
# a JSON response even when conmon exits via pexit() without having
# written to the sync pipe through the normal code path.
#
# Strategy: make the bundle directory read-only so that mkfifo() fails
# in setup_terminal_control_fifo() (ctrl.c). This triggers pexitf()
# AFTER sync_pipe_fd is set but BEFORE any normal write to the sync pipe.
# The atexit handler is the only code path that writes to the sync pipe.
#
# These tests FAIL on a baseline build without the atexit handler (the
# sync pipe receives nothing and the reader times out).
#
# Note: root bypasses filesystem permission checks, so chmod 555 does
# not block mkfifo for root. These tests must run as a non-root user.

load test_helper

setup() {
    check_conmon_binary

    if [ "$(id -u)" -eq 0 ]; then
        skip "atexit tests require non-root (root bypasses chmod permissions)"
    fi

    setup_test_env

    # Create a separate bundle directory (not TEST_TMPDIR) so we can
    # make it read-only without breaking pidfiles, logs, or sockets.
    BUNDLE_PATH="$TEST_TMPDIR/bundle"
    ROOTFS="$BUNDLE_PATH/rootfs"
    mkdir -p "$ROOTFS"

    # Generate a minimal valid OCI config.json.
    generate_runtime_config "$BUNDLE_PATH" "$ROOTFS" false "true"

    # Make the bundle directory read-only. This causes mkfifo() to fail
    # with EACCES in ctrl.c:setup_fifo(), which calls pexitf() -> exit()
    # -> atexit handler fires and writes fallback JSON to the sync pipe.
    chmod 555 "$BUNDLE_PATH"
}

teardown() {
    # Restore write permission so cleanup_tmpdir can remove it.
    chmod -R 755 "$TEST_TMPDIR" 2>/dev/null || true
    cleanup_test_env
}

@test "sync-pipe atexit: error reported on unexpected exit without exit-command" {
    # Trigger a pexit() after sync_pipe_fd is set but before the normal
    # runtime-failure path writes to it. The atexit handler must write
    # the fallback JSON. Without the atexit handler, the sync pipe gets
    # nothing and this test fails.

    start_oci_sync_pipe_reader

    _run_conmon \
        --log-path "k8s-file:$LOG_PATH" \
        6>"$OCI_SYNCPIPE_PATH"

    # conmon double-forks; the outer process exits 0 regardless.
    # The grandchild hits the mkfifo failure and exits via pexitf().
    wait_for_syncpipe_output 1

    run cat "$TEST_TMPDIR/syncpipe-output"
    echo "# syncpipe output: $output" >&3
    assert "${output}" =~ '"pid"' "sync pipe should contain pid field"
    assert "${output}" =~ '"message"' "sync pipe should contain message field"
}

@test "sync-pipe atexit: error reported on unexpected exit with exit-command" {
    # Same scenario but with --exit-command. The do_exit_command atexit
    # handler calls report_sync_pipe_if_needed() before
    # close_all_fds_ge_than(3), so the sync pipe fd is still open.

    start_oci_sync_pipe_reader

    _run_conmon \
        --log-path "k8s-file:$LOG_PATH" \
        --exit-command /bin/true \
        6>"$OCI_SYNCPIPE_PATH"

    wait_for_syncpipe_output 1

    run cat "$TEST_TMPDIR/syncpipe-output"
    echo "# syncpipe output: $output" >&3
    assert "${output}" =~ '"pid"' "sync pipe should contain pid field"
    assert "${output}" =~ '"message"' "sync pipe should contain message field"
}
