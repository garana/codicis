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
- TLS is NOT handled in-process: an edge reverse proxy terminates HTTPS/WSS and
  forwards plaintext to codicis on localhost. The old in-process
  `SecureTransport` seam (H1) is dropped. See the market-data/scaling decision
  in "To Design".
- Order book is a dense dynamic-array "ladder" (ring buffer indexed by tick
  offset) for O(1) price->level lookup, with intrusive FIFO lists per level.
- IPC is a generic helper framework so more helper types (trade reporter,
  last-known-price reporter, ...) can be added later. Pipelining is required.

This file (Progress + To Design) is the durable project record. The plan file
`~/.claude/plans/hashed-seeking-manatee.md` holds only the current active plan
(it no longer carries the whole-project master plan).

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
  queue; an entry is removed only when the storage helper reports success.
  Partial handling exists today: report-before-place means a new order is not
  placed unless its pre-report is acked (HelperClient now has per-request
  timeouts and fails -- never drops -- requests on a dead/closed helper), and
  post-placement trade/fill report failures are surfaced by the engine
  (`report_failures()` counter + error log). Bounded outbox + client
  backpressure is now DONE: `storage.processed_queue_max` caps the un-committed
  processed queue; when it is full AppServer sheds new orders with HTTP 503 (+
  Retry-After) on both the REST and WS submit paths. Still deferred: the actual
  *rollback* -- undoing the in-memory book mutation (and dependent fills) when
  a trade/fill fails to persist. Detailed design (compensation vs.
  optimistic-with-undo-log, cascade of dependent fills, client notification)
  intentionally deferred.
- **Additional helper types.** Trade reporter, last-known-price reporter, etc.,
  added later on the same `Helper`/`HelperCodec` framework.
- **Client positions from the storage helper (reduce-only durability) -- DONE,
  except the reader-connection split.** Implemented: `report_order` carries
  `owner`; the helper maintains per-(owner,symbol) net from the reported fill
  stream and answers `pull_position(user_uuid, symbol) -> net`; the engine pulls
  on a client's first order for a symbol, seeds the book (keyed by the
  client_id `client_for` assigns), and parks placement until the pull returns
  (`pos_ready` gate; per-account coalescing). REMAINING: the pull currently
  shares the writer's storage connection -- move it onto the separate reader
  `HelperClient` (see the item below) so a slow write ack can't delay a
  position read, and so it composes with pull-levels-on-demand's park/resume.
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
- **Market-data & scaling architecture (decided).** Four decisions on how the
  feed and edge scale, keeping the matching process busy only with matching:
  1. **TLS offloaded at the edge (replaces H1).** codicis does NOT implement
     TLS in-process; an edge reverse proxy (HAProxy, or nginx/Envoy) terminates
     TLS/WSS and forwards plaintext http/ws to codicis on localhost. The
     `SecureTransport` seam is dropped. HAProxy notes: WebSocket works in HTTP
     mode with a long `timeout tunnel`; use PROXY protocol (`send-proxy`) if
     codicis needs the real client IP. Deployment component, not a build dep.
  2. **One feed-helper builds L1, L2 and L3** from a single input: the order
     lifecycle event stream (add/update/remove) plus the trade stream. It does
     NOT re-run matching -- L3/MBO is essentially relaying the events; L1/L2
     aggregate the same deltas into best-bid/ask and per-price sums (holding a
     replica is unavoidable for depth, but it is a mechanical apply, not
     inference). Prerequisite: codicis must emit EVERY book mutation (adds,
     cancels, peg reprices, iceberg replenish, stop triggers, GTD/DAY expiry) as
     a sequence-numbered event log, not just trades. Public trade prints stay
     anonymous (price/qty/time/aggressor side, no order ids); a separate
     drop-copy/private feed to an order's owner may carry its UUID. The feed
     path is best-effort + non-blocking (the engine must never stall on a slow
     feed consumer): consumers detect a seq gap and resync from a snapshot --
     unlike the reliable, acked storage path. Transport: start with a pipe,
     evolve to an SPMC shared-memory ring or UDP multicast.
  3. **External UUIDs with a uuid<->id map.** Assign a random uuidv4 per order
     at creation (v4, not v7 -- no ordering/rate leak). External events, feeds,
     and the client-facing order handle use the UUID; internal matching stays on
     the fast integer OrderId. Keep a uuid<->OrderId map. Open item: pruning the
     map on order removal cleanly needs the book-event stream (fill/cancel
     events) -- until then prune on cancel and accept growth on fills.
  4. **Per-symbol book registry; Symbol stays OFF the Order class.** A matching
     engine owns `unordered_map<Symbol, OrderBook>`; the inbound request (or a
     symbol-scoped session) supplies the symbol and routes to the right book.
     Do NOT use an ambient/AsyncContext for the symbol -- it is intrinsic order
     routing data, the event loop doesn't know it until the payload is parsed,
     and one connection can multiplex symbols; pass it explicitly. Keep it off
     the hot `Order` struct (the book already knows its own symbol; cancel is
     scoped by symbol), which also serves the lean-Order goal. Build order:
     per-symbol registry -> UUIDs -> book-event stream -> feed-helper (move WS
     serving out of the matcher).
