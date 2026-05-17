# Contributing to Manwe

This document covers the practical bits: building, running tests,
coding style, and the PR flow. For architecture, see the per-part docs
in `docs/`.

## Building

Manwe is a CMake project targeting C++23. Recent AppleClang, gcc 12+,
and clang 15+ all work.

```bash
# Typical configure + build (Debug)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# Release
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Dependencies

| Component | Dependency | Notes |
|---|---|---|
| Yarn / Wire | none | header-only or self-contained |
| Soccer (TLS) | LibreSSL `libtls` | optional; auto-detected by CMake |
| Soccer (io_uring backend, Linux) | `liburing` | optional; opt-in via `-DYARN_USE_IO_URING=ON` |

On macOS: `brew install libressl`. On Debian/Ubuntu: `apt-get install
libtls-dev liburing-dev`.

### Configure-time options

| Option | Default | What it does |
|---|---|---|
| `YARN_USE_IO_URING` | OFF | Replace the default epoll backend on Linux with io_uring (via liburing). |
| `YARN_IO_URING_SQPOLL` | OFF | When io_uring is enabled, set `IORING_SETUP_SQPOLL` so the kernel polls the SQ. Subsumes `YARN_USE_IO_URING`. |

## Running tests

```bash
./bin/YarnTests
```

The suite is 62 cases (as of v0.3); typical wall time ~250 ms. Run it
five times in a row to catch flakes:

```bash
for i in $(seq 1 5); do ./bin/YarnTests > /dev/null && echo "run $i ok"; done
```

The TLS test will skip if `openssl` isn't on `PATH` (it generates a
self-signed cert at runtime). The ICMP ping integration test will skip
unless `euid == 0`.

## Coding style

The project uses C++23 with the following conventions:

### Doxygen comments

Every class, public method, non-trivial private method, and non-obvious
field gets a Doxygen block. Memory orderings and lock invariants are
explained in `@note` lines. Example:

```cpp
/**
 * @brief One sentence summary.
 *
 * Longer description if needed.
 *
 * @param x What x is.
 * @return What this returns.
 */
```

When the WHY isn't obvious from the code, write it in the comment. When
the code is the documentation, don't.

### No magic values

Buffer sizes, timeouts, retry counts, thresholds, queue capacities —
name them as `constexpr`:

```cpp
constexpr std::size_t kReadBufBytes = 4096;
std::array<std::byte, kReadBufBytes> buf{};
```

Test data values (`EXPECT_EQ(compute(2), 4)`) are exempt — the inline
values are the assertion.

### No emojis

Do not add emojis to code, comments, or commit messages. Markdown docs
likewise.

### Doc structure

Architecture docs in `docs/` follow a dual-section layout: **Architecture**
(why / invariants / lock discipline) followed by **Using it** (worked
examples). Match the existing files when adding new ones.

### Header layout

Each library lives at `<Name>/includes/` (headers) and `<Name>/src/`
(sources). Library targets export their include directory via
`target_include_directories(... PUBLIC ...)` with both
`$<BUILD_INTERFACE>` and `$<INSTALL_INTERFACE>` generator expressions.

## Adding tests

Tests live in `YarnTests/main.cpp` (one consolidated suite per the
project's testing strategy — see `docs/testing.md`). Add new cases via
the `TEST(name)` macro:

```cpp
TEST(my_new_behaviour) {
    EXPECT_EQ(compute(2), 4);
    EXPECT_TRUE(condition);
    EXPECT_THROWS_AS(throws_op(), MyException);
}
```

For concurrency tests, use the `wait_for` helper at the top of the
file rather than unconditional `sleepFor`.

When testing a coroutine, prefer named free-function coroutines over
lambda-bodied coroutines: lambda closures are temporaries that don't
survive past the enclosing full-expression, but their captures live in
the coroutine frame via a dangling `this` — that's a UB trap. Pass
state as function parameters instead.

## Pull request flow

1. Write the code + tests + doc updates in one branch.
2. Run the suite locally (5 iterations).
3. Open a PR. CI runs the suite on macOS and Linux (default epoll +
   io_uring).
4. Bin artifacts (`bin/lib*.a`, `bin/YarnTests`, etc.) are tracked.
   Include the regenerated binaries in your commit.

## Static destructor order

Manwe relies on construction-order guarantees:

- `Yarn::Yarn` is constructed at first `Yarn::instance()`.
- `Reactor::Reactor` calls `Yarn::instance()` before completing, so
  Yarn is constructed first → destructed last.
- The Reactor's destructor stops its event loop thread before any Yarn
  state is torn down, so resumption scheduling stays valid until join.

If you add another singleton that depends on Yarn or Reactor, follow
the same pattern: call `Yarn::instance()` from its constructor's first
line to force the ordering.

## Issue / bug triage

Concurrency bugs in Manwe usually fall into one of:

- A lambda-coroutine lifetime trap (closure dies before coroutine body
  runs).
- A missing memory ordering (e.g. relaxed where release/acquire is
  needed).
- A lock-order inversion across `cmu` / `schedMu` / `mu`.
- A self-join in a destructor that runs on a worker thread.

If you suspect one of these, the existing tests in YarnTests should
catch it — if not, add the reproducer and a fix as a single PR.
