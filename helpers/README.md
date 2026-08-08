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
integration/        black-box integration suite (testcontainers-go)
```

The shared runtime handles the protocol once; each backend supplies only a thin
`Store` (or feed `Sink`) adapter. Because Go statically links only imported
packages, each binary depends on the fewest libraries -- e.g. `storage-postgres`
pulls in `pgx` and nothing MySQL/Mongo-related.

## Build

The build is fully self-contained and does **not** use any globally installed
Go:

- The **Go toolchain is pinned and tree-local.** `make` first runs
  `tools/bootstrap-go.sh`, which downloads the pinned Go SDK (checksum-verified)
  into a gitignored `./.toolchain/` and points `GOROOT`/`GO` at it. Bootstrapping
  needs only `curl`, `tar`, and a sha256 tool -- no pre-existing Go.
- **Dependencies are vendored** under `vendor/` (never the global module cache),
  and `GOTOOLCHAIN=local` means no toolchain re-exec/download at build time.

```
cd helpers
make build          # bootstraps ./.toolchain (once), then builds every helper
make test           # unit tests (wire codec + protocol dispatch; no services)
make vendor         # re-resolve + re-vendor after changing deps
```

The module targets Go 1.25 (`go.mod`); the pinned version lives in
`tools/bootstrap-go.sh`. The testcontainers-based integration module (below)
reuses the same tree-local Go.

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

`-migrate` applies the schema on startup: for SQL that is idempotent
`CREATE TABLE IF NOT EXISTS` (`schema/postgres.sql`, `schema/mysql.sql`); for
Mongo it installs the `$jsonSchema` validators (`schema/mongo.js`) that enforce
field presence and BSON types (rejecting e.g. a string price -- the class of bug
a schemaless store would otherwise accept). Writes (`report_*`, `commit`) are
applied in strict arrival order;
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
| `pull_watermarks` | (none)                                 | max_id, max_rank               |
| `ping`          | (none)                                   | (type pong)                    |

`report_fill` decrements the resting order (removing it when `status=filled`)
and updates the owner's net position (`buy` +, `sell` -). `pull_levels` returns
the orders on `side` beyond `from_price` (worse than it) across at most `count`
price levels, best price first and arrival (`seq`) order within a level; the
`orders` blob is `id,price,leaves,seq` records joined by `;`. `pull_watermarks`
returns the largest order id ever reported (`max_id`, from `orders`) and the
largest resting rank (`max_rank`, from `resting`); codicis calls it once on boot
to seed its id/priority counters above anything durable, so post-restart orders
never collide with -- or jump ahead of -- restored resting orders.

## Integration tests

All eleven backends (3 storage + 8 feed) are covered by one black-box suite
under `integration/` and run with a single command:

```
make -C helpers integration        # ~60s once images are pulled
```

There is no docker-compose and no fixed host ports: the suite uses
testcontainers-go, which brings up each backing service on an ephemeral,
Docker-assigned port, so nothing collides with a host postgres/mongod/etc.
`integration/run.sh` bootstraps the tree-local Go, builds the helper binaries
the tests exec, vendors the integration deps on first run, then runs the suite
offline. It needs only a Docker daemon and curl/tar/sha256.

The tests are black-box -- they exec the built helper binary and drive it over
the wire protocol (storage) or feed it book events on stdin and subscribe to the
broker (feed), asserting against golden snapshots in `integration/testdata/`.
Extra args pass through to `go test`, e.g. run one backend:

```
./integration/run.sh -run TestStoragePostgres
```

## Feed helpers

The market-data fan-out bridges subscribe to the codicis feed-helper's TCP
stream (newline JSON: an L1 snapshot on connect, then L1 updates and any
l2/l2d/l3 query replies) and republish each per-symbol message to a broker. The
shared `internal/feed` runtime does the connect / read / reconnect loop once
(the feed is best-effort: on a dropped connection the bridge reconnects and the
helper re-snapshots); each bridge binary supplies only a `Sink`.

| Broker        | Binary          | Client (pure-Go)        | Destination                       |
| ------------- | --------------- | ----------------------- | --------------------------------- |
| Redis         | `feed-redis`    | redis/go-redis          | PUBLISH channel `<prefix><sym>`   |
| NATS          | `feed-nats`     | nats-io/nats.go         | subject `<prefix><sym>`           |
| Kafka         | `feed-kafka`    | segmentio/kafka-go      | topic `<prefix><sym>`, keyed by sym |
| RabbitMQ/AMQP | `feed-rabbitmq` | rabbitmq/amqp091-go     | topic exchange, routing `<prefix><sym>` |
| MQTT          | `feed-mqtt`     | eclipse/paho.mqtt.golang| topic `<prefix><sym>`             |
| ZeroMQ        | `feed-zeromq`   | go-zeromq/zmq4          | PUB `[<prefix><sym>, payload]`    |
| AWS SNS       | `feed-sns`      | aws-sdk-go-v2           | Publish to a topic ARN            |
| AWS SQS       | `feed-sqs`      | aws-sdk-go-v2           | SendMessage to a queue URL        |

Every client is pure-Go (no CGO / no system C libs), so each binary links only
its own driver. Run a bridge against a running feed-helper:

```
feed-redis    -feed 127.0.0.1:9100 -redis localhost:6379 -prefix md.
feed-kafka    -feed 127.0.0.1:9100 -brokers localhost:9092 -prefix md.
feed-rabbitmq -feed 127.0.0.1:9100 -amqp amqp://guest:guest@localhost:5672/
feed-sqs      -feed 127.0.0.1:9100 -queue "$SQS_URL" -endpoint "$AWS_ENDPOINT_URL"
```

The `integration/` suite covers every bridge (see "Integration tests" above): a
fake feed source repeats a fixed L1 stream, the built bridge republishes to a
testcontainers-managed broker, and the test subscribes and snapshots the
published messages. SNS/SQS use LocalStack; ZeroMQ is pure-Go and binds its own
PUB socket (no broker container). Run one bridge:

```
./integration/run.sh -run TestFeedRedis
```

## Third-party licenses

Every vendored dependency is permissively licensed and **none are GPL/LGPL**, so
vendoring and redistribution are compatible with codicis's own license:

- MIT: pgx, segmentio/kafka-go, xxhash, montanaflynn/stats, youmark/pkcs8
- Apache-2.0: mongo-driver, nats.go, aws-sdk-go-v2, xdg-go/*
- BSD-2/3: redis/go-redis, rabbitmq/amqp091-go, go-zeromq/zmq4, golang.org/x/*,
  gorilla/websocket, pierrec/lz4, snappy, klauspost/compress
- MPL-2.0 (weak, file-level; used unmodified): go-sql-driver/mysql
- EPL-2.0 (weak, file-level; used unmodified): eclipse/paho.mqtt.golang -- only
  the `feed-mqtt` binary; an operator wanting strictly-permissive-only deps can
  skip it.

Each dependency's license text is retained under `vendor/`.