- **Sparse-book fallback.** The dense ladder's memory is proportional to the
  in-window tick span; add a hash-index fallback for pathologically sparse
  instruments.
- **AON pre-scan over-counts AON makers (residual).** Maker-side AON is now
  implemented (the matcher skips a resting AON maker the aggressor can't fully
  take, filling past it), but the FOK/AON/Min-Qty pre-scan (`available_fill`)
  still counts an AON maker's *full* size as available even when the aggressor
  cannot take it whole. So a FOK/AON/Min-Qty aggressor facing a *larger* AON
  maker can pass the pre-scan and then fill 0 (FOK then discards; Min-Qty may
  under-reject). Fix: make `available_fill` AON-aware (count an AON maker only
  if the running remainder can take it in full). Deep edge; no partial fills
  result, so low priority.
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
  INTERIM DONE: the book's node-allocating containers (each level's FIFO list,
  the id->Entry / stops / pegged / groups / order_group maps, and the per-side
  `deque<Level>`) now draw from one `std::pmr::unsynchronized_pool_resource`
  owned by `OrderBook::Impl` (`Level` was made allocator-aware so the pooled
  resource propagates into each level's list). This recycles node allocations
  without changing behavior; it does NOT yet remove the separate list/map nodes
  or the allocator's virtual dispatch -- the intrusive Order pool remains the
  target. Group's inner `std::vector`s are still globally allocated (rare).
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
  - **DB mirrors the C++ width per market (operator decision).** When the
    template lands, the storage schema must faithfully mirror each market's
    precision -- NOT a "widest uniform type" shortcut. Each width class gets its
    OWN full set of tables/collections (orders, resting, positions, fills,
    trades) typed to that width (`BIGINT` for 64-bit; `NUMERIC`/`DECIMAL`/
    `Decimal128` for wider), routed by a symbol->width registry. Partitions
    can't vary column type, so genuinely separate tables. (Task: multi-precision
    DB.) Note the latent `resting` gap: `pull_levels` sorts by `(price, seq)`
    but `seq` is arrival order, never re-stamped on reprice/replenish -- see the
    priority-rank vs arrival-seq task.
  - **ETH / uint256 -- offer BOTH, per market (operator decision).** (1) Scaled
    fixed-point where WE scale server-side (int64 price/qty on a per-instrument
    tick/lot far coarser than 18 decimals, wider int128 notional; client sends
    human units). (2) Full `uint256` (wei) for on-chain/on-chain-settled
    fidelity. Hybrid: int64 matching + uint256 at the settlement boundary. The
    uint256 markets take the wide table set, scaled-int64 markets the `BIGINT`
    set. (Task: ETH numeric options.)
  - Mongo storage helper now ships `$jsonSchema` validators (`schema/mongo.js`,
    applied via `storage-mongo -migrate`) enforcing field presence + BSON types
    (`int`/`long` for the 64-bit engine; a wide market's collections would
    accept `decimal`). Recovers the type safety a schemaless store lacks.

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
      Both transports now serve orders: a WebSocket endpoint on `net.ws_port`
      (default 8081) accepts the same form-encoded order body as a text frame
      and streams back the JSON result, submitting through the same
      TradingEngine. Order parsing/rendering is shared between HTTP and WS; the
      async reply routes back through WsServer keyed by (fd, connection id) so
      it is dropped safely if the client disconnects. Covered by an end-to-end
      WS integration test and a live Python-client smoke test.
      WebSocket clients can also subscribe to a market-data stream
      (`action=subscribe`/`unsubscribe`): AppServer tracks subscribers (dropped
      on disconnect via a new WsServer on-close callback) and, after any
      accepted order or successful cancel, broadcasts a top-of-book + trades
      update to them. Covered by a two-connection integration test (subscriber
      receives the resting-ask snapshot and the trade print) and a live smoke
      test.
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
            cancelled ids for the app to sweep on a timer). Maker-side AON:
            the matcher walks a level (and levels) skipping a resting AON order
            the aggressor can't fully take (filling past it -- a deliberate
            FIFO/price priority inversion); a GTC AON that can't fill on entry
            rests as an AON maker rather than being rejected (only IOC/FOK/
            market AON reject).
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
            the matchable view.
      - [x] OT4 discretionary: a discretionary order (kDiscretion +
            `Order::discretion` band) displays/rests at its limit price but,
            when it is the aggressor, reaches into the spread by the band -- a
            buy takes up to price+discretion, a sell down to price-discretion --
            trading at the maker price. The matcher widens the acceptance test
            via `effective_limit()` (used in crosses/available_fill/match); the
            resting remainder shows at the plain limit. Documented
            simplification: the band is exercised only on entry, not re-applied
            while resting (like pegs that do not auto-match). Core-only, like
            the other liquidity flags -- not yet exposed over REST.
      - [x] OT4 reduce-only: the book tracks a signed net position per client id
            (same key as STP), updated on every fill for both taker and maker.
            A reduce-only order may only shrink an existing position: it needs
            an account (client id != 0), a same-direction position (sell reduces
            a long, buy a short), and is capped at the reducible quantity -- the
            net position minus reduce-only leaves already RESTING on that side
            (reserved on rest, released on fill/cancel/STP-cancel) so concurrent
            reduce-only orders can never together flip the position. Wrong-side
            or zero position rejects. The position map lives in the OrderBook
            (per symbol); a fuller cross-symbol/margin risk layer would sit
            above it. Core-only (not yet exposed over REST). The account is the
            user uuid, mapped by the engine to the internal client_id
            (client_for); anonymous (empty uuid -> client_id 0) cannot be
            reduce-only. Positions are now DURABLE: seeded from the storage
            helper (see the position-persistence progress item below), not just
            session-accumulated.
      - [x] Position persistence via the storage helper (durable reduce-only):
            the storage helper is the system of record. `report_order` now
            carries `owner`; the reference helper remembers each order's
            owner+side and, on each `report_fill`, updates a per-(owner,symbol)
            net; a new `pull_position(user, symbol) -> net` returns it. The
            TradingEngine pulls a client's position on its FIRST order for a
            symbol (via `ensure_position`) -- when the client has no book
            presence, so no fill can race the seed -- seeds the book
            (`MatchingEngine::seed_position`), and PARKS placement until the
            pull returns (a new `pos_ready` gate on the report-before-place
            staging; concurrent orders for the same account coalesce onto the
            one in-flight pull). On pull failure the book stays flat
            (reduce-only then conservatively finds nothing to reduce). Tested at
            the helper level (report_order/fill -> pull_position round-trip over
            the spawned reference helper) and the engine level (a pulled long
            caps an
            oversized reduce-only). NB: the pull shares the writer's storage
            connection for now -- moving it to a separate reader HelperClient is
            still the To Design optimization below.
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
            OT8 pull-levels-on-demand windowing is in progress (3 phases):
            - [x] Phase 1: the storage helper holds the full continuous resting
                  book, reported when an order rests (report_rest), decremented
                  by report_fill, removed by report_cancel; pull_levels
                  returns a multi-record order list (best-price first,
                  arrival/seq order within a level) for the deep slice beyond
                  the caller's resident boundary. Eviction/pull-back write
                  nothing -- the order is already stored.
            - [x] Phase 2: the OrderBook is a bounded resident cache
                  (`OrderBook(stp, mem_levels)`, MatchingEngine passes it). An
                  order that would open a level worse than the window's far edge
                  when full rests DEEP (SubmitOutcome.rested_deep, not
                  materialized); a better order evicts the worst resident level
                  (SubmitOutcome.evicted). has_deep / deep_boundary /
                  worst_resident accessors + insert_resident (pull-back
                  primitive). mem_levels defaults to 0 (unbounded) and is not
                  yet wired into AppServer -- production stays unbounded until
                  Phase 3 adds the engine cooperation.
            - [x] Phase 3: async pull-back in the engine. On rest (resident or
                  deep) the engine report_rest's the order + registers a handle
                  (deep orders are cancellable); cancel does report_cancel +
                  matching_.cancel (no-op if deep). In drain, ensure_depth
                  gates
                  the head order: on a symbol's first order it warms the top N
                  per side (pull_levels buy from INT64_MAX, sell from
                  INT64_MIN);
                  and it pulls the deep contra levels a crossing aggressor would
                  reach (needs_deep: has_deep(contra) && the limit crosses
                  past
                  the worst resident contra), insert_resident's them, and
                  resumes draining -- chaining pulls until covered. mem_levels
                  is wired from config.book.mem_levels into MatchingEngine +
                  TradingEngine.
                  Race: a cancel while a pull is in flight adds the id to
                  pull_ignore_ so on_pull does not resurrect it (cancel wins).
                  Verified: test_app pulls a deep level an aggressor reaches and
                  fills it; live smoke pulls two deep levels. LIMITATIONS
                  (noted,
                  follow-ons): pulls share the writer storage connection (the
                  separate reader HelperClient is still deferred); PulledOrder
                  carries no client_id, so a pulled-back order loses
                  STP/position
                  attribution in memory (storage positions stay correct via
                  report_fill); handles are in-memory, so after a restart pulled
                  orders match but are not cancellable by their old uuid.
      - [x] OT7 auctions (MOO/LOO/MOC/LOC/OPG): auction-flagged orders are
            queued into a per-book opening or closing auction rather than
            trading continuously. `run_opening_auction`/`run_closing_auction`
            compute one uniform clearing price (max executed volume; ties
            broken by smallest imbalance, then nearest the last trade price,
            then lowest price; all-market falls back to the last price) and
            cross all eligible orders at that single price with market-first,
            then price, then arrival-seq priority. Unfilled LOO/LOC limit
            remainders enter continuous trading (match + rest); MOO/MOC/OPG
            remainders are discarded. Auction trades cascade stops and reprice
            pegs. Queued orders can be cancelled before the cross. Wired into
            the app: order submission takes an `auction=moo|loo|moc|loc|opg`
            field, and `POST /auction` (form `symbol` + `phase=open|close`) runs
            the cross via `TradingEngine::run_auction` (reports prints + fills
            to storage, broadcasts market data), returning the executions. NB
            the trigger is operator-driven (no automatic session clock yet); the
            endpoint should be access-controlled at the edge. Simplification:
            the cross is over auction-designated orders only, not merged with
            the resting continuous book. All six order-type CATEGORIES are now
            implemented; OT8 pull-levels-on-demand windowing is the remaining
            matching-engine work.
