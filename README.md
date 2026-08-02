# codicis

A central limit order book (CLOB) with order matching and processing, written
in C++. `codicis` runs as a single process, accepts client requests over an
HTTP/S REST API and WebSockets, matches orders in memory, and delegates all
persistence to child-process helpers communicating over pipes.

## Status

Under active development. The full infrastructure (config, event loop, HTTP +
WebSocket, IPC helper framework) and the matching-engine foundation
(Market/Limit with core time-in-force) are complete, wired into a runnable
`codicis` server with a REST API. The suite is green. See the roadmap below and
`CLAUDE.md` for details.

## Running

```
cmake -S . -B build && cmake --build build
./build/src/app/codicis \
    --net.http_port=8080 \
    --storage.helper_cmd=build/tools/storage_helper/codicis_storage_helper
```

Then, for example:

```
curl -s localhost:8080/health
curl -s -X POST --data 'side=sell&type=limit&price=100&qty=10' \
    localhost:8080/orders
curl -s localhost:8080/book
curl -s -X POST --data 'side=buy&type=limit&price=105&qty=4' \
    localhost:8080/orders   # crosses; trades at the maker price 100
curl -s -X POST --data 'id=1' localhost:8080/orders/cancel
```

WebSocket clients connect to `net.ws_port` (default 8081), send the opening
RFC 6455 handshake, then submit orders as text frames using the same
form-encoded body; the JSON result is streamed back as a text frame. For
example: `side=sell&type=limit&price=100&qty=10`.

Every option is also settable via a config file (see
`config/codicis.example.conf`); CLI flags override file values.

## Design highlights

- **Single process, event-driven.** One event loop drives all sockets and
  helper pipes. A common abstraction wraps `kqueue` (BSD/macOS) and `epoll`
  (Linux).
- **Hand-rolled network layer.** HTTP/1.1 and RFC6455 WebSocket are implemented
  directly on the event loop with no third-party HTTP/WS libraries. HTTPS/WSS
  use a system crypto library for TLS only (planned hardening phase).
- **No in-process persistence.** Persistence and deep order-book levels are
  delegated to pluggable child-process helpers over pipes, with pipelined
  request/response and both a text `key=value` and a binary wire encoding.
- **In-memory top of book.** Only the levels nearest the top of book are kept
  in memory (configurable); deeper levels are pulled on demand from the storage
  helper. The book is a dense "ladder" giving O(1) price-to-level lookup.
- **Deterministic matching.** Integer-tick prices (no floating point), price-
  time priority using a monotonic arrival sequence number.

## Order types (planned, phased)

| Category           | Types                                                       |
|--------------------|-------------------------------------------------------------|
| Core               | Market, Limit, Stop, Stop-Limit, Trailing Stop, Trailing SL |
| Time-in-force      | GTC, DAY, GTD, IOC, FOK, AON, GTX                           |
| Auction / session  | MOO, LOO, MOC, LOC, OPG                                     |
| Display / liquidity| Iceberg, Hidden, Post-Only, Reduce-Only, Min-Qty, Discretion|
| Pegged             | Primary Peg, Market Peg, Midpoint Peg, Pegged+Offset        |
| Linked / contingent| OCO, OTO, Bracket                                          |

## Building

Requirements: a C++20 compiler and CMake 3.20+. Catch2 is vendored, so the
build works offline.

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Options:

- `-DCODICIS_BUILD_TESTS=OFF` to skip tests.
- `-DCODICIS_WERROR=OFF` to not treat warnings as errors.

For Claude Code users, the `build` and `test` skills wrap these commands.

## Configuration

Configuration comes from a config file and/or CLI flags; CLI flags take
precedence and share the same names as file keys (e.g. `net.http_port`). See
`config/codicis.example.conf`. (Config subsystem lands in phase P1.)

## Repository layout

```
include/codicis/   Public headers (.h), one folder per subsystem.
src/               Implementation (.cc), one folder per subsystem.
tools/             Reference child-process helpers.
tests/             Catch2 unit tests.
cmake/             Build helpers (platform detection, Catch2).
third_party/       Vendored dependencies (Catch2 amalgamated).
config/            Example configuration.
```

## Roadmap

P0 build skeleton (done) -> P1 config -> P2 event loop -> P3 HTTP ->
P4 WebSocket -> P5 IPC helpers -> P6 wiring/core seam -> OT0-OT8 order types ->
H1-H2 hardening (TLS, robustness).
