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
curl -s -X POST --data 'symbol=BTC&side=sell&type=limit&price=100&qty=10' \
    localhost:8080/orders
curl -s 'localhost:8080/book?symbol=BTC'
curl -s -X POST --data 'symbol=BTC&side=buy&type=limit&price=105&qty=4' \
    localhost:8080/orders   # crosses; trades at the maker price 100
curl -s -X POST --data 'symbol=BTC&id=1' localhost:8080/orders/cancel
```

Every order/cancel carries a `symbol`; each instrument has its own order book.

WebSocket clients connect to `net.ws_port` (default 8081), send the opening
RFC 6455 handshake, then submit orders as text frames using the same
form-encoded body; the JSON result is streamed back as a text frame. For
example: `side=sell&type=limit&price=100&qty=10`.

A client can also subscribe to a symbol's market-data stream by sending
`action=subscribe&symbol=BTC`. It then receives a top-of-book + trades update
whenever that book changes, e.g.
`{"type":"md","symbol":"BTC","bid":null,"bid_qty":0,"ask":100,"ask_qty":6,"trades":[{"price":100,"qty":4}]}`.
Send `action=unsubscribe&symbol=BTC` to stop.

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

## Authentication and authorization

codicis separates **authorization** (does this user own this order?) from
**authentication** (is the request really from that user?). Authorization is
always enforced in-process: every order is assigned an opaque external UUID
handle owned by a user UUID, and a cancel succeeds only for the owner. The
owner UUID is **never** taken from the request body -- it comes only from an
authenticated source (or is anonymous when authentication is disabled).

Authentication is a configurable layer, because two reasonable operating
principles pull in opposite directions:

1. **Block unexpected traffic as early as possible**, so it never reaches the
   app/network layers. This favors trusting an authenticating edge proxy.
   - Pro: least work in codicis, one choke point, keeps crypto off the
     matching path (which pairs with offloading TLS to the edge).
   - Con: relies on the network boundary being trustworthy; a misconfigured
     bind is a full bypass (i.e. higher cross-layer trust).
2. **Reduce cross-layer trust**, by having codicis verify identity itself.
   - Pro: does not trust the edge; supports revocation via a token store.
   - Con: puts validation (often cryptographic) near the process, needing a
     helper pool + cache to keep the matching path free.

Two mechanisms implement these, and they are **not exclusive** -- an integrator
can enable both at once for defense in depth:

- **Option A (trusted header).** An authenticating edge validates the caller
  and forwards the user UUID in a header (e.g. `X-User-Id`); codicis trusts it.
- **Option B (auth helper).** codicis forwards a credential header (e.g.
  `Authorization`) to a pool of auth helper child processes, which validate it
  and return the user UUID (with an optional `not_after` expiry). Results are
  cached (positive + negative), and concurrent lookups of the same credential
  are coalesced into one helper request.

When **both** are enabled, a request must pass both and they must resolve to
the **same** UUID, otherwise it is rejected (403). A missing or malformed
identity is 401; a failed or mismatched validation is 403. `GET /health` and
`GET /book` require no identity. Over WebSocket, identity is resolved once from
the handshake headers and applies to every order on that connection.

Configuration keys (all under `auth.`; CLI/file share names):

| Key                               | Purpose                                |
| --------------------------------- | -------------------------------------- |
| `auth.header.enabled`             | Enable Option A (trusted header)       |
| `auth.header.name`                | Header carrying the user UUID          |
| `auth.helper.enabled`             | Enable Option B (auth helper pool)     |
| `auth.helper.cmd`                 | Command to launch each auth helper     |
| `auth.helper.concurrency`         | Number of helper processes (pool size) |
| `auth.helper.pipelining`          | Helper accepts overlapping requests    |
| `auth.helper.pipeline_depth`      | Max in-flight per helper (1 if not)    |
| `auth.helper.credential_header`   | Header value forwarded to the helper   |
| `auth.helper.request_timeout_ms`  | Per-request helper timeout             |
| `auth.cache.max_entries`          | Positive cache entry cap               |
| `auth.cache.max_bytes`            | Positive cache byte budget (0=off)     |
| `auth.cache.ttl_ms`               | Positive entry lifetime cap            |
| `auth.cache.negative_max_entries` | Negative cache entry cap               |
| `auth.cache.negative_max_bytes`   | Negative cache byte budget (0=off)     |
| `auth.cache.negative_ttl_ms`      | Negative entry lifetime (max age)      |

Recommended values: leave both mechanisms off by default (anonymous). Set
`concurrency` near the number of CPU cores when the helper does crypto; set
`pipeline_depth` to about 8 when the helper pipelines, else 1. A positive entry
lives until the helper's `not_after`, capped by `ttl_ms` (30-120 s is typical).
Negative (denied) entries have no `not_after`, so `negative_ttl_ms` is their
sole age bound -- keep it short (a few seconds) so a revoked or mistyped
credential clears quickly.

Each cache is bounded by an entry count (`max_entries`) and an optional byte
budget over key+value sizes (`max_bytes`, 0 = no byte limit); whichever binds
first triggers tail eviction. The positive and negative caches are separate so
a burst of invalid credentials cannot evict validated entries. Note eviction is
lazy on read plus these size caps -- age (TTL) bounds staleness, not memory, so
size the caps to bound resident memory.

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
