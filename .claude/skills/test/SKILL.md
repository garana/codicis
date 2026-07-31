---
name: test
description: Build and run the codicis Catch2 unit tests via ctest. Use when asked to test codicis, run the test suite, or verify changes pass tests.
---

# Test codicis

Tests use Catch2 (vendored) and are driven by ctest. Each subsystem has its own
test binary registered with ctest.

## Run the full suite

From the repository root:

```
cmake --build build
ctest --test-dir build --output-on-failure
```

The first line ensures binaries are current (configure first with the `build`
skill if the `build/` directory does not exist yet). `--output-on-failure`
prints the Catch2 output for any failing test.

## Useful variants

- Verbose: `ctest --test-dir build -V`
- Run one binary directly (full Catch2 CLI): `./build/tests/test_util`
- Filter by tag within a binary: `./build/tests/test_util "[buffer]"`
- List test cases: `./build/tests/test_util --list-tests`
- Parallel: `ctest --test-dir build -j`

## Notes

- If configuration is missing, run the `build` skill first
  (`cmake -S . -B build`).
- A green run prints `100% tests passed`. Investigate any non-zero failures
  before committing; commits are made only on a green suite.
