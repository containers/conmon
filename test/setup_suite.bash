#!/usr/bin/env bash
#
# Suite-wide setup: prepare the container rootfs used by the container
# tests exactly once.
#
# Doing this here rather than in each test's setup() means:
#  - the image is pulled once per run, not once per test (which used to
#    be dozens of registry round-trips, and racy under `bats --jobs`);
#  - a failure to obtain the image aborts the whole run, instead of
#    silently turning every container test into a skip (which made a
#    broken environment look like a fully passing test suite).

load test_helper

# Bail out of the suite, telling the user what went wrong.
suite_fail() {
    echo "# FAIL: $*" >&3
    return 1
}

setup_suite() {
    if ! command -v podman >/dev/null 2>&1; then
        suite_fail "podman is required to prepare the test rootfs"
        return 1
    fi

    # Keep the rootfs as a tarball rather than an extracted tree: tests
    # extract their own copy, which is both cheap and free of the
    # permission problems of copying files like /etc/shadow (mode 0000)
    # around as an unprivileged user.
    export CONMON_TEST_ROOTFS_TAR="$BATS_SUITE_TMPDIR/rootfs.tar"

    # Note the lack of output redirection here: when this fails, the
    # reason for the failure is the whole point.
    # NB: no --policy here, it is not supported by podman < 5.0 (as found
    # on e.g. Ubuntu 24.04), and plain "podman pull" pulls anyway.
    if ! podman pull "$UBI10_MICRO_IMAGE"; then
        suite_fail "failed to pull $UBI10_MICRO_IMAGE"
        return 1
    fi

    local ctr
    if ! ctr=$(podman create "$UBI10_MICRO_IMAGE"); then
        suite_fail "failed to create a container from $UBI10_MICRO_IMAGE"
        return 1
    fi
    if ! podman export "$ctr" > "$CONMON_TEST_ROOTFS_TAR"; then
        podman rm "$ctr" >/dev/null 2>&1 || true
        suite_fail "failed to export the rootfs from $UBI10_MICRO_IMAGE"
        return 1
    fi
    podman rm "$ctr" >/dev/null 2>&1 || true
}
