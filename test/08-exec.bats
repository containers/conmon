#!/usr/bin/env bats

load test_helper

setup() {
    check_conmon_binary
    check_runtime_binary
    setup_container_env "while [ ! -f /tmp/test.txt ]; do /busybox sleep 0.1; done; /busybox cat /tmp/test.txt"
    generate_process_spec "echo 'Hello from exec!' && echo 'Hello there!' > /tmp/test.txt"
}

teardown() {
    cleanup_test_env
}

@test "exec: simple --exec --exec-process-spec" {
    start_conmon_with_default_args --log-path "k8s-file:$LOG_PATH"
    wait_for_runtime_status "$CTR_ID" running

    start_conmon_with_default_args \
        --log-path "k8s-file:$LOG_PATH.exec" \
        --exec \
        --exec-process-spec "${BUNDLE_PATH}/process.json"

    wait_for_runtime_status "$CTR_ID" stopped

    # Check that the main process noticed the /tmp/test.txt.
    assert_file_exists "$LOG_PATH"
    run cat "$LOG_PATH"
    assert "${output}" =~ "Hello there!"  "'Hello there!' found in the log"

    # Check that the exec process output is stored in the log.
    assert_file_exists "$LOG_PATH.exec"
    run cat "$LOG_PATH.exec"
    assert "${output}" =~ "Hello from exec!"  "'Hello from exec!' found in the log"
}

@test "exec: --exec-attach without no --api-version" {
    start_conmon_with_default_args --log-path "k8s-file:$LOG_PATH"
    wait_for_runtime_status "$CTR_ID" running

    start_conmon_with_default_args \
        --log-path "k8s-file:$LOG_PATH.exec" \
        --sync \
        --exec \
        --exec-process-spec "${BUNDLE_PATH}/process.json" \
        --exec-attach

    assert_failure
    assert "${output}" =~ "Attach can only be specified for a non-legacy exec session"
}

@test "exec: --exec-attach without _OCI_ATTACHPIPE env variable" {
    start_conmon_with_default_args --log-path "k8s-file:$LOG_PATH"
    wait_for_runtime_status "$CTR_ID" running

    start_conmon_with_default_args \
        --log-path "k8s-file:$LOG_PATH.exec" \
        --api-version 1 \
        --sync \
        --exec \
        --exec-process-spec "${BUNDLE_PATH}/process.json" \
        --exec-attach

    assert_failure
    assert "${output}" =~ "--attach specified but _OCI_ATTACHPIPE was not"
}

@test "exec: --exec-attach" {
    start_conmon_with_default_args --log-path "k8s-file:$LOG_PATH"
    wait_for_runtime_status "$CTR_ID" running

    # Create the attach pipe and later pass it as fd 4 to conmon.
    mkfifo "$OCI_ATTACHSYNC_PATH"
    export _OCI_ATTACHPIPE=4

    # Run the reader in the background, otherwise it would block until the
    # conmon opens the other side of the pipe.
    {
        exec {r}<"$OCI_ATTACHSYNC_PATH"
        while IFS= read -r -u "$r" line; do
            echo "$line" >>$TEST_TMPDIR/attach-output
        done
    } &

    # Start the conmon and pass the writer's side as fd 4.
    start_conmon_with_default_args \
        --log-path "k8s-file:$LOG_PATH.exec" \
        --api-version 1 \
        --exec \
        --exec-process-spec "${BUNDLE_PATH}/process.json" \
        --exec-attach 4>"$OCI_ATTACHSYNC_PATH"

    wait_for_runtime_status "$CTR_ID" stopped

    # Check that the conmon wrote something back.
    assert_file_exists $TEST_TMPDIR/attach-output
    run cat $TEST_TMPDIR/attach-output
    assert "${output}" =~ '"data": 0'
}

@test "exec: --exec-attach with _OCI_STARTPIPE" {
    start_conmon_with_default_args --log-path "k8s-file:$LOG_PATH"
    wait_for_runtime_status "$CTR_ID" running

    # Create attach pipe and later pass it as fd 4 to conmon.
    mkfifo "$OCI_ATTACHSYNC_PATH"
    export _OCI_ATTACHPIPE=4

    # Create start pipe and later pass it as fd 5 to conmon.
    mkfifo "$OCI_STARTPIPE_PATH"
    export _OCI_STARTPIPE=5

    # Run the reader in the background, otherwise it would block until the
    # conmon opens the other side of the pipe.
    {
        exec {r}<"$OCI_ATTACHSYNC_PATH"
        while IFS= read -r -u "$r" line; do
            echo "$line" >>$TEST_TMPDIR/attach-output
        done
    } &

    # Run the writer in the background, otherwise it would block until the
    # conmon opens the other side of the pipe.
    {
        exec {w}>"$OCI_STARTPIPE_PATH"
        # Conmon reads twice from the startpipe. First read is before the fork(),
        # so just write to unblock `start_conmon_with_default_args`.
        printf 'start conmon\n' >&$w
        # Wait for the main test process to do initial asserts. It will signal
        # to this process by file creation.
        timeout 5 bash -c 'while [ ! -f $TEST_TMPDIR/startpipe-continue ]; do sleep 0.1; done;'
        # Do the second write to really start the conmon.
        printf 'start attach\n' >&$w
    } &

    # Start the conmon and pass fds to it.
    start_conmon_with_default_args \
        --log-path "k8s-file:$LOG_PATH.exec" \
        --api-version 1 \
        --exec \
        --exec-process-spec "${BUNDLE_PATH}/process.json" \
        --exec-attach 4>"$OCI_ATTACHSYNC_PATH" 5<"$OCI_STARTPIPE_PATH"

    # Give conmon some time to really start.
    sleep 1

    # The exec should not start yet.
    assert_file_exists "$LOG_PATH.exec"
    run cat "$LOG_PATH.exec"
    assert "${output}" !~ "Hello from exec!"

    # Trigger second write to startpipeline.
    touch $TEST_TMPDIR/startpipe-continue

    # The exec should start now.
    wait_for_runtime_status "$CTR_ID" stopped
    assert_file_exists "$LOG_PATH.exec"
    run cat "$LOG_PATH.exec"
    assert "${output}" =~ "Hello from exec!"
}
