# AGENTS.md

conmon is a small C daemon that monitors one OCI container. Podman and
CRI-O exec it with CLI flags, and it runs the OCI runtime (runc or crun).
It double-forks to detach from the caller (unless `--sync`). It holds the
container's stdio, writes the logs, serves the attach socket, and records
OOM events and the exit status in files. It is a single binary built on
glib, with optional libsystemd for journald. It targets Linux, and the few
FreeBSD bits sit under `#ifdef __FreeBSD__`.

## The caller contract: do not break it

Podman and CRI-O parse everything below. If you change any of it, the
callers have to change in step.

| Interface | Where | Format |
|---|---|---|
| CLI flags | `src/cli.c` `opt_entries` | Hidden flags (`--pidfile`, `--restore-arg`) are deprecated aliases. Keep them. |
| Sync pipe | fd in `$_OCI_SYNCPIPE`, `src/parent_pipe_fd.c` | One JSON object per line: `{"data": N}` (`--api-version` >= 1), legacy `{"pid": N}` / `{"exit_code": N}`, optional `"message"`. Negative N means failure. |
| Start / attach pipes | `$_OCI_STARTPIPE`, `$_OCI_ATTACHPIPE`, `src/conmon.c` | conmon blocks reading the start pipe before forking, so the caller can set up cgroups. With `--exec-attach`, conmon writes a sync message to the attach pipe once the attach socket is ready, then reads the start pipe a second time just before exec'ing the runtime, so no output is lost before the caller attaches. |
| Exit status | `src/conmon.c` (end of `main`) | Decimal status in `<persist-dir>/exit` and `<exit-dir>/<cid>`. A signal is reported as 128+N. A `--timeout` kill is reported as `-1`. |
| OOM marker | `src/cgroup.c` `create_oom_files` | Empty file `oom` in the persist dir and the bundle dir. |
| Attach socket | `src/conn_sock.c` | `SOCK_SEQPACKET` at `<socket-dir-path>/<cuuid>/attach`. `<socket-dir-path>/<cuuid>` is a symlink to the bundle that keeps `sun_path` short; `--full-attach` uses the bundle path instead. Each output frame starts with one byte holding the `stdpipe_t` value (2 = stdout, 3 = stderr). The comment in `src/utils.h` says to keep this enum in sync with `container_attach.go`. |
| Control FIFO | `<bundle>/ctl`, `src/ctrl.c` | Lines of the form `TYPE H W\n`. TYPE 1 resizes the window, TYPE 2 reopens the logs (`src/config.h`). |
| k8s-file log | `src/ctr_logging.c` `write_k8s_log` | `<timestamp> <stdout\|stderr> <F\|P> <text>`, the CRI log format. The timestamp is local time as `YYYY-MM-DDTHH:MM:SS.nnnnnnnnn±HH:MM`, always with 9 fractional digits (`set_k8s_timestamp`). Unlike Go's `RFC3339Nano`, trailing zeros are not trimmed. |
| journald fields | `src/ctr_logging.c` | `CONTAINER_ID_FULL`, `CONTAINER_ID`, `CONTAINER_NAME`, `CONTAINER_TAG`, `CONTAINER_PARTIAL_MESSAGE=true`. |

- `--log-path` takes `driver:path` and can be repeated. The drivers are
  `k8s-file`, `journald`, `passthrough` and `none`/`off`/`null`. The
  parser is `parse_log_path`. A bare value is a k8s-file path only if it
  has no `:` and is not a driver name: a bare `journald`, `passthrough`
  or `none` selects that driver, and a bare `k8s-file` fails with
  "k8s-file requires a filename". A leading colon (`:journald`) forces a
  k8s-file path. The value is split with `strtok(":")`, so log paths
  cannot contain a colon: `/tmp/a:b` fails with "No such log driver
  /tmp/a", and `k8s-file:/a:b` is silently cut to `/a`.
- `passthrough` hands conmon's own stdio straight to the runtime. In that
  mode there is no attach socket and no ctl FIFO.
- Every visible flag is documented in `docs/conmon.8.md`. When you add or
  change a flag, update `opt_entries`, the `extern` in `src/cli.h` and the
  man page together.
- Seccomp notify support was removed in `9a57dda`. Do not reintroduce it.

## Source map (`src/`)