- [~] Multi-symbol + market-data architecture (decided; see To Design):
      - [x] Per-symbol book registry: `MatchingEngine` (src/core) owns one
            OrderBook per Symbol, created on first use, routing submit/cancel/
            find/best/expire by symbol. Symbol stays off the Order struct.
            Tested for per-symbol isolation (no cross-symbol matching), scoped
            cancel (same id in two symbols), and empty-not-error unknown-symbol
            queries. NOT yet wired into TradingEngine/AppServer (next step:
            symbol in the request + per-symbol routing/MD).
      - [x] Wire MatchingEngine into TradingEngine + AppServer. TradingEngine
            now takes a MatchingEngine and `submit(symbol, order, cb)`, routing
            placement to the symbol's book and tagging every storage report
            (order/trade/fill) with the symbol. AppServer holds a
            MatchingEngine; every order/cancel requires a `symbol` field,
            `GET /book?symbol=` is per-symbol, and market-data subscriptions are
            per-symbol (`action=subscribe&symbol=`) with per-symbol broadcast.
            Tests updated; verified live with independent BTC/ETH books.
      - [x] External UUIDs (uuid<->id map): every order gets an opaque uuidv4
            handle (returned to the client), mapped in the TradingEngine to its
            internal (symbol, id); handles are pruned on cancel and on maker
            fill. Each order has an owner (a user uuid); cancel is authorized
            only for the owner (kOk/kNotFound/kForbidden). NB: an earlier commit
            (a4a2d95) briefly took the owner from a client `user` form field;
            that field was REMOVED -- identity now comes only from auth (below).
      - [~] A sequence-numbered book-event stream, then the feed-helper building
            L1/L2/L3. Build in that order.
            - [x] Slice 1 (core emission): `core/book_event.h` defines BookEvent
                  (seq, symbol, type, order/taker id, side, price, qty) +
                  BookEventSink. OrderBook emits Add/Cancel/Trade at the
                  mutation chokepoints -- rest() (resident AND deep), the
                  in-match STP cancel, cancel() (which also covers GTD/DAY
                  expiry and OCO sibling cancels routed through it), and the
                  continuous + auction trade sites. A full fill does NOT emit
                  Cancel (the Trade already conveys the maker's departure);
                  eviction/pull-back emit nothing (the order stays in the book).
                  MatchingEngine is each book's sink: the book stamps the
                  symbol, the engine stamps a single GLOBAL monotonic seq
                  (`set_book_event_sink`, `last_event_seq`) and forwards to the
                  downstream feed consumer. Tested in test_matching_engine
                  (Add/Trade/Add across two symbols with contiguous seqs;
                  cancel emits, fill does not). No transport yet.
            - [x] Slice 2 (in-place mutations): kReprice (a pegged order moves
                  old->new price in move_pegged; BookEvent.prev_price holds the
                  old), kReplenish (an iceberg refreshes its displayed slice in
                  the match loop; qty = new slice), kTrigger (a parked stop
                  fires in evaluate_stops -- a lifecycle marker; the resulting
                  depth/executions still arrive as the inject's own Add/Trade).
                  Tested in test_order_book (a repricing midpoint peg, a
                  replenishing iceberg, a firing stop each emit their event).
            - [x] Slice 3 (feed-helper + publisher, over a pipe): `feed/`
                  library -- feed_wire (compact binary BookEvent codec),
                  BookReplica (I/O-free L1/L2/L3 per symbol from the event
                  stream, with seq-gap detection), and FeedPublisher (codicis-
                  side BookEventSink: encodes events into a bounded buffer
                  drained to the helper stdin on the loop; best-effort --
                  drop-on-overflow, the seq gap signals loss, the matcher never
                  blocks). Reference `codicis_feed_helper` (tools/) runs on the
                  generic EventLoop (NO backend-specific code): reads events on
                  stdin, applies to a BookReplica, and fans out to many TCP
                  subscribers (JSON: snapshot on connect, L1 updates streamed,
                  l2/l3 queries). Drop policy: per-subscriber 4 MiB outbound cap
                  -> disconnect the slow subscriber (never backpressure the hot
                  path). AppServer spawns it (feed.helper_cmd) and points
                  matching_.set_book_event_sink at the publisher;
                  codicis_feed_events_dropped_total on /metrics. Tests:
                  test_feed (wire round-trip, replica L1/L2/L3 + gap, publisher
                  pipe write), test_feed_helper (spawn the binary, stream
                  events, a TCP subscriber sees the L1 update + an l2 query).
                  Fan-out validated under the epoll backend on Linux (CI).
            - [x] Slice 4 (displayed-vs-total depth): every book event now
                  carries a `displayed` (lit) quantity beside the matchable
                  `qty` -- 0 for hidden, the slice for an iceberg, else == qty;
                  a Trade carries the lit amount its fill consumed; Replenish
                  re-lights the refreshed slice. Populated at each emit site
                  from the book's DisplayedOf/slice and carried on the wire.
                  BookReplica keeps BOTH aggregates per side: matchable
                  (depth/best_bid/best_ask) and lit (displayed_depth/
                  best_displayed_bid/best_displayed_ask); a level with only
                  hidden size is absent from the lit book. Non-breaking: the
                  matchable accessors are unchanged, the lit view is additive.
                  The feed-helper's L1 now carries lit_bid/lit_ask beside
                  bid/ask and answers an `l2d` (lit depth) command; L3
                  market-by-order stays the full/matchable view. Tested:
                  test_order_book (Add emits displayed 5/0/3 for normal/hidden/
                  iceberg) and test_feed (replica separates a hidden better bid
                  from the lit book; wire round-trips displayed).
            - [ ] Slice 5+: transport beyond the pipe (SPMC shared-memory ring
                  or UDP multicast); snapshot-based replica RESYNC after a gap
                  (needs a snapshot source). Feed fan-out to Kafka/SNS/SQS is a
                  separate task (a language-agnostic bridge off the helper's TCP
                  stream -- see the task list).
- [~] Authentication + authorization layer:
      - [x] Owner authorization enforced in the engine (owner uuid vs cancel
            requester); order handles are external uuids (see above).
      - [x] Configurable request authentication (`auth.*`), two non-exclusive
            mechanisms: Option A trusts a user-uuid header from an
            authenticating edge; Option B forwards a credential header to a pool
            of auth helper child processes (concurrency + pipelining depth) that
            resolve it to a uuid. Positive/negative TokenCache (approximate
            weighted-LRU on an intrusive list; positive entries expire at the
            helper's `not_after`), single-flight coalescing per credential, and
            a WallClock for absolute expiry. Both enabled => must pass and agree
            (403 on mismatch); 401 on missing/invalid, anonymous when disabled.
            WS identity is resolved once at the handshake. Reference
            `codicis_auth_helper`. Covered by test_token_cache, test_auth, and
            auth cases in test_app; verified live.
      - [x] Aggregator WebSocket (per-frame identity): an optional second WS
            listener (`net.aggregator_ws_enabled` + `net.aggregator_ws_port`,
            one WsServer, second TcpListener, connections tagged
            `is_aggregator`). A connection accepted there whose handshake
            carries NO identity supplies a `user` uuid on EACH frame (validated
            per frame), letting one connection multiplex many end-users -- an
            ingress path for a network-side request aggregator. Per-frame is
            granted ONLY when unauthenticated AND on the aggregator port (else
            the handshake identity applies as usual). Trust is the network-
            restricted port; bind it private (`net.aggregator_ws_bind_address`,
            empty reuses `net.bind_address`). test_app: two users trade over one
            aggregator connection, a frame with no `user` is rejected. NB: this
            is a WebSocket path; the pipe-based [ingress helper] below is the
            other per-user ingress mechanism.
      - [ ] JWT/PASETO verification at the edge, mTLS, or HMAC request signing
            as alternative/stronger authentication front-ends (see To Design).
- [x] H2 Hardening (robustness):
      - [x] Linux/epoll compile + validation: built clean under -Werror in an
            ubuntu:24.04 clean-room; 7 real-EpollLoop integration tests
            (test_net_epoll) exercise partial reads, EAGAIN/EPOLLOUT
            backpressure, half-close, accept-drain, backlog; also fixed a real
            fd-leak bug --
            listen/accepted sockets are now FD_CLOEXEC (were leaking into every
            fork/exec'd helper).
      - [x] Bounded outbox + backpressure: `storage.processed_queue_max` caps
            the un-committed processed queue; AppServer sheds new orders (REST +
            WS) with HTTP 503 + Retry-After when it is full.
      - [x] HTTP parser hardening: request-smuggling defenses (conflicting
            duplicate Content-Length, CL+TE together, CL overflow) and RFC 7230
            character validation (non-token method/header-name, control chars in
            target/value, obs-fold rejection). Adversarial test_http_parser
            cases.
      - [x] WebSocket frame + reassembly hardening: reject set RSV bits and
            unmasked client frames (require_masked), a data frame arriving
            mid-reassembly, and a fragmented message past a 64 MiB cap; explicit
            16 MiB per-frame cap. Adversarial test_ws cases.
      - [x] Helper-codec hardening: text-record 64 MiB cap (no-terminator
            memory-exhaustion guard) + req_id overflow/length rejection; binary
            codec overrun guards locked in by adversarial test_ipc cases.
      - [x] Load/soak tests (on Linux/epoll): in-process event-loop
            load/soak (test_net_soak -- 100 pipelined connections x 50 requests,
            1000 accept/close waves with zero-leak bookkeeping) and the feed
            fan-out soak (test_feed_soak -- 50 subscribers, sustained stream,
            slow-consumer 4 MiB drop, abrupt-close survival). Green in the
            ubuntu:24.04 clean-room. (H1 in-process TLS is dropped entirely --
            TLS is offloaded to an edge proxy; see To Design.)
- [x] Observability: a Prometheus text metrics endpoint (`metrics.enabled`,
      `metrics.path` default `/metrics`). AppServer keeps single-threaded
      process counters (orders received/accepted/rejected/backpressure, cancels
      + failures, auctions, trades, WS messages) and renders them alongside live
      gauges read from the subsystems (storage outbox depth + report failures,
      pending placements, live symbols, MD subscribers). No identity required --
      restrict at the edge. test_app asserts the counters after a resting sell,
      a crossing buy, and a parse reject.
- [x] Ingress helper (`ingress.helper_cmd`, empty = off): the optional client-
      request source. Unlike storage/auth helpers (codicis initiates), an
      ingress helper INITIATES: it pulls order/cancel traffic from its own
      external system (Kafka/RabbitMQ/SQS/...) and writes requests to its stdout
      (codicis reads); codicis writes replies to its stdin. New `IngressHelper`
      (src/ipc) is the server side of the same `HelperCodec` + a
      `SpawnIngressHelper` (mirrors SpawnHelper's pipe/fork/exec, CLOEXEC+
      nonblock). AppServer spawns it in start() and routes each request through
      the SAME engine as REST/WS via `handle_ingress`. Each request carries a
      per-message `user` (the helper aggregates many end-users; trust = private-
      pipe managed child) and the order body travels as a single `form` field so
      an order field named `type` cannot collide with the codec's reserved
      envelope `type`. Reference `codicis_ingress_helper` (tools/) relays
      request lines from a file (its stand-in external source). test_app spawns
      it and confirms a relayed order rests on the book. Reuses the metrics
      counters.
- [~] Data helpers in Go (`helpers/`, separate Go module, deps vendored + a
      repo-local `.gocache` so nothing installs globally): reference storage +
      feed helpers. Chosen because the mature DB/broker clients live outside
      C++, and the helper-process boundary keeps those 3P deps out of the
      matcher's address space (each binary links only its own driver). Shared
      code (`internal/wire` text codec, `internal/storage` Store interface +
      protocol dispatch with ordered writes / concurrent Pull* reads) is
      written once; each backend is a thin adapter.
      - [x] Storage: `storage-postgres` (pgx), `storage-mysql`
            (go-sql-driver), `storage-mongo` (mongo-driver). Full protocol
            (report_order/rest/fill/cancel/trade, commit watermark,
            pull_position, pull_levels blob), schema DDL (`schema/`, applied via
            `-migrate`), position attribution on fill, reader/writer split.
            Unit tests (wire round-trip, protocol dispatch via a fake Store) run
            with no services; `-tags integration` tests drive each Store through
            a full lifecycle against real Postgres/MySQL/Mongo in
            docker-compose (`docker/`) -- all three green.
      - [x] Feed fan-out bridges: subscribe to the feed-helper's TCP JSON
            stream and republish per-symbol to a broker. Shared `internal/feed`
            does the connect/read/reconnect loop once (best-effort: reconnect +
            re-snapshot on drop); each bridge supplies a Sink. ALL eight are
            pure-Go (no CGO -- verified building with CGO_ENABLED=0), each
            linking only its own client: feed-redis (go-redis), feed-nats
            (nats.go), feed-kafka (segmentio/kafka-go), feed-rabbitmq
            (amqp091-go), feed-mqtt (paho, EPL-2.0/EDL-1.0), feed-zeromq
            (go-zeromq/zmq4 -- pure Go, no libzmq), feed-sns + feed-sqs
            (aws-sdk-go-v2, LocalStack-compatible via -endpoint). Each has an
            integration test (fake feed source -> bridge -> real broker ->
            consumer); all green against redis/nats/kafka/rabbitmq/mosquitto and
            LocalStack in docker-compose (ZeroMQ needs no broker). Dep licenses
            re-audited: all MIT/Apache/BSD except mysql (MPL) + paho (EPL),
            both weak file-level and used unmodified -- no GPL/LGPL.
      - [x] Integration harness: `helpers/docker/run-integration.sh` (and
            `make integration`) brings up every backing service via
            docker-compose, waits for readiness, runs all 11 helper integration
            tests against them (offline, from vendor/), tears down, and exits
            non-zero on any failure -- the one command a clean-room/CI invokes.
            Verified locally end-to-end (~30s), all green.
      NB the Go helpers are OUTSIDE the C++ CMake build; the C++ reference
      helpers (tools/) remain the in-tree contract tests.
