# virmux

**Run heavy commands without freezing your machine.**

[![CI](https://github.com/dpologdvinity/virmux/actions/workflows/ci.yml/badge.svg)](https://github.com/dpologdvinity/virmux/actions/workflows/ci.yml)
[![Sanitizers](https://github.com/dpologdvinity/virmux/actions/workflows/sanitizers.yml/badge.svg)](https://github.com/dpologdvinity/virmux/actions/workflows/sanitizers.yml)
[![Lint](https://github.com/dpologdvinity/virmux/actions/workflows/lint.yml/badge.svg)](https://github.com/dpologdvinity/virmux/actions/workflows/lint.yml)
[![Fuzz](https://github.com/dpologdvinity/virmux/actions/workflows/fuzz.yml/badge.svg)](https://github.com/dpologdvinity/virmux/actions/workflows/fuzz.yml)
[![CodeQL](https://github.com/dpologdvinity/virmux/actions/workflows/codeql.yml/badge.svg)](https://github.com/dpologdvinity/virmux/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform: Linux / WSL2](https://img.shields.io/badge/Platform-Linux%20%7C%20WSL2-orange.svg)](https://learn.microsoft.com/en-us/windows/wsl/)

Test suites, compilers and AI coding agents each assume they own the whole
computer: they start a worker for every CPU core and size their memory use to
all of your RAM. Run a few of them at once and your laptop grinds to a halt.
On WSL2, the whole Windows desktop can lock up.

`virmux` is a command-line tool you put in front of those commands. It keeps
each one on a leash so your editor and shell stay responsive.

```bash
virmux run --cores 2 --mem 2G -- npm test
```

## What it does

**It tells the command your machine is smaller than it is.** Most tools decide
how many workers to start by asking how many cores and how much memory exist.
Inside `virmux run`, they get the answer you chose:

```console
$ nproc
10
$ virmux run --cores 2 -- nproc
2

$ grep MemTotal /proc/meminfo
MemTotal:       25644240 kB
$ virmux run --mem 2G -- grep MemTotal /proc/meminfo
MemTotal:        2097152 kB
```

So `npm test`, `make -j$(nproc)`, `pytest -n auto`, PyTorch and friends start 2
workers instead of 10, with no changes to your project. This works for every
user, with no special permissions. It covers programs that use the system C
library (Node, Python, most C/C++ and Rust tools) and anything that reads
`OMP_NUM_THREADS`. The command is also pinned to that many real CPUs, so Go and
other statically linked programs count the right number of cores too (pass
`--no-pin` to keep all CPUs available).

**It also:**

- **Enforces the limits.** From an ordinary desktop terminal, each run gets its
  own cgroup in the part of the cgroup tree systemd hands to your user. Going
  over `--mem` gets the command killed instead of your machine swapping to a
  halt, and it gets at most `--cores` CPUs' worth of time. On machines with four
  or more CPUs, the two lowest are left free for your shell.
- **Sacrifices the command first when memory runs out.** If the machine runs out
  of memory, Linux kills the command running under `virmux` instead of your
  editor or terminal.
- **Caps runaway process creation.** `--max-pids` stops a fork bomb or a
  misbehaving build from spawning thousands of processes.
- **Freezes idle sessions.** Pause and resume a session by hand, or let the
  background helper freeze sessions that have gone idle, so they stop using
  CPU until you need them again.
- **Gives each run a throwaway copy of your project.** With `--ephemeral`, the
  command sees your repository but its writes go to a temporary layer that is
  discarded afterwards. Useful for running several AI agents on one checkout
  without them overwriting each other.
- **Shows everything in a dashboard.** `virmux top` lists running sessions and
  lets you freeze or kill them with a keypress.

## What needs extra setup

Some features depend on permissions or kernel settings that a normal Linux or
WSL2 user does not have by default. `virmux` falls back gracefully and tells
you when that happens.

| Feature | Works out of the box | Needs extra setup |
| ------- | -------------------- | ----------------- |
| Smaller core and memory counts reported to the command | Yes | |
| Command killed first when memory runs out | Yes | |
| Process-count cap (`--max-pids`) | Yes | |
| Freeze and resume sessions | Yes | |
| Throwaway project copies (`--ephemeral`) | Yes, where unprivileged user namespaces are enabled (Ubuntu, Debian, WSL2) | |
| Pinning to `--cores` real CPUs (`--no-pin` turns it off) | Yes | |
| Enforced memory, CPU-time and process limits | Yes, from a terminal in a systemd desktop session (anything running under the systemd user manager) | ssh and console logins sit outside the user manager: run `systemd-run --user --scope virmux run ...`. Without it, `virmux` prints one warning and only the reported counts above apply. `scripts/doctor.sh` shows which case you are in. |
| Sharing identical memory pages between sessions (KSM) | | Enable KSM (`echo 1 \| sudo tee /sys/kernel/mm/ksm/run`) and set `VIRMUX_KSM_AUTO_MERGE=1` |
| Compressed swap in RAM (zram) | | Set up zram yourself; `virmux stats` and `virmux pack` then use it |
| Swapping out a session's idle memory (`pack`, auto-freeze) | | The `CAP_SYS_NICE` capability, for example running as root. Without it the kernel refuses ([#5](https://github.com/dpologdvinity/virmux/issues/5)). |
| Full process snapshots (`checkpoint` / `restore`) | | [CRIU](https://criu.org/) with root privileges. Without it, checkpoint falls back to freezing the session. |

Run `bash scripts/doctor.sh` to see which of these your machine supports.

## Install

Requires Linux kernel 5.10 or newer (Ubuntu 22.04+, Debian 12+ or WSL2),
`meson`, `ninja`, GCC 13+ or Clang, and the ncurses development headers.

```bash
sudo apt-get install -y build-essential meson ninja-build libncurses-dev
git clone https://github.com/dpologdvinity/virmux.git
cd virmux
meson setup build --buildtype=release
ninja -C build
sudo meson install -C build   # optional: installs virmux to /usr/local
```

Tagged versions will also ship prebuilt x86_64 and arm64 tarballs on the
[Releases page](https://github.com/dpologdvinity/virmux/releases) (none
published yet). Unpack one anywhere and run `bin/virmux`.

## Usage

```bash
# Run a command limited to 2 cores and 2 GB
virmux run --cores 2 --mem 2G -- npm test

# Keep every CPU available (still capped at 2 CPUs' worth of time)
virmux run --cores 2 --no-pin -- make -j8

# Run against a throwaway copy of a repository
virmux run --ephemeral --workspace ~/code/my-repo -- npx playwright test
```

To track sessions, start the background helper once. Every `virmux run` after
that registers with it:

```bash
virmux daemon                 # add --auto-freeze to freeze idle sessions
virmux list                   # show sessions
virmux freeze 1               # pause session 1
virmux thaw 1                 # resume it
virmux kill 1                 # stop it and everything it started
virmux top                    # interactive dashboard
```

`virmux top` (shown here with `--headless`, which prints one snapshot):

```text
+------------------------------------------------------------------------------+
| VIRMUX v0.1.0  [Linux / 10 Cores / 24.5G RAM]               Host Load: 13.99 |
| Dedicated Shield: Cores 0-1 (Fluid)   | Worker Pool: Cores 2-9 (Managed)     |
| KSM Dedup: 0 B Saved  | zram: 0 B (1.0x) | Sessions: 2                       |
+------------------------------------------------------------------------------+
| S# | NAME        | STATE   | V-CORES | RAM (RSS) | DEDUP | COMMAND           |
+------------------------------------------------------------------------------+
|>1  | sleep       | RUNNING | 2       | 3.8 MiB   | 0 B   | sleep 20          |
| 2  | sleep       | RUNNING | 4       | 3.7 MiB   | 0 B   | sleep 20          |
+------------------------------------------------------------------------------+
| [j/k] Navigate  [Space] Freeze/Thaw  [K] Kill  [q] Quit                      |
+------------------------------------------------------------------------------+
```

Other commands: `virmux stats` (memory overview), `virmux pack` (ask the
kernel to swap out a process's idle memory), `virmux checkpoint` / `restore`,
`virmux cas` (build artifact cache) and `virmux bench` (stress benchmark). Run
`virmux <command> --help` for details.

## How it compares

- **`nice` / `taskset`** change priority or pin cores, but the command still
  starts one worker per core it can see.
- **`systemd-run --user --scope -p MemoryMax=… -p CPUQuota=…`** enforces hard
  limits, but the command still sees all of your cores and RAM, so it starts
  too many workers and then gets throttled or killed.
- **`.wslconfig` memory limits** cap the whole WSL2 VM, not individual jobs.

`virmux` combines per-command limits with changing what the command *sees*, so
well-behaved tools size themselves correctly from the start.

## Status

Early development (v0.1.0). Linux and WSL2 only. Expect rough edges and open an
[issue](https://github.com/dpologdvinity/virmux/issues) when you hit one.

Known gaps:

- `virmux run --heavy` is meant to limit how many heavy jobs run at once, but
  separate `virmux run` commands do not coordinate yet
  ([#4](https://github.com/dpologdvinity/virmux/issues/4)).
- Without `CAP_SYS_NICE`, `virmux pack` reports memory as reclaimed even when the
  kernel refused to swap it out
  ([#5](https://github.com/dpologdvinity/virmux/issues/5)).

## Learn more

- [ARCHITECTURE.md](ARCHITECTURE.md) explains how it works: the preloaded
  library that changes reported hardware, the supervisor, cgroups, namespaces
  and the memory subsystem.
- [`docs/specs/`](docs/specs/) has detailed design specs for each subsystem.
- [CONTRIBUTING.md](CONTRIBUTING.md) covers development setup, tests and CI.

## License

MIT. See [LICENSE](LICENSE).
