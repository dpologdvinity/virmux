# Fuzz Targets

libFuzzer harnesses for the virmux code that parses untrusted input. Each
`fuzz_<name>.cpp` explains in its header comment why that input is untrusted.

| Executable | Code under test | Seed corpus |
| --- | --- | --- |
| `fuzz_ipc_frame` | `decode_ipc_frame()` / `decode_ipc_header()` and `recv_ipc_message()` over a socketpair (cross-checked) | `corpus/ipc_frame` |
| `fuzz_daemon_request` | `parse_daemon_request()` and the registration / session-id / kill payload parsers | `corpus/daemon_request` |
| `fuzz_session_list_json` | `SessionRegistry::from_json()` (client-side `MSG_LIST_SESSIONS_RESP` parsing) | `corpus/session_list_json` |
| `fuzz_bench_report` | `parse_comparison_json()` + `format_comparison_table()` + JSON round trip | `corpus/bench_report` |
| `fuzz_size_bytes` | `parse_size_bytes()` (`virmux cas clean --max-bytes`) | `corpus/size_bytes` |

`fuzz_daemon_request` inputs start with a 2-byte message type in host byte
order (little-endian on x86-64), followed by the payload.

Not fuzzed: the `/proc` and sysfs readers in `src/memory/telemetry.cpp` and
`src/memory/ksm.cpp`. The kernel writes those files, so a sandboxed process
can't control them, and the parsing is a single bounded `std::from_chars` per
line.

## Build

Fuzzing needs clang (libFuzzer ships with it) and ASan + UBSan on every
target, so the core library and the harnesses use the same sanitizer runtime:

```bash
CC=clang CXX=clang++ meson setup build-fuzz \
  -Dfuzzing=true -Db_sanitize=address,undefined -Db_lundef=false
ninja -C build-fuzz
```

If you use `-Dfuzzing=true` with gcc, or leave out
`-Db_sanitize=address,undefined`, configure stops with an error. When fuzzing
is enabled, `virmux_core` is built with
`-fsanitize=fuzzer-no-link -fno-sanitize-recover=all`, so any UBSan report
aborts and libFuzzer records it as a crash. The default build
(`-Dfuzzing=false`) doesn't change.

## Replay the seed corpus (regression mode)

```bash
meson test -C build-fuzz --suite fuzz
```

Each test runs `fuzz_<name> -runs=0 tests/fuzz/corpus/<name>`. It executes
every corpus file once, doesn't mutate anything, and never writes to the
corpus directory.

## Fuzz locally

libFuzzer writes new interesting inputs into the **first** corpus directory
you give it. To keep the checked-in seeds clean, put a scratch directory
first:

```bash
mkdir -p /tmp/fuzz-ipc
./build-fuzz/fuzz_ipc_frame -max_total_time=60 /tmp/fuzz-ipc tests/fuzz/corpus/ipc_frame
```

You can also fuzz the checked-in corpus directly and let it grow:

```bash
./build-fuzz/fuzz_ipc_frame -max_total_time=60 tests/fuzz/corpus/ipc_frame
```

Useful flags: `-jobs=N -workers=N` (parallel runs), `-max_len=N` (default
4096), `-artifact_prefix=DIR/` (where `crash-*`, `leak-*`, `timeout-*` and
`oom-*` files go). Reproduce a finding with
`./build-fuzz/fuzz_<name> path/to/crash-<sha1>`.

## When a fuzzer finds a bug

1. Write a failing unit test in `tests/unit/` that reproduces it.
2. Fix the root cause in `src/`.
3. Copy the crashing input into `tests/fuzz/corpus/<name>/regress_<what>` so
   the `fuzz` suite replays it from then on.
