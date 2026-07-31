---
name: build
description: Configure and build the codicis C++ project with CMake. Use when asked to build, compile, or configure codicis, or after code changes to check they compile.
---

# Build codicis

`codicis` uses CMake (3.20+) and a C++20 compiler. Catch2 is vendored under
`third_party/catch2`, so the build needs no network.

## Standard build

Run from the repository root:

```
cmake -S . -B build
cmake --build build
```

The first command configures (only needed once, or after changing
`CMakeLists.txt` files); the second compiles. Both are safe to re-run.

## Useful variants

- Parallel build: `cmake --build build -j`
- Skip tests: `cmake -S . -B build -DCODICIS_BUILD_TESTS=OFF`
- Do not fail on warnings: `cmake -S . -B build -DCODICIS_WERROR=OFF`
- Release build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- Clean rebuild: `rm -rf build` then reconfigure.

## Notes

- The event backend is auto-detected: `kqueue` on macOS/BSD, `epoll` on Linux.
  The configure output prints which one was selected.
- Warnings are treated as errors by default; keep the tree warning-clean.
- To run the tests after building, use the `test` skill.
