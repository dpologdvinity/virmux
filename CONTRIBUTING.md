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
- **No Memory Leaks:** All heap allocations must be paired with deterministic cleanups. Code is verified with AddressSanitizer (`-Db_sanitize=address`).
- **No Absolute Home Paths:** Never commit or hardcode absolute home paths. Always use `~`, `$HOME`, or repo-relative paths.
- **Secrets:** Never commit API keys, tokens, or local credentials.