| File | Owns |
|---|---|
| `conmon.c` | `main`: double fork, pipes, forking and exec'ing the runtime, the main loop, writing the exit files. |
| `cli.c` / `cli.h` | GOption flag table, flag validation, `opt_*` globals. |
| `runtime_args.c` | Builds the runtime argv (`create`/`exec`/`restore`, `--console-socket`, `--pid-file`). |
| `ctr_logging.c` | Log drivers (k8s-file, journald, passthrough). Also log-path security checks (`--log-allowlist-dir`, symlink checks), truncation, and `--log-rotate`. |
| `ctr_stdio.c` | Reading container stdout/stderr, the HUP handling for a tty, draining at exit. |
| `conn_sock.c` | The attach socket, the console socket, and the `--sdnotify-socket` relay. |
| `ctrl.c` / `cmsg.c` | Accepting the terminal from the runtime (an fd sent with `SCM_RIGHTS`), the ctl/winsz FIFOs. |
| `ctr_exit.c` | SIGCHLD reaping, signal forwarding, `--timeout`, `--exit-command`. |
| `cgroup.c` / `oom.c` | OOM detection on cgroup v1 (eventfd) and v2 (inotify on `memory.events`). `oom_score_adj` handling. |
| `close_fds.c` | Records the fds open at startup (a constructor). Closes the fds handed to the runtime (`LISTEN_FDS`, `--preserve-fds`) and the other inherited fds. |
| `parent_pipe_fd.c` | Sync pipe JSON. |
| `self_pipe.c` | Async-signal-safe wakeup of the main loop. |
| `utils.h` | `pexit`/`nexit`/`nwarn`/`ndebug`… logging macros, `_cleanup_*` attributes. |

## Build

```sh
make                                   # bin/conmon; journald enabled if pkg-config finds libsystemd
make DISABLE_SYSTEMD=1                 # without journald
meson setup build && ninja -C build    # CI runs this with --fatal-meson-warnings
```

- Sources are listed by hand in two places: `OBJS` in the `Makefile` and
  the `executable()` list in `meson.build`. A new `.c`/`.h` file has to be
  added to both.
- The strict warning set (`-Werror -Wshadow -Wvla -Wswitch-default
  -Wformat-signedness`, and more) lives in the Makefile's `CFLAGS ?=`.
  Passing `CFLAGS=` replaces it, and meson only uses `warning_level=1`.
  Only a plain `make` checks what CI checks.
- `VERSION` is the single version source for both build systems. The
  static-binary job checks that the binary is not stamped `-dirty`, so
  build steps must not modify tracked files.

## Tests (bats, Linux, root)

CI (`.github/workflows/integration.yml`, job `conmon`) runs:

```sh
sudo mkdir -p /var/run/crio            # DEFAULT_SOCKET_PATH in src/config.h
sudo make test-binary                  # = make test → test/run-tests.sh
sudo test/run-tests.sh 07-attach.bats --filter 'stdin'   # one file / pattern
sudo test/run-tests.sh -r /usr/bin/crun                  # other runtime (default /usr/bin/runc)
sudo make test-coverage                # coverage.yml; runs `clean` first
```

- Requirements: `bats`, `socat`, runc or crun, and `podman`. The
  `setup_suite.bash` script pulls `registry.access.redhat.com/ubi10/ubi-micro`
  once to build the rootfs (override with `UBI10_MICRO_IMAGE`), so the
  tests need network access. Some tests also use `cc`,
  `journalctl` and cgroup v2, and use `jq` when it is installed.
- The CI job does not pass `--strict`, so a skipped test does not fail it.
  Check the output for skips. runc's CI also runs this suite with its own
  `CONMON_BINARY`/`RUNTIME_BINARY`, so do not hard-code paths.
- Write tests with the helpers in `test/test_helper.bash`:
  `setup_container_env`, `start_conmon_with_default_args`,
  `run_conmon_expecting_failure`, `wait_for_runtime_status`,
  `wait_for_conmon_exit`, `wait_for_log_line`, `wait_for_syncpipe_output`,
  `retry`. Do not `sleep` and then assert: many recent `test:` commits fix
  exactly that kind of race.
- For C-level tests: `test/oom_watch.c` does `#include "../src/cgroup.c"`
  to exercise static functions, and `test/waitpid_shim.c` is an
  `LD_PRELOAD` shim used to force a race deterministically. Prefer these
  over copying production logic into a test.
- CI also builds CRI-O and runs its integration suite against this conmon
  (job `cri-o`, with critest on and off). That job catches contract breaks.
