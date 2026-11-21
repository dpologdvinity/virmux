# Contributing to `virmux`

Thank you for your interest in contributing to `virmux`!

---

## Development Setup

1. **Prerequisites:**
   Ensure development tools and dependencies are installed:

   ```bash
   sudo apt-get update
   sudo apt-get install -y gcc meson ninja-build libncurses-dev
   ```

2. **Clone and Configure:**

   ```bash
   git clone https://github.com/dpologdvinity/virmux.git
   cd virmux
   meson setup build
   ninja -C build
   ```

3. **Running Tests:**

   ```bash
   ninja -C build test
   ```

4. **Fuzzing (optional, clang only):** libFuzzer harnesses for the IPC,
   daemon-request, session-list, bench-report and size parsers live in
   `tests/fuzz/`. See [`tests/fuzz/README.md`](tests/fuzz/README.md):

   ```bash
   CC=clang CXX=clang++ meson setup build-fuzz \
     -Dfuzzing=true -Db_sanitize=address,undefined -Db_lundef=false
   meson test -C build-fuzz --suite fuzz
   ```

---

## Continuous Integration

Every workflow guards against a specific way virmux can break. Each one can be
reproduced locally with the command shown.

| Workflow | Why it exists | Reproduce locally |
| -------- | ------------- | ----------------- |
| **Lint** (`lint.yml`) | Formatting, clang-tidy bug-pattern checks, docs and workflow linting. Runs in parallel with builds so a style nit never hides a real failure. | See [Local lint tools](#local-lint-tools) |
| **CI** (`ci.yml`) | Ubuntu 22.04/24.04 × GCC/Clang × debug/release, native **arm64** (the interceptor issues architecture-specific raw syscalls), **Fedora latest** (newest GCC/Clang warnings, tmpfs `/tmp`) and a **coverage** report in the job summary. | `meson setup build -Dwarning_level=3 -Dwerror=true && meson test -C build` |
| **Sanitizers** (`sanitizers.yml`) | ASan+UBSan (UB fails the build) and TSan for the multithreaded, forking daemon. | `CC=clang CXX=clang++ meson setup build-tsan -Db_sanitize=thread -Db_lundef=false && meson test -C build-tsan` |
| **Fuzz** (`fuzz.yml`) | libFuzzer on every parser of untrusted socket/JSON input: 2 minutes per target on PRs, 15 minutes nightly with an accumulated corpus. | See [`tests/fuzz/README.md`](tests/fuzz/README.md) |
| **CodeQL** (`codeql.yml`) | Taint-tracking security analysis across the socket, `/proc` and mount boundaries; weekly re-scan for new queries. | GitHub only |
| **Release** (`release.yml`) | On a `vX.Y.Z` tag: smoke-tested x86_64/arm64 tarballs, `SHA256SUMS` and SLSA provenance (`gh attestation verify`). The tag must match `version` in `meson.build`, and `CHANGELOG.md` must have a section for it. | `meson install -C build --destdir stage` |
| **Claude** (`claude.yml`) | On-demand assistant: mention `@claude` in an issue, PR comment or review to diagnose CI failures, answer questions or push a fix. Only the owner and collaborators can trigger it; automatic review is left to CodeRabbit. | GitHub only |

### Local lint tools

clang-format and clang-tidy output differs between releases, so CI pins the
versions below. Install the same versions locally:

```bash
uv tool install clang-format==23.1.3
uv tool install clang-tidy==23.1.3
npm ci

git ls-files -z '*.c' '*.cpp' '*.h' '*.hpp' ':!:tests/unity/**' | xargs -0 clang-format --dry-run --Werror
CC=clang CXX=clang++ meson setup build-tidy
run-clang-tidy.py -p build-tidy -quiet '^(?!.*/tests/unity/).*/(src|cli|tests)/.*\.(c|cpp)$'
npm run lint:md
actionlint
```

If the Fuzz workflow fails on your pull request, the uploaded reproducer may
expose a bug that predates your change. It is still a real bug: fix it or open
an issue with the reproducer attached.

GitHub Actions are pinned to commit SHAs; Dependabot updates them weekly in a
single grouped pull request.

---

## Branching & Commit Standards

- **Branching:** Create feature branches off `main` with descriptive names: `feat/<feature-name>`, `fix/<bug-name>`, or `docs/<topic>`.
- **Commit Style:** Always use [Conventional Commits](https://www.conventionalcommits.org/):
  - `feat(memory): implement process_madvise pageout dispatcher`
  - `fix(sched): resolve race condition in token bucket release`
  - `docs(specs): add GPU multi-tenancy architecture section`
  - `test(core): add Unity unit tests for IPC codec`

---

## Code Quality & Hygiene

- **Standard:** C17 standard compliance with `-Wall -Wextra -Wpedantic -Werror`.
- **Static Analysis:** `clang-tidy` is a zero-findings gate using the curated checks in `.clang-tidy` (`bugprone-*`, `clang-analyzer-*`, `performance-*`; every finding is an error). See [Local lint tools](#local-lint-tools) for the command. Fix findings at the root cause. Suppress only genuine false positives, as `// NOLINTNEXTLINE(check-name): reason`.
- **No Memory Leaks:** All heap allocations must be paired with deterministic cleanups. Code is verified with AddressSanitizer (`-Db_sanitize=address`).
- **No Absolute Home Paths:** Never commit or hardcode absolute home paths. Always use `~`, `$HOME`, or repo-relative paths.
- **Secrets:** Never commit API keys, tokens, or local credentials.
