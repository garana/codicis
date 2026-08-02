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
- **Separate reader helper for pull-levels-on-demand.** Use a second
  `HelperClient` instance (same class/framework as the writer) for pulling
  non-resident levels, distinct from the writer/committer connection.
  Rationale: the level set being *read* (deep, non-resident levels) and the
  set being *written* (top-of-book orders/trades/commits) are disjoint -- a
  partition in set-theory terms -- so a read never depends on a pending write
  and must not queue behind slow write acks/commits on the same pipe. Two
  connections remove that head-of-line blocking; the writer keeps strict
  report-before-place/commit ordering while pulls run in parallel. Caveat: the
  partition is exact only away from the resident/non-resident boundary. When a
  level migrates (a pull promotes deep -> resident, or eviction demotes
  resident -> deep), the handoff must be serialized so a pull can't race a
  concurrent write to the same range (and mind read-your-writes if the backend
  is eventually consistent). Ties into the deferred top-of-book windowing that
  OT8's pull path needs.
- **Sparse-book fallback.** The dense ladder's memory is proportional to the
  in-window tick span; add a hash-index fallback for pathologically sparse
  instruments.
- **Move Order's conditional axes out-of-line (owning pointer).** Measured on
  arm64/libc++, the three inline `std::optional<TriggerSpec/PegSpec/LinkSpec>`
  members are 104 of `Order`'s 192 bytes (54%) and are disengaged for every
  plain limit order (they are stored inline, always reserved). Replace them
  with a single owning pointer on `Order` to a `Conditional` struct that holds
  the trigger/peg/link payloads together, allocated only for the rare
  stop/peg/linked/bracket orders (a bracket needs trigger + link at once, so
  they must co-live in one allocation, not a variant). This shrinks a plain
  `Order` to ~96 bytes -- halving the hot-path cache footprint of the book and
  the `orders` map -- while conditional orders pay one heap allocation + an
  indirection off the hot limit path. Use an owning smart pointer (deep-copy on
  Order copy so value semantics are preserved) rather than a separate side
  table. Do this when OT3-OT6 start populating those payloads. Cheaper partial
  win available first: reorder payload fields to group the 1-byte tags and cut
  internal padding (e.g. `TriggerSpec` 40 -> 32 bytes).
- **Intrusive book + Order pool (decided).** Make the book intrusive: `Order`
  carries its own doubly-linked prev/next hooks and lives in a fixed-size Order
  pool -- a slab allocator over `mmap`'d chunks (huge pages where available;
  LIFO free-list; O(1) alloc/free; single-threaded core, so no locking). The
  price level's FIFO becomes an intrusive list threaded through the Order
  nodes, removing the separate `std::list<OrderId>` node allocations; the
  id->order index becomes `unordered_map<OrderId, Order*>` (consider an
  intrusive hash hook or open-addressing map to drop those node allocs too).
  Cancel stays O(1) by unlinking through the Order's own hooks; pool slot
  addresses are stable, so pointers don't invalidate. `submit()` allocates a
  node from the pool, fills it, and links it in; steady-state allocation
  approaches zero (freed slots reused). This changes `Order` from a copied-by-
  value POD into a pool-managed node that matching mutates in place. Composes
  with the out-of-line `Conditional` owning pointer above. `std::pmr` over an
  `unsynchronized_pool_resource` is an acceptable stepping stone, but the
  target is the intrusive pool. Measure before committing.