- `make clean` also removes leftover `/tmp/conmon-test-*` directories.

## Lint (`.github/workflows/validate.yml`, gated by its `all-done` job)

| Job | Command | Notes |
|---|---|---|
| lint | `make fmt && git diff --exit-code` | Runs clang-format on `*.c`/`*.h` (`.clang-format`: Linux braces, tabs, 140 columns) and strips trailing whitespace from `.bats`. It uses GNU `sed -i`, so it breaks with BSD sed on macOS. |
| analyze | `make analyze` | clang `--analyze -Werror`. Three checkers are disabled because they do not model `__attribute__((cleanup))`. |
| cppcheck | `make cppcheck` | `--inline-suppr` is enabled. |
| shfmt / shellcheck | `make shfmt`, `make shellcheck` | These run pinned container images through `CONTAINER_ENGINE` (default `podman`). `make localshfmt`/`localshellcheck` use local binaries. Shell indentation is 4 spaces (`.editorconfig`). |
| meson-build | see Build | |

The `actionlint` and `zizmor` workflows lint `.github/workflows/`.
Actions are pinned to commit SHAs and use `persist-credentials: false`.
Keep both when you edit a workflow. Renovate bumps the versions marked
`# renovate:` in `hack/github-actions-setup`. The bats version there is
deliberately pinned at v1.13.0.

## Invariants and recurring bug classes

- **Signal handlers do almost nothing.** `on_sig_exit` forwards the signal
  with `kill()` or calls `self_pipe_wake()`. Calling `raise()` from a
  handler caused SIGABRT inside glibc's `ppoll` (issue #657). SIGCHLD
  arrives through a signalfd (`get_signal_descriptor`). Globals read in a
  handler are `volatile sig_atomic_t` (`container_pid`, `create_pid`).
- **Everything after the fork runs on the glib main loop.** Callbacks are
  registered with `g_unix_fd_add`. Returning `G_SOURCE_REMOVE` drops the
  watch for good: `17210cb` fixed OOM monitoring that silently stopped
  this way. For precise timers use `g_timeout_add`, not
  `g_timeout_add_seconds`, which can fire up to 250 ms early (`7f06cc8`).
- **Exiting:** `atexit` handlers do real work (`reap_children`,
  `do_exit_command`, `cleanup_socket_dir_symlink`). In forked children,
  use `_pexit`/`_pexitf`, which call `_exit`. Use `pexit*` when errno is
  meaningful (it appends `%m`) and `nexit*` when it is not (`503073c`).
- **fds:** open everything `O_CLOEXEC`. The caller passes the runtime's
  fds starting at 3 and conmon's own pipes after them, so
  `close_runtime_fds` must stop before the first of conmon's own fds
  (`e3395c0`, `ee497c9`). Inherited fds (Podman's reserved ports) are
  closed before the exit file is written, so the ports are free by the
  time the caller sees the exit.
- **exec semantics:** a non-zero runtime exit status during `--exec` is
  the command's exit code, not a failure. A missing pid file is how a
  runtime failure is detected (`05eff51`). Order-of-events races between
  the runtime reaper and the main loop recur (`488e60d`, the comments in
  `container_exit_cb`).
- **Logging edge cases:** rotation uses an `O_EXCL` temp file plus
  an `fcntl(F_SETLK)` record lock (`validate_and_lock_rotation`), not
  `flock(2)`. On Linux the two lock types do not exclude each other. A
  stale temp file must not stop rotation for good
  (`d37a16c`). Do not keep more than `--log-max-files` backups
  (`156323e`). A transient `EAGAIN` on stdin writes is backpressure, not
  an error (`3c441c1`).
- **Memory:** do not `_cleanup_free_` strings that GLib owns (for
  example `g_get_tmp_dir`, `52f8fd2`).

## Commits and PRs

- The subject is `area: imperative summary` in lowercase, where area is
  the file or subsystem: `logging:`, `cli:`, `conn_sock:`, `cgroup:`,
  `close_runtime_fds:`, `conmon:`, `test:`, `ci:`, `meson:`, `hack:`.
- The body explains the failure mode and why the fix is correct.
- Reference what is being fixed with kernel-style trailers:
  `Fixes: <sha> ("subject")` or `Fixes: #NNN`.
- Sign off with `git commit -s`. Most commits carry `Signed-off-by`.
- A bug fix ships with a bats regression test in the same commit.
- There is no PR template or CONTRIBUTING file in this repo.
