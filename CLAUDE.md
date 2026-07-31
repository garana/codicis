# CLAUDE.md — codicis

Guidance for Claude Code (and humans) working in this repository. Keep this
file up to date with progress every phase.

## What this project is

`codicis` is a **central limit order book (CLOB)** with order matching and
processing, written in C++. It is a single process that accepts client
requests over an HTTP/S REST API and WebSockets, matches orders in memory,
and delegates all persistence and deep order-book storage to **child-process
helpers** over pipes.

This section is the authoritative record of the original requirements (the
source `initial.md` has been folded into this file and README.md):

- Central limit order book, order matching and processing, in C++.
- Support all known order types (see "Order types" below). Categories were
  approved with the user before implementation.
- Accept requests via: HTTP/S RESTful API, and WebSockets.
- Single process using an event loop: `epoll` on Linux, `kevent`/`kqueue` on
  BSD/macOS. A common abstraction wraps BOTH backends.
- No persistent storage in-process. Persistence is delegated to child-process
  **helpers**; bidirectional communication over pipes (helper stdin/stdout).
- Keep in memory (configurable limit) only the order-book levels nearest the
  top of book.
- New order flow: reported to us -> stored via storage helper -> then placed
  in memory if within the in-memory levels.
- Order update (from clients): if the order is in memory and already (fully or
  partially) filled -> reject. If not filled at all -> report to storage, and
  when it returns, store in memory if within the in-memory levels.
- As the top of book shifts to levels not in memory, pull those levels via the
  storage helpers.
- As orders match, report trades via the storage helpers. Once the storage
  helper confirms the results are committed, remove those orders/trades from
  the "processed queue".
- Configuration from a config file AND CLI flags. CLI flags take higher
  precedence. The same names are used in CLI flags and config files.
- Keep Claude sessions for future resume when Claude starts in this folder.
- C/C++ with CMake for build, Catch2 for unit tests.
- Keep README.md and CLAUDE.md up to date with current progress.
- Generate commits as work is done. **Do NOT add a Co-Authored-By line.**
- Write skills for build and test (see `.claude/skills/`).
- Prefer JSDoc-style comments.
- Use `.cc` for C++ source files (not `.cpp`) and `.h` for headers (not `.hh`).
- Use git for version control.

## Conventions

- Language: C++20. Compiler on the dev host: Apple clang 21 (arm64 macOS).
- File extensions: `.cc` sources, `.h` headers.
- Comments: JSDoc/Doxygen-style `/** ... @param ... @return ... */`. Only add
  or update comments where warranted; do not rewrite existing ones wholesale.
- 80-column width. Plain ASCII in Markdown tables, rows aligned to content.
- Warnings: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`
  with `-Werror` (toggle via `-DCODICIS_WERROR=OFF`).
- No floating point anywhere in the matching engine. Prices are integer ticks.
- Order time-priority uses a monotonic arrival sequence number, never wall
  clock time.
- Commits: one per green phase/milestone; no Co-Authored-By line.

## Architecture (summary)

Module dependency DAG (one direction; `core` has no I/O dependencies so it is
unit-testable in isolation):

```
util <- config
util <- event
util,event <- net
util,event <- ipc
util <- core (matching)
all of the above <- app (wiring/main)
```

- `util`   Result/Status, Buffer, logging, Clock.
- `config` OptionRegistry + file parser + CLI parser (CLI wins).
- `event`  EventLoop abstraction + KqueueLoop + EpollLoop (level-triggered).
- `net`    Hand-rolled HTTP/1.1 + RFC6455 WebSocket on the event loop.
- `ipc`    Pluggable child-process helper framework (text key=value AND binary
           codecs, pipelined), incl. the storage helper schema.
- `core`   Order model, dense-ladder order book, matching engine.
- `app`    main(), dependency-injection wiring net -> core -> ipc.

Key design decisions:
- Network layer is hand-rolled (no third-party HTTP/WS library).
- TLS/HTTPS uses a system crypto library (OpenSSL/LibreSSL) for the crypto
  layer only, behind a `SecureTransport` seam; HTTP/WS parsing stays
  hand-rolled. (Hardening phase.)
- Order book is a dense dynamic-array "ladder" (ring buffer indexed by tick
  offset) for O(1) price->level lookup, with intrusive FIFO lists per level.
- IPC is a generic helper framework so more helper types (trade reporter,
  last-known-price reporter, ...) can be added later. Pipelining is required.

The full master plan lives at
`~/.claude/plans/hashed-seeking-manatee.md`.

## Order types (approved scope, phased)

1. Core: Market, Limit, Stop/Stop-Market, Stop-Limit, Trailing Stop,
   Trailing Stop-Limit.
2. Time-in-force: GTC, DAY, GTD, IOC, FOK, AON, GTX.
3. Auction/session: MOO, LOO, MOC, LOC, OPG.
4. Display/liquidity: Iceberg/Reserve, Hidden, Post-Only, Reduce-Only,
   Min-Quantity, Discretionary.
5. Pegged: Primary Peg, Market Peg, Midpoint Peg, Pegged+Offset.
6. Linked/contingent: OCO, OTO, Bracket.

## To Design (deferred — revisit before/while building the relevant phase)

- **Store-queue with rollback.** Order/trade execution keeps a "to store"
  queue; an entry is removed only when the storage helper reports success. On a
  storage error, a rollback mechanism must undo the corresponding in-memory
  book mutation (and any dependent trades). The detailed design (compensation
  vs. optimistic-with-undo-log, cascade of dependent fills, client
  notification) is intentionally deferred.
- **Additional helper types.** Trade reporter, last-known-price reporter, etc.,
  added later on the same `Helper`/`HelperCodec` framework.
- **Sparse-book fallback.** The dense ladder's memory is proportional to the
  in-window tick span; add a hash-index fallback for pathologically sparse
  instruments.

## Build and test

See the `build` and `test` skills in `.claude/skills/`. In short:

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`epoll` is compile-guarded and cannot run on the dev Mac; it is validated on
Linux (CI/container) during hardening.

## Progress

- [x] P0 Skeleton and build system: CMake, Platform detection, vendored
      Catch2 (offline-safe), `util` library (Result/Status, Buffer, logging,
      Clock), first passing test, README/CLAUDE, skills. `ctest` green.
- [ ] P1 Config subsystem
- [ ] P2 Event loop (kqueue + epoll)
- [ ] P3 Net HTTP
- [ ] P4 Net WebSocket
- [ ] P5 IPC helper framework
- [ ] P6 App wiring and core seam
- [ ] OT0-OT8 Order types (matching engine)
- [ ] H1-H2 Hardening (TLS + robustness)