- **Parameterize numeric widths -- template Order (decided).** Make `Order`
  (and `OrderBook`/matcher/`Trade`) a template over the numeric types, e.g.
  `template<class Qty = std::uint64_t, class Px = std::int64_t,
  class Notional = ...>` with defaults, so a market can select 64/128/256-bit
  precision (`__int128`, or `intx::uint256` for full on-chain fidelity). Open
  items to resolve when implementing:
  - Signedness: `Quantity`/`Ticks` are currently `int64_t` (signed); the
    requested default is `uint64_t` (unsigned). The matcher keeps quantities
    non-negative, so unsigned works, but audit every subtraction
    (`leaves -= fill`) and any reduce-only/position math before switching.
  - Notional is a *separate, wider* parameter: `price * qty` needs more bits
    than either factor (see the overflow analysis), so default it to at least
    the sum of the Px+Qty bit-widths, or 128-bit.
  - Heterogeneous widths in one process: a template is only *required* if the
    same binary must run instruments of different widths (e.g. BTC `int64`
    beside ETH 256-bit). If one deployment shares a single width, a build-time
    typedef in `types.h` is simpler and keeps the pimpl compile firewall.
    Decide which is needed -- it drives template-vs-typedef.
  - Cost: templating the core moves `OrderBook`/matcher into headers, giving up
    the current pimpl firewall and lengthening compiles; weigh against the
    flexibility.
  - Define a numeric concept (arithmetic + `<`/`==`/`min`) so custom types like
    `intx::uint256` (which provide these) plug in and mis-instantiations fail
    cleanly. Centralize `to_string`/parse -- the std lib formats neither 128-
    nor 256-bit -- for storage-helper fields and JSON output.

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
- [x] P1 Config subsystem: OptionRegistry (single shared declaration),
      `key = value` file parser, CLI parser (`--k=v`, `--k v`, bare bool,
      `--config`), precedence defaults->file->CLI, type/range validation,
      unknown-key rejection. Typed getters. Catch2 suite green.
- [x] P2 Event loop (kqueue + epoll): `EventLoop` abstraction with a
      base-class timer min-heap and level-triggered `IoHandler`/`TimerHandler`
      dispatch; native `KqueueLoop`; compile-guarded `EpollLoop` (Linux, not
      yet compiled/validated here); `MakeEventLoop` factory; `FakeEventLoop`
      test fixture. Deterministic timer tests (ManualClock) + real kqueue
      pipe/socketpair I/O tests. Green.
- [x] P3 Net HTTP: hand-rolled incremental HTTP/1.1 parser (partial input,
      Content-Length + chunked, pipelining, keep-alive resolution, size
      limits), request/response types, exact-match router (404/405),
      non-blocking `TcpListener`, and an event-loop-driven `HttpServer` with a
      per-connection read/parse/route/write state machine and dispatch-safe
      teardown (EventLoop gained `defer()`). Parser unit tests + real loopback
      integration tests (GET/POST/404/405/keep-alive). Green.
- [x] P4 Net WebSocket: RFC 6455 frame codec (masking, extended lengths,
      control-frame validation), hand-rolled SHA-1 + Base64 in util for the
      Sec-WebSocket-Accept handshake, and a `WsServer`/`WsConnection` that
      upgrades over the HTTP parser, reassembles fragmented messages, and
      auto-handles ping/pong/close. Vector tests (accept key, RFC masked
      frame, SHA-1/Base64) + real loopback echo integration. Green.
- [x] P5 IPC helper framework: wire-agnostic `HelperMessage` with two
      interchangeable codecs (text `key=value` + length-prefixed binary);
      `HelperClient` (pipelined, out-of-order correlation) over injected fds
      plus a `SpawnHelper` fork/exec spawner; `StorageClient` implementing the
      storage schema (report_order/report_trade, processed-queue outbox,
      commit watermark that drops committed entries, pull_levels); and a
      reference `codicis_storage_helper` child binary. Codec/round-trip +
      socketpair client + storage outbox tests + real spawned-helper
      integration. Green.
- [x] P6 App wiring and core seam: `codicis` executable. Config-driven
      startup (BuildOptionRegistry), spawns the storage helper, and serves a
      REST API on the event loop wiring net -> core -> ipc: `GET /health`,
      `POST /orders` (form body -> matching engine), `POST /orders/cancel`,
      `GET /book`. Matched orders/trades are reported to the storage helper and
      committed on a periodic timer. End-to-end app test (spawned helper) plus
      a verified curl smoke test. NB: strict report-before-place ordering and
      async HTTP responses are deferred to OT8.
