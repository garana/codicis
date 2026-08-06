# codicis data helpers (Go)

Reference **storage** and **feed** helpers for codicis, written in Go. codicis
delegates persistence and market-data fan-out to child-process helpers over
pipes; a helper is any program that speaks the documented wire protocol, so
these are **reference implementations** -- an operator may replace any of them
with their own (in any language) as long as the protocol matches.

Go is used here because the mature, well-supported client libraries for these
databases and brokers live in the Go/Java/etc. ecosystems, and the
helper-process boundary keeps those third-party dependencies **out of the
matching engine's address space**: each helper links only its own driver, so a
bug or CVE in a driver can at worst crash a restartable child, never the matcher.

## Layout

```
internal/wire       the codicis helper text codec ("key=value" line protocol)
internal/storage    shared storage-helper runtime (Store interface + dispatch)
internal/feed       shared feed decode + subscription (added with the feed helpers)
schema/             reference DDL (postgres.sql, mysql.sql) + embed
cmd/storage-*       one storage-helper binary per database
cmd/feed-*          one feed-fanout binary per broker (added incrementally)
docker/             docker-compose backing services for integration tests
```

The shared runtime handles the protocol once; each backend supplies only a thin
`Store` (or feed `Sink`) adapter. Because Go statically links only imported
packages, each binary depends on the fewest libraries -- e.g. `storage-postgres`
pulls in `pgx` and nothing MySQL/Mongo-related.

## Build

Dependencies are **vendored** under `vendor/`, and when re-resolving they are
cached in a repo-local `.gocache` (never the global module cache), so builds are
reproducible and self-contained:

```
cd helpers
make build          # -> bin/storage-postgres, bin/storage-mysql, bin/storage-mongo
make test           # unit tests (wire codec + protocol dispatch; no services)
make vendor         # re-resolve + re-vendor after changing deps
```

## Storage helpers

| Backend    | Binary             | Driver                          | Config                    |
| ---------- | ------------------ | ------------------------------- | ------------------------- |
| PostgreSQL | `storage-postgres` | jackc/pgx                       | `-dsn` / `$CODICIS_PG_DSN`   |
| MySQL      | `storage-mysql`    | go-sql-driver/mysql             | `-dsn` / `$CODICIS_MYSQL_DSN`|
| MongoDB    | `storage-mongo`    | go.mongodb.org/mongo-driver     | `-uri` / `$CODICIS_MONGO_URI`|

Point codicis at one via `storage.helper_cmd`, e.g.:

```
storage.helper_cmd = /path/to/storage-postgres -dsn postgres://.../codicis -migrate
```

`-migrate` applies the schema on startup (idempotent `CREATE TABLE IF NOT
EXISTS`). Writes (`report_*`, `commit`) are applied in strict arrival order;
reads (`pull_levels`, `pull_position`) run concurrently on the connection pool
(`-read-concurrency`, default 8) -- matching the reader/writer split codicis
expects.

### Wire protocol (so you can write your own)

Text codec: one record is `req_id=<n>`, `type=<t>`, then `key=value` lines, then
a blank line. `req_id`/`type` are reserved. Requests are pipelined and a helper
may answer out of order (the response echoes `req_id`).

| Request         | Fields                                   | Response                       |
| --------------- | ---------------------------------------- | ------------------------------ |
| `report_order`  | symbol, owner, id, side, price, qty      | status=ok                      |
| `report_rest`   | symbol, side, id, price, leaves, seq     | status=ok                      |
| `report_fill`   | symbol, id, qty, remaining, status       | status=ok                      |
| `report_cancel` | symbol, id                               | status=ok                      |
| `report_trade`  | symbol, taker, maker, price, qty         | status=ok                      |
| `commit`        | (none)                                   | committed=<highest write id>   |
| `pull_position` | user, symbol                             | net=<int>                      |
| `pull_levels`   | symbol, side, from_price, count          | symbol, side, orders=<blob>, count |
| `ping`          | (none)                                   | (type pong)                    |

`report_fill` decrements the resting order (removing it when `status=filled`)
and updates the owner's net position (`buy` +, `sell` -). `pull_levels` returns
the orders on `side` beyond `from_price` (worse than it) across at most `count`
price levels, best price first and arrival (`seq`) order within a level; the
`orders` blob is `id,price,leaves,seq` records joined by `;`.

## Integration tests (Docker)

```
docker compose -f docker/docker-compose.yml up -d      # postgres, mysql, mongo
cd helpers
CODICIS_PG_DSN='postgres://codicis:codicis@localhost:5432/codicis?sslmode=disable' \
  go test -tags integration ./cmd/storage-postgres/
CODICIS_MYSQL_DSN='codicis:codicis@tcp(localhost:3306)/codicis' \
  go test -tags integration ./cmd/storage-mysql/
CODICIS_MONGO_URI='mongodb://localhost:27017' \
  go test -tags integration ./cmd/storage-mongo/
```

Each test drives its `Store` through a full order lifecycle (rest -> pull ->
partial fill -> position -> complete fill -> cancel) against the real database.
Without the env var set, the test skips.

## Feed helpers

The market-data fan-out bridges subscribe to the codicis feed-helper's TCP
stream (newline JSON: an L1 snapshot on connect, then L1 updates and any
l2/l2d/l3 query replies) and republish each per-symbol message to a broker. The
shared `internal/feed` runtime does the connect / read / reconnect loop once
(the feed is best-effort: on a dropped connection the bridge reconnects and the
helper re-snapshots); each bridge binary supplies only a `Sink`.

| Broker | Binary       | Client            | Destination                    |
| ------ | ------------ | ----------------- | ------------------------------ |
| Redis  | `feed-redis` | redis/go-redis    | PUBLISH channel `<prefix><sym>`|
| NATS   | `feed-nats`  | nats-io/nats.go   | subject `<prefix><sym>`        |

More brokers (Kafka, AMQP/RabbitMQ, MQTT, ZeroMQ, AWS SNS/SQS) follow the same
`Sink` shape and are added alongside their `docker/` services.

Run a bridge against a running feed-helper:

```
feed-redis -feed 127.0.0.1:9100 -redis localhost:6379 -prefix md.
```

Integration tests (gated on the broker env var) stand up a fake feed source and
assert the bridge republishes to the real broker:

```
docker compose -f docker/docker-compose.yml up -d redis nats
CODICIS_REDIS_ADDR=localhost:6379 go test -tags integration ./cmd/feed-redis/
CODICIS_NATS_URL=nats://localhost:4222 go test -tags integration ./cmd/feed-nats/
```

## Third-party licenses

Every vendored dependency is permissively licensed (MIT, Apache-2.0, BSD-3, and
MPL-2.0 for the MySQL driver) -- none are GPL/LGPL, so vendoring and
redistribution are compatible with codicis's own license. Each dependency's
license text is retained under `vendor/`.
