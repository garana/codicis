---
name: tidy
description: Run clang-tidy static analysis on the codicis sources. Use when asked to lint, run clang-tidy, or check code for bug patterns beyond compiler warnings.
---

# clang-tidy for codicis

Static analysis with clang-tidy, configured by the repository `.clang-tidy`
(bugprone / performance / portability / clang-analyzer / misc / modernize, with
the noisiest stylistic checks disabled). Findings are advisory -- they are not
build errors.

The build always exports `build/compile_commands.json`
(`CMAKE_EXPORT_COMPILE_COMMANDS`), and on macOS pins the SDK sysroot so
Homebrew's clang-tidy can find system headers.

## Standalone (recommended)

Configure once so the compilation database exists, then lint specific files:

```
cmake -S . -B build
/opt/homebrew/opt/llvm/bin/clang-tidy -p build src/core/order_book.cc
```

`clang-tidy` is not on `PATH`; it ships with Homebrew LLVM at
`/opt/homebrew/opt/llvm/bin/clang-tidy`. Lint several files by listing them, or
a subsystem with a glob (e.g. `src/net/*.cc`).

## During the build (opt-in, slower)

```
cmake -S . -B build -DCODICIS_CLANG_TIDY=ON
cmake --build build
```

This runs clang-tidy on every compiled translation unit. It is OFF by default
so the normal `build`/`test` skills stay fast; the vendored Catch2 amalgamation
is excluded.

## Notes

- Apply a check's suggested fix with `--fix` (review the diff afterwards).
- Keep `bugprone-implicit-widening-of-multiplication-result` findings in mind
  for price x quantity math -- they flag exactly the 64-bit overflow class
  discussed for notional values.
- `.clang-format` (Google style, 80 columns) matches the codebase; run
  `clang-format -i <file>` to reformat.