- [~] OT0-OT8 Order types (matching engine) -- in progress:
      - [x] OT0 domain types (integer-tick prices, SeqNo time priority,
            orthogonal type/TIF/flag axes + optional trigger/peg/link payloads)
            and the ingress normalizer (GTX->PostOnly, FOK->IOC+AON).
      - [x] OT1 continuous book: dense-ladder BookSide (deque indexed by tick,
            O(1) lookup), FIFO levels, O(1) cancel, price-time matcher for
            Market/Limit (trade at maker price), with IOC discard, FOK/AON
            all-or-none pre-scan, and Post-Only reject-if-cross.
      - [x] OT2 constraints: self-trade prevention (kNone/CancelResting/
            CancelAggressor/CancelBoth via client_id), Min-Quantity floor, and
            GTD/DAY expiry (expiry_ns + OrderBook::expire(now) returning
            cancelled ids for the app to sweep on a timer).
      - [x] OT3 stops & trailing: stop-market, stop-limit, and trailing-stop
            orders parked in a pending structure keyed off the last trade
            price; a trigger evaluator fires them (inject as market/limit),
            cascading until quiescent (firing by arrival seq); trailing stops
            re-anchor on favorable moves. Cancel works while parked;
            `last_trade_price`/`pending_stop_count` accessors. (Trigger scan is
            O(pending); price-keyed stop maps are the later optimization.)
      - [x] OT4 iceberg + hidden: iceberg orders expose only a display slice
            and match only that much per fill, then replenish from the reserve
            and re-queue at the back of the level (time-priority loss); hidden
            orders match on their full reserve but contribute 0 to the public
            book. `displayed_qty_at` vs `total_qty_at` split the visible from
            the matchable view. (Reduce-Only needs a position/risk layer and
            Discretionary needs a hidden price band in matching -- both still
            TODO within OT4.)
      - [x] OT5 pegged: Primary (same-side best), Market (opposite-side best),
            and Midpoint pegs with a signed offset and protective cap. Pegs
            rest in the ladder (matchable) but their price derives from the
            reference BBO computed over NON-pegged orders only (so they never
            chase themselves); a single reprice pass runs after any book event.
            Repricing moves the order (time-priority loss). NB: a peg that
            reprices to a marketable price rests there and matches on the next
            aggressor rather than auto-matching -- a documented simplification.
      - [x] OT6 contingent orders: a group manager keyed on LinkSpec.group_id
            reacts to fills. OCO cancels the sibling leg when one fills; OTO
            holds a child out of the book and releases it when the parent
            fully fills; a bracket is OTO + OCO -- the entry's fill releases
            take-profit and stop-loss as a mutually-cancelling OCO pair.
            Child release re-enters submit (trades cascade into the outcome).
      - [~] OT8 storage determinism (engine + app wiring done; windowing TODO):
            `TradingEngine` (src/engine) enforces report-before-place -- each
            order is reported to the storage helper FIRST and placed only on
            ack -- with a staging buffer that keeps placement in arrival-seq
            order even when acks return out of order. On placement it reports
            every trade (both order ids) and each affected order's fill
            (partial/complete, taker + makers) via new StorageClient
            report_fill. A failed pre-report does not place the order.
            AppServer is wired through the engine: the net layer gained async
            (deferred) HTTP responses -- the router supports async handlers, and
            responses route back through the server keyed by (fd, connection
            id) so they stay safe across connection teardown/fd reuse; only one
            request is outstanding per connection at a time (preserves HTTP/1.1
            ordering). POST /orders is now async report-before-place;
            GET /health, /book and cancel stay synchronous. Verified by tests
            and a live curl run.
            TODO: pull-levels-on-demand, which first needs top-of-book
            windowing (level eviction) -- currently the book holds all levels
            in memory, so there are no non-resident levels to pull.
      - [ ] OT4 remainder (reduce-only, discretionary), OT7 auctions.
- [ ] H1-H2 Hardening (TLS + robustness)
